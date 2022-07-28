
typedef enum {
    ADDRESS_DESC_NONE,
    ADDRESS_DESC_GPR,
    ADDRESS_DESC_XMM,
    ADDRESS_DESC_FLAGS,
    ADDRESS_DESC_STACK,
    ADDRESS_DESC_SPILL
} AddressDescType;

typedef struct {
    uint8_t type;
    TB_DataType dt;
    union {
        GPR  gpr;
        XMM  xmm;
        Cond flags;
        int  spill;
    };
} AddressDesc;

typedef struct {
    size_t memory_usage;

    size_t locals_count;
    size_t return_count;
    size_t line_info_count;
    size_t label_patch_count;
} FunctionTallySimple;

typedef struct {
    X64_CtxHeader header;

    bool is_sysv;

    TB_Reg* use_count;
    int* ordinal;
    int register_barrier;
    GPR temp_load_reg; // sometimes we need a register to do a double-deref

    // Peephole to improve tiling
    // of memory operands:
    struct {
        TB_Reg  mapping;

        GPR     base : 8;
        GPR     index : 8;
        Scale   scale : 8;
        int32_t disp;
    } tile;

    // Register allocation:
    TB_Reg gpr_allocator[16];
    TB_Reg xmm_allocator[16];
    int gpr_available, xmm_available;

    AddressDesc addresses[];
} X64_FastCtx;

#define EITHER2(a, b, c)    ((a) == (b) || (a) == (c))
#define EITHER3(a, b, c, d) ((a) == (b) || (a) == (c) || (a) == (d))
#define FITS_INTO(a, type)  ((a) == ((type)(a)))

// a valid type that the x64 backend can eat along with
typedef struct {
    TB_DataType dt;
    uint64_t mask;
} LegalInt;

// forward declarations are a bitch
static void fast_mask_out(X64_FastCtx* restrict ctx, TB_Function* f, const LegalInt l, const Val* dst);

// returns a mask to remove the "out of bounds" bits
static LegalInt legalize_int(TB_DataType dt) {
    if (dt.type != TB_INT) return (LegalInt){ dt, 0 };

    int bits;
    if (!TB_NEXT_BIGGEST(&bits, dt.data, 8, 16, 32, 64)) {
        // support bigger types
        tb_todo();
    }

    int original_bits = dt.data;
    uint64_t mask = ~UINT64_C(0) >> (64 - original_bits);

    // we don't need the mask if it lines up nicely with machine sizes
    if (original_bits == 8 || original_bits == 16 || original_bits == 32 || original_bits == 64) {
        mask = 0;
    }

    dt.data = bits;
    return (LegalInt){ dt, mask };
}

static uint8_t legalize_float(TB_DataType dt) {
    assert(dt.type == TB_FLOAT);

    uint8_t flags = 0;
    if (dt.data == TB_FLT_64) {
        assert(dt.width == 0 || dt.width == 1);
        flags |= INST2FP_DOUBLE;
    } else if (dt.data == TB_FLT_32) {
        assert(dt.width == 0 || dt.width == 2);
    } else {
        tb_unreachable();
    }

    flags |= (dt.width ? INST2FP_PACKED : 0);
    return flags;
}

static bool is_address_node(TB_Function* f, TB_Reg r) {
    switch (f->nodes[r].type) {
        case TB_LOCAL:
        case TB_PARAM_ADDR:
        case TB_EXTERN_ADDRESS:
        case TB_GLOBAL_ADDRESS:
        case TB_ARRAY_ACCESS:
        case TB_MEMBER_ACCESS:
        return true;

        default: return false;
    }
}

static bool fits_into_int32(TB_Node* n) {
    if (n->type == TB_INTEGER_CONST &&
        n->integer.num_words == 1 &&
        FITS_INTO(n->integer.single_word, int32_t)) {
        return true;
    }

    return false;
}

static void fast_evict_gpr(X64_FastCtx* restrict ctx, TB_Function* f, GPR gpr) {
    if (ctx->gpr_allocator[gpr] == TB_TEMP_REG) {
        ctx->gpr_allocator[gpr] = TB_NULL_REG;
        ctx->gpr_available += 1;
        return;
    } else if (ctx->gpr_allocator[gpr] == TB_NULL_REG) {
        return;
    }

    // Allocate stack slot and remap value into it
    TB_Reg r = ctx->gpr_allocator[gpr];
    if (ctx->use_count[r] == 0) {
        ctx->gpr_allocator[gpr] = TB_NULL_REG;
        ctx->gpr_available += 1;
        return;
    }

    LegalInt l = legalize_int(f->nodes[r].dt);

    // printf("%s: Evicted r%d from %s\n", f->name, r, GPR_NAMES[gpr]);
    ctx->gpr_allocator[gpr] = TB_NULL_REG;
    ctx->gpr_available += 1;

    int size = get_data_type_size(l.dt);
    int pos  = STACK_ALLOC(size, size);

    ctx->addresses[r] = (AddressDesc) {
        .type = ADDRESS_DESC_SPILL, .dt = l.dt, .spill = pos
    };

    // Save out GPR into stack slot
    Val src = val_gpr(l.dt, gpr);
    Val dst = val_stack(l.dt, pos);
    INST2(MOV, &dst, &src, l.dt);
}

static void fast_evict_xmm(X64_FastCtx* restrict ctx, TB_Function* f, XMM xmm) {
    if (ctx->xmm_allocator[xmm] == TB_TEMP_REG) {
        ctx->xmm_allocator[xmm] = TB_NULL_REG;
        ctx->xmm_available += 1;
        return;
    } else if (ctx->xmm_allocator[xmm] == TB_NULL_REG) {
        return;
    }

    // Allocate stack slot and remap value into it
    TB_Reg r = ctx->xmm_allocator[xmm];
    if (ctx->use_count[r] == 0) {
        ctx->xmm_allocator[xmm] = TB_NULL_REG;
        ctx->xmm_available += 1;
        return;
    }

    TB_DataType dt = f->nodes[r].dt;

    // printf("%s: Evicted r%d from %s\n", f->name, r, GPR_NAMES[gpr]);
    ctx->xmm_allocator[xmm] = TB_NULL_REG;
    ctx->xmm_available += 1;

    int size = get_data_type_size(dt);
    int pos  = STACK_ALLOC(size, size);

    ctx->addresses[r] = (AddressDesc) { .type = ADDRESS_DESC_SPILL, .dt = dt, .spill = pos };

    // Save out XMM into stack slot
    Val src = val_xmm(dt, xmm);
    Val dst = val_stack(dt, pos);

    uint8_t flags = legalize_float(dt);
    INST2SSE(FP_MOV, &dst, &src, flags);
}

static const GPR GPR_PRIORITIES[] = {
    RAX, RCX, RDX, R8, R9, R10, R11, RDI, RSI, RBX, R12, R13, R14, R15
};

static GPR fast_alloc_gpr(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r) {
    assert(ctx->gpr_available > 0);

    loop(i, COUNTOF(GPR_PRIORITIES)) {
        GPR gpr = GPR_PRIORITIES[i];

        if (ctx->gpr_allocator[gpr] == TB_NULL_REG) {
            ctx->gpr_allocator[gpr] = r;
            ctx->gpr_available -= 1;

            // mark register as to be saved
            ctx->header.regs_to_save |=
            (1u << gpr) & (ctx->is_sysv ? SYSV_ABI_CALLEE_SAVED : WIN64_ABI_CALLEE_SAVED);

            return gpr;
        }
    }

    tb_unreachable();
    return GPR_NONE;
}

static XMM fast_alloc_xmm(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r) {
    assert(ctx->xmm_available > 0);

    loop(xmm, 16) {
        if (ctx->xmm_allocator[xmm] == TB_NULL_REG) {
            ctx->xmm_allocator[xmm] = r;
            ctx->xmm_available -= 1;

            // callee saves
            if (!ctx->is_sysv && xmm > 5) { ctx->header.regs_to_save |= (1u << (16 + xmm)); }

            return xmm;
        }
    }

    // spilling
    tb_todo();
    return XMM_NONE;
}

static void fast_kill_temp_gpr(X64_FastCtx* restrict ctx, TB_Function* f, GPR gpr) {
    if (ctx->gpr_allocator[gpr] == TB_TEMP_REG) {
        ctx->gpr_allocator[gpr] = TB_NULL_REG;
        ctx->gpr_available += 1;
    }
}

static void fast_kill_temp_xmm(X64_FastCtx* restrict ctx, TB_Function* f, XMM xmm) {
    if (ctx->xmm_allocator[xmm] == TB_TEMP_REG) {
        ctx->xmm_allocator[xmm] = TB_NULL_REG;
        ctx->xmm_available += 1;
    }
}

static void fast_def_gpr(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r, GPR gpr, TB_DataType dt) {
    ctx->addresses[r] = (AddressDesc) { .type = ADDRESS_DESC_GPR, .dt = dt, .gpr = gpr };
}

static void fast_def_xmm(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r, XMM xmm, TB_DataType dt) {
    ctx->addresses[r] = (AddressDesc) { .type = ADDRESS_DESC_XMM, .dt = dt, .xmm = xmm };
}

static void fast_def_spill(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r, int spill, TB_DataType dt) {
    ctx->addresses[r] = (AddressDesc) { .type = ADDRESS_DESC_SPILL, .dt = dt, .spill = spill };
}

static void fast_def_stack(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r, int spill, TB_DataType dt) {
    ctx->addresses[r] = (AddressDesc) { .type = ADDRESS_DESC_STACK, .dt = dt, .spill = spill };
}

static void fast_def_flags(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r, Cond cc, TB_DataType dt) {
    ctx->addresses[r] = (AddressDesc) { .type = ADDRESS_DESC_FLAGS, .dt = dt, .flags = cc };
}

static void fast_kill_reg(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r) {
    if (ctx->use_count[r] == 0) {
        if (ctx->addresses[r].type == ADDRESS_DESC_GPR) {
            GPR gpr = ctx->addresses[r].gpr;

            assert(ctx->gpr_allocator[gpr] == r || ctx->gpr_allocator[gpr] == TB_TEMP_REG);
            ctx->gpr_allocator[gpr] = TB_NULL_REG;
            ctx->gpr_available += 1;
        } else if (ctx->addresses[r].type == ADDRESS_DESC_XMM) {
            XMM xmm = ctx->addresses[r].xmm;

            assert(ctx->xmm_allocator[xmm] == r || ctx->xmm_allocator[xmm] == TB_TEMP_REG);
            ctx->xmm_allocator[xmm] = TB_NULL_REG;
            ctx->xmm_available += 1;
        }

        ctx->addresses[r].type = ADDRESS_DESC_NONE;
    }
}

static Val fast_eval(X64_FastCtx* ctx, TB_Function* f, TB_Reg r) {
    TB_Node* restrict n = &f->nodes[r];
    TB_DataType dt = n->dt;

    ctx->use_count[r] -= 1;
    if (ctx->addresses[r].type) {
        switch (ctx->addresses[r].type) {
            case ADDRESS_DESC_GPR:
            assert(ctx->addresses[r].dt.width == 0);
            return val_gpr(dt, ctx->addresses[r].gpr);

            case ADDRESS_DESC_XMM:
            return val_xmm(dt, ctx->addresses[r].xmm);

            case ADDRESS_DESC_STACK:
            case ADDRESS_DESC_SPILL:
            return (Val) {
                .type = VAL_MEM,
                .dt = dt,
                .is_spill = (ctx->addresses[r].type == ADDRESS_DESC_SPILL),
                .mem = {
                    .base = RBP,
                    .index = GPR_NONE,
                    .scale = SCALE_X1,
                    .disp = ctx->addresses[r].spill
                }
            };

            case ADDRESS_DESC_FLAGS:
            return val_flags(ctx->addresses[r].flags);

            default: break;
        }
    } else {
        if (fits_into_int32(n)) {
            return val_imm(n->dt, n->integer.single_word);
        } else if (n->type == TB_GLOBAL_ADDRESS) {
            TB_Module* m = f->module;
            const TB_Global* g = n->global.value;

            if (g->storage == TB_STORAGE_TLS) {
                if (m->tls_index_extern == 0) {
                    tb_panic("TB error: no tls_index provided\n");
                }

                // since t0 dies before dst is allocated we just recycle it
                // mov t0, dword    [_tls_index]
                Val dst = val_gpr(TB_TYPE_PTR, fast_alloc_gpr(ctx, f, r));
                if (dst.gpr >= 8) *ctx->header.out++ = 0x41;
                *ctx->header.out++ = 0x8B;
                *ctx->header.out++ = ((dst.gpr & 7) << 3) | RBP;
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;
                tb_emit_ecall_patch(f->module, f, m->tls_index_extern, GET_CODE_POS() - 4, s_local_thread_id);

                // mov t1, qword gs:[58h]
                Val t1 = val_gpr(TB_TYPE_PTR, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
                *ctx->header.out++ = 0x65;
                *ctx->header.out++ = t1.gpr >= 8 ? 0x49 : 0x48;
                *ctx->header.out++ = 0x8B;
                *ctx->header.out++ = mod_rx_rm(MOD_INDIRECT, t1.gpr, RSP);
                *ctx->header.out++ = mod_rx_rm(SCALE_X1, RSP, RBP);
                *ctx->header.out++ = 0x58;
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;

                // mov t1, qword    [t1+t0*8]
                Val mem = val_base_index(TB_TYPE_PTR, t1.gpr, dst.gpr, SCALE_X8);
                INST2(MOV, &t1, &mem, TB_TYPE_I64);

                // lea addr,        [t1+relocation]
                *ctx->header.out++ = rex(true, dst.gpr, RBP, 0);
                *ctx->header.out++ = 0x8D;
                if ((t1.gpr & 7) == RSP) {
                    *ctx->header.out++ = mod_rx_rm(MOD_INDIRECT_DISP32, dst.gpr, RSP);
                    *ctx->header.out++ = mod_rx_rm(SCALE_X1, RSP, t1.gpr);
                } else {
                    *ctx->header.out++ = mod_rx_rm(MOD_INDIRECT_DISP32, dst.gpr, t1.gpr);
                }
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;
                *ctx->header.out++ = 0x00;
                tb_emit_global_patch(f->module, f, GET_CODE_POS() - 4, n->global.value, s_local_thread_id);

                fast_def_gpr(ctx, f, r, dst.gpr, TB_TYPE_PTR);
                fast_kill_temp_gpr(ctx, f, t1.gpr);
                return dst;
            } else {
                return val_global(n->global.value);
            }
        }
    }

    tb_function_print(f, tb_default_print_callback, stderr);
    fprintf(stderr, "error: could not eval r%u\n", r);

    tb_unreachable();
    return (Val) { 0 };
}

// OP lhs, eval(rhs)
static void fast_folded_op(X64_FastCtx* ctx, TB_Function* f, Inst2Type op, const Val* lhs, TB_Reg rhs_reg) {
    Val rhs = fast_eval(ctx, f, rhs_reg);

    TB_Node* restrict n = &f->nodes[rhs_reg];
    LegalInt l = legalize_int(op == MOVSXD ? TB_TYPE_I64 : n->dt);
    //assert(l.mask == 0 && "TODO");

    if (!rhs.is_spill && is_value_mem(&rhs) && n->type != TB_LOAD) {
        Val tmp = val_gpr(TB_TYPE_PTR, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
        if (is_value_mem(lhs)) {
            INST2(LEA, &tmp, &rhs, l.dt);
        } else {
            if (rhs.type == VAL_MEM && rhs.mem.index == GPR_NONE && rhs.mem.disp == 0) {
                // lea rcx, [rdx] => mov rcx, rdx
                Val base = val_gpr(TB_TYPE_PTR, rhs.mem.base);
                INST2(MOV, &tmp, &base, l.dt);
            } else {
                INST2(LEA, &tmp, &rhs, l.dt);
            }
        }

        INST2(op, lhs, &tmp, l.dt);
        fast_kill_temp_gpr(ctx, f, tmp.gpr);
    } else if (is_value_mem(lhs) && is_value_mem(&rhs)) {
        Val tmp = val_gpr(TB_TYPE_PTR, fast_alloc_gpr(ctx, f, TB_TEMP_REG));

        INST2(MOV, &tmp, &rhs, l.dt);
        INST2(op, lhs, &tmp, l.dt);

        fast_kill_temp_gpr(ctx, f, tmp.gpr);
    } else if (rhs.type == VAL_IMM && inst2_tbl[op].op_i == 0 && inst2_tbl[op].rx_i == 0) {
        // doesn't support immediates
        Val tmp = val_gpr(TB_TYPE_I32, fast_alloc_gpr(ctx, f, TB_TEMP_REG));

        INST2(MOV, &tmp, &rhs, l.dt);
        INST2(op, lhs, &tmp, l.dt);

        fast_kill_temp_gpr(ctx, f, tmp.gpr);
    } else if (rhs.type != VAL_GPR || (rhs.type == VAL_GPR && !is_value_gpr(lhs, rhs.gpr))) {
        INST2(op, lhs, &rhs, l.dt);
    }

    if (l.mask && !(op == MOV && rhs.type == VAL_IMM && (rhs.imm & l.mask) == rhs.imm)) {
        fast_mask_out(ctx, f, l, lhs);
    }
}

// OP lhs, eval(rhs)
static void fast_folded_op_sse(X64_FastCtx* ctx, TB_Function* f, Inst2FPType op, const Val* lhs, TB_Reg rhs_reg) {
    Val rhs = fast_eval(ctx, f, rhs_reg);

    TB_Node* restrict n = &f->nodes[rhs_reg];
    TB_DataType dt = n->dt;

    uint8_t flags = legalize_float(dt);
    if (is_value_mem(lhs) && is_value_mem(&rhs)) {
        Val tmp = val_xmm(TB_TYPE_VOID, fast_alloc_xmm(ctx, f, TB_TEMP_REG));

        INST2SSE(op, &tmp, &rhs, flags);
        INST2SSE(FP_MOV, lhs, &tmp, flags);

        fast_kill_temp_xmm(ctx, f, tmp.xmm);
    } else {
        INST2SSE(op, lhs, &rhs, flags);
    }
}

// (eval(src) != 0) ? 1 : 0
static Cond fast_eval_cond(X64_FastCtx* ctx, TB_Function* f, TB_Reg src_reg) {
    Val src = fast_eval(ctx, f, src_reg);

    TB_Node* restrict n = &f->nodes[src_reg];
    LegalInt l = legalize_int(n->dt);
    //assert(l.mask == 0);

    if (!src.is_spill && is_value_mem(&src) && n->type != TB_LOAD) {
        Val tmp = val_gpr(TB_TYPE_PTR, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
        INST2(LEA, &tmp, &src, l.dt);

        // early-kill: this is fine here because no allocations are made
        // between here and the end of the function (the time it actually
        // should be killed)
        fast_kill_temp_gpr(ctx, f, tmp.gpr);
    }

    if (is_value_mem(&src)) {
        Val imm = val_imm(TB_TYPE_I32, 0);
        INST2(CMP, &src, &imm, l.dt);
        return NE;
    } else if (src.type == VAL_GPR) {
        INST2(TEST, &src, &src, l.dt);
        return NE;
    } else if (src.type == VAL_IMM) {
        Val tmp = val_gpr(TB_TYPE_I32, fast_alloc_gpr(ctx, f, TB_TEMP_REG));

        // 'xor a, a' will set ZF to 1
        INST2(XOR, &tmp, &tmp, l.dt);
        fast_kill_temp_gpr(ctx, f, tmp.gpr);

        return (src.imm ? E : NE);
    } else if (src.type == VAL_FLAGS) {
        return src.cond;
    } else {
        tb_todo();
        return NE;
    }
}

static Val fast_eval_address(X64_FastCtx* ctx, TB_Function* f, TB_Reg r) {
    Val address = fast_eval(ctx, f, r);

    TB_Node* restrict n = &f->nodes[r];
    TB_DataType dt = n->dt;

    if (address.type == VAL_GPR) {
        return val_base_disp(TB_TYPE_PTR, address.gpr, 0);
    } else if (is_value_mem(&address) && address.is_spill) {
        // reload
        Val tmp = val_gpr(dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
        ctx->temp_load_reg = tmp.gpr;
        INST2(MOV, &tmp, &address, dt);

        return val_base_disp(dt, tmp.gpr, 0);
    } else {
        return address;
    }
}

static void fast_mask_out(X64_FastCtx* restrict ctx, TB_Function* f, const LegalInt l, const Val* dst) {
    if (l.mask == (int32_t)l.mask) {
        Val mask = val_imm(l.dt, l.mask);
        INST2(AND, dst, &mask, l.dt);
    } else {
        Val tmp = val_gpr(l.dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));

        // MOVABS     REX.W B8+r imm64
        *ctx->header.out++ = tmp.gpr >= 8 ? 0x49 : 0x48;
        *ctx->header.out++ = 0xB8 + (tmp.gpr & 7);
        *((uint64_t*)ctx->header.out) = l.mask;
        ctx->header.out += 8;

        INST2(AND, dst, &tmp, l.dt);

        fast_kill_temp_gpr(ctx, f, tmp.gpr);
    }
}

// you can read, we at least need the src to be either a GPR or i32
static void fast_memset_const_size(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg addr, const Val* src, size_t sz, bool allow_8byte_set) {
    Val dst = fast_eval_address(ctx, f, addr);
    assert(is_value_mem(&dst));

    if (allow_8byte_set) {
        for (; sz >= 8; sz -= 8, dst.mem.disp += 8) {
            INST2(MOV, &dst, src, TB_TYPE_I64);
        }
    }

    for (; sz >= 4; sz -= 4, dst.mem.disp += 4) {
        INST2(MOV, &dst, src, TB_TYPE_I32);
    }

    for (; sz >= 2; sz -= 2, dst.mem.disp += 2) {
        INST2(MOV, &dst, src, TB_TYPE_I16);
    }

    for (; sz >= 1; sz -= 1, dst.mem.disp += 1) {
        INST2(MOV, &dst, src, TB_TYPE_I8);
    }
}

static Val fast_get_tile_mapping(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg r) {
    assert(ctx->tile.mapping == r);
    // printf("TILE USED UP! r%u\n", r);
    ctx->tile.mapping = 0;

    return (Val) { VAL_MEM,
        .mem = {
            .base = ctx->tile.base,
            .index = ctx->tile.index,
            .scale = ctx->tile.scale,
            .disp  = ctx->tile.disp
        }
    };
}

static void fast_evict_everything(X64_FastCtx* restrict ctx, TB_Function* f) {
    #if 1
    loop(i, COUNTOF(GPR_PRIORITIES)) {
        GPR gpr = GPR_PRIORITIES[i];

        if (ctx->gpr_allocator[gpr] != TB_NULL_REG) {
            // eviction notice lmao
            fast_evict_gpr(ctx, f, gpr);
        }
    }

    loop(xmm, 16) {
        if (ctx->xmm_allocator[xmm] != TB_NULL_REG) {
            // eviction notice lmao
            fast_evict_xmm(ctx, f, xmm);
        }
    }
    #endif
}

static void fast_spill_tile(X64_FastCtx* restrict ctx, TB_Function* f) {
    Val src = {
        VAL_MEM,
        .mem = {
            .base = ctx->tile.base,
            .index = ctx->tile.index,
            .scale = ctx->tile.scale,
            .disp  = ctx->tile.disp
        }
    };

    GPR dst_gpr;
    if (ctx->gpr_allocator[ctx->tile.base] == ctx->tile.mapping) {
        dst_gpr = ctx->tile.base;
    } else if (ctx->tile.index != GPR_NONE && ctx->gpr_allocator[ctx->tile.index] == ctx->tile.mapping) {
        dst_gpr = ctx->tile.index;
    } else {
        dst_gpr = fast_alloc_gpr(ctx, f, ctx->tile.mapping);
    }

    fast_def_gpr(ctx, f, ctx->tile.mapping, dst_gpr, TB_TYPE_PTR);

    Val dst = val_gpr(TB_TYPE_PTR, dst_gpr);
    INST2(LEA, &dst, &src, TB_TYPE_PTR);

    // printf("%s:r%d: failed to tile value :(\n", f->name, ctx->tile.mapping);
    // printf("TILE FAILURE! r%u\n", ctx->tile.mapping);
    ctx->tile.mapping = 0;
}

static void fast_eval_basic_block(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg bb, TB_Reg bb_end) {
    // first node in the basic block
    bb = f->nodes[bb].next;
    if (bb == bb_end) return;

    TB_FOR_EACH_NODE_RANGE(n, f, bb, bb_end) {
        TB_Reg r = n - f->nodes;

        TB_Node* restrict n = &f->nodes[r];
        TB_NodeTypeEnum reg_type = n->type;
        TB_DataType dt = n->dt;

        // spilling
        if (ctx->gpr_available < 4) {
            int barrier = ctx->register_barrier;

            loop(i, COUNTOF(GPR_PRIORITIES)) {
                GPR gpr = GPR_PRIORITIES[i];

                if (ctx->gpr_allocator[gpr] != TB_NULL_REG &&
                    ctx->gpr_allocator[gpr] != TB_TEMP_REG &&
                    ctx->ordinal[ctx->gpr_allocator[gpr]] < barrier) {
                    assert(ctx->gpr_allocator[gpr] != r);

                    // eviction notice lmao
                    fast_evict_gpr(ctx, f, gpr);
                    if (ctx->gpr_available >= 4) break;
                }
            }
        } else if (ctx->xmm_available < 4) {
            int barrier = ctx->register_barrier;

            loop(xmm, 16) {
                if (ctx->xmm_allocator[xmm] != TB_NULL_REG &&
                    ctx->xmm_allocator[xmm] != TB_TEMP_REG &&
                    ctx->ordinal[ctx->xmm_allocator[xmm]] < barrier) {
                    assert(ctx->xmm_allocator[xmm] != r);

                    // eviction notice lmao
                    fast_evict_xmm(ctx, f, xmm);
                    if (ctx->xmm_available >= 4) break;
                }
            }
        }

        #if 0
        printf("r%d:\tXMM = { ", r);
        loop(i, 16) {
            printf("XMM%zu:", i);
            if (ctx->xmm_allocator[i] == TB_TEMP_REG) printf("R    ");
            else printf("r%-3d ", ctx->xmm_allocator[i]);
        }
        printf("}\n");
        #endif

        // memory operand tiling
        if (ctx->tile.mapping) {
            bool can_keep_it = false;
            if (ctx->use_count[r] <= 1 && ctx->use_count[ctx->tile.mapping] <= 1) {
                if (reg_type == TB_LOAD && n->load.address == ctx->tile.mapping) {
                    can_keep_it = true;
                } else if (reg_type == TB_STORE && n->store.address == ctx->tile.mapping) {
                    can_keep_it = true;
                }
            }

            if (reg_type == TB_SIGN_EXT) {
                TB_Reg potential_load = n->unary.src;
                if (f->nodes[potential_load].type == TB_LOAD &&
                    f->nodes[potential_load].load.address == ctx->tile.mapping &&
                    ctx->use_count[potential_load] == 1) {
                    can_keep_it = true;
                }
            }

            if (ctx->tile.base == RBP && ctx->tile.index == GPR_NONE) {
                // it's a RBP relative... it's constant so we good
                ctx->addresses[ctx->tile.mapping] = (AddressDesc){
                    .type = ADDRESS_DESC_STACK,
                    .dt = TB_TYPE_PTR,
                    .spill = ctx->tile.disp
                };

                ctx->tile.mapping = 0;
                can_keep_it = true;
            }

            if (!can_keep_it) {
                fast_spill_tile(ctx, f);
            }
        }

        switch (reg_type) {
            case TB_NULL:
            case TB_PARAM:
            case TB_PHI1:
            case TB_PHI2:
            case TB_PHIN:
            case TB_GLOBAL_ADDRESS:
            case TB_PARAM_ADDR:
            case TB_LOCAL:
            break;
            case TB_EXTERN_ADDRESS:
            case TB_FUNC_ADDRESS: {
                GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                fast_def_gpr(ctx, f, r, dst_gpr, TB_TYPE_PTR);

                *ctx->header.out++ = rex(true, dst_gpr, RBP, 0);
                *ctx->header.out++ = 0x8D;
                *ctx->header.out++ = mod_rx_rm(MOD_INDIRECT, dst_gpr, RBP);
                *((uint32_t*)ctx->header.out) = 0;
                ctx->header.out += 4;

                if (reg_type == TB_EXTERN_ADDRESS) {
                    tb_emit_ecall_patch(f->module, f, n->external.value, GET_CODE_POS() - 4, s_local_thread_id);
                } else {
                    tb_emit_call_patch(f->module, f, n->func.value, GET_CODE_POS() - 4, s_local_thread_id);
                }
                break;
            }
            case TB_INTEGER_CONST:
            if (!fits_into_int32(n)) {
                assert(dt.type == TB_PTR || (dt.type == TB_INT && dt.data <= 64));

                GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                fast_def_gpr(ctx, f, r, dst_gpr, TB_TYPE_PTR);

                *ctx->header.out++ = dst_gpr >= 8 ? 0x49 : 0x48;
                *ctx->header.out++ = 0xB8 + (dst_gpr & 7);
                *((uint64_t*)ctx->header.out) = n->integer.single_word;
                ctx->header.out += 8;
            }
            break;
            case TB_FLOAT_CONST: {
                assert(dt.type == TB_FLOAT && dt.width == 0);
                uint64_t imm = (Cvt_F64U64) { .f = n->flt.value }.i;

                XMM dst_xmm = fast_alloc_xmm(ctx, f, r);
                fast_def_xmm(ctx, f, r, dst_xmm, TB_TYPE_PTR);

                if (imm == 0) {
                    if (dst_xmm >= 8) {
                        *ctx->header.out++ = rex(true, dst_xmm, dst_xmm, 0);
                    }
                    *ctx->header.out++ = 0x0F;
                    *ctx->header.out++ = 0x57;
                    *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, dst_xmm, dst_xmm);
                } else {
                    // Convert it to raw bits
                    *ctx->header.out++ = dt.data == TB_FLT_64 ? 0xF2 : 0xF3;
                    if (dst_xmm >= 8) *ctx->header.out++ = 0x44;
                    *ctx->header.out++ = 0x0F;
                    *ctx->header.out++ = 0x10;
                    *ctx->header.out++ = ((dst_xmm & 7) << 3) | RBP;

                    uint32_t disp = 0;
                    if (dt.data == TB_FLT_64) {
                        uint64_t* rdata_payload = tb_platform_arena_alloc(sizeof(uint64_t));
                        *rdata_payload = imm;

                        disp = tb_emit_const_patch(f->module, f, GET_CODE_POS(), rdata_payload, sizeof(uint64_t), s_local_thread_id);
                    } else if (dt.data == TB_FLT_32) {
                        uint32_t imm32 = (Cvt_F32U32) { .f = n->flt.value }.i;

                        uint32_t* rdata_payload = tb_platform_arena_alloc(sizeof(uint32_t));
                        *rdata_payload = imm32;

                        disp = tb_emit_const_patch(f->module, f, GET_CODE_POS(), rdata_payload, sizeof(uint32_t), s_local_thread_id);
                    } else {
                        tb_unreachable();
                    }

                    *((uint32_t*)ctx->header.out) = disp;
                    ctx->header.out += 4;
                }
                break;
            }
            case TB_STRING_CONST: {
                const char* str = n->string.data;
                size_t len = n->string.length;

                GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                fast_def_gpr(ctx, f, r, dst_gpr, TB_TYPE_PTR);

                *ctx->header.out++ = rex(true, dst_gpr, RBP, 0);
                *ctx->header.out++ = 0x8D;
                *ctx->header.out++ = mod_rx_rm(MOD_INDIRECT, dst_gpr, RBP);

                uint32_t disp = tb_emit_const_patch(f->module, f, GET_CODE_POS(), str, len, s_local_thread_id);

                *((uint32_t*)ctx->header.out) = disp;
                ctx->header.out += 4;
                break;
            }

            case TB_LINE_INFO: {
                f->lines[f->line_count++] = (TB_Line) {
                    .file = n->line_info.file, .line = n->line_info.line, .pos = GET_CODE_POS()
                };
                break;
            }

            case TB_DEBUGBREAK: {
                *ctx->header.out++ = 0xCC;
                break;
            }

            case TB_VA_START: {
                assert(!ctx->is_sysv && "How does va_start even work on SysV?");

                // on Win64 va_start just means whatever is one parameter away from
                // the parameter you give it (plus in Win64 the parameters in the stack
                // are 8bytes, no fanciness like in SysV):
                // void printf(const char* fmt, ...) {
                //     va_list args;
                //     va_start(args, fmt); // args = (char*) (((uintptr_t) &fmt) + 8);
                //     ...
                // }
                Val addr = fast_eval_address(ctx, f, n->unary.src);
                assert(addr.type == VAL_MEM);
                addr.mem.disp += 8;

                GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                fast_def_gpr(ctx, f, r, dst_gpr, TB_TYPE_PTR);

                Val dst = val_gpr(dt, dst_gpr);
                INST2(LEA, &dst, &addr, dt);
                break;
            }

            case TB_MEMBER_ACCESS: {
                Val addr = fast_eval_address(ctx, f, n->member_access.base);

                if (addr.type == VAL_MEM) {
                    assert(ctx->tile.mapping == 0);
                    addr.mem.disp += n->member_access.offset;

                    ctx->tile.mapping = r;
                    ctx->tile.base    = addr.mem.base;
                    ctx->tile.index   = addr.mem.index;
                    ctx->tile.scale   = addr.mem.scale;
                    ctx->tile.disp    = addr.mem.disp;
                } else if (addr.type == VAL_GLOBAL) {
                    addr.global.disp += n->member_access.offset;

                    GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                    fast_def_gpr(ctx, f, r, dst_gpr, TB_TYPE_PTR);

                    Val dst = val_gpr(dt, dst_gpr);
                    INST2(LEA, &dst, &addr, dt);
                } else {
                    tb_unreachable();
                }
                break;
            }
            case TB_ARRAY_ACCESS: {
                // it's called fast isel for a reason and it's definetely not
                // because of the codegen quality...
                uint32_t stride = n->array_access.stride;

                Val val;
                if (ctx->use_count[n->array_access.index] == 1 &&
                    ctx->addresses[n->array_access.index].type == ADDRESS_DESC_GPR) {
                    val = fast_eval(ctx, f, n->array_access.index);
                } else {
                    val = val_gpr(TB_TYPE_PTR, fast_alloc_gpr(ctx, f, r));
                    fast_folded_op(ctx, f, MOV, &val, n->array_access.index);
                }

                // if it's an LEA index*stride
                // then stride > 0, if not it's free
                // do think of it however
                GPR index_reg = val.gpr;
                uint8_t stride_as_shift = 0;

                if (tb_is_power_of_two(stride)) {
                    stride_as_shift = tb_ffs(stride) - 1;

                    if (stride_as_shift > 3) {
                        assert(stride_as_shift < 64 && "Stride to big!!!");

                        // shl index, stride_as_shift
                        *ctx->header.out++ = rex(true, 0, val.gpr, 0);
                        *ctx->header.out++ = 0xC1;
                        *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0x04, val.gpr);
                        *ctx->header.out++ = stride_as_shift;

                        stride_as_shift = 0; // pre-multiplied, don't propagate
                    }
                } else {
                    // imul dst, index, stride
                    *ctx->header.out++ = rex(true, val.gpr, val.gpr, 0);
                    *ctx->header.out++ = 0x69;
                    *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, val.gpr, val.gpr);

                    *((uint32_t*)ctx->header.out) = stride;
                    ctx->header.out += 4;

                    stride_as_shift = 0; // pre-multiplied, don't propagate
                }

                // post conditions :)
                assert(index_reg != GPR_NONE);
                assert(stride_as_shift >= 0 && stride_as_shift <= 3 &&
                    "stride_as_shift can't fit into an LEA");

                // Resolve base (if it's not already in a register)
                if (stride_as_shift) {
                    if (ctx->use_count[n->array_access.base] == 1 &&
                        ctx->addresses[n->array_access.base].type == ADDRESS_DESC_GPR) {
                        Val src = fast_eval(ctx, f, n->array_access.base);

                        assert(ctx->tile.mapping == 0);
                        ctx->tile.mapping = r;
                        ctx->tile.base    = src.gpr;
                        ctx->tile.index   = index_reg;
                        ctx->tile.scale   = stride_as_shift;
                        ctx->tile.disp    = 0;
                    } else {
                        Val temp = val_gpr(TB_TYPE_PTR, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
                        fast_folded_op(ctx, f, MOV, &temp, n->array_access.base);

                        assert(ctx->tile.mapping == 0);
                        ctx->tile.mapping = r;
                        ctx->tile.base    = temp.gpr;
                        ctx->tile.index   = index_reg;
                        ctx->tile.scale   = stride_as_shift;
                        ctx->tile.disp    = 0;

                        // fast_kill_temp_gpr(ctx, f, temp.gpr);
                    }
                } else {
                    fast_folded_op(ctx, f, ADD, &val, n->array_access.base);
                    fast_def_gpr(ctx, f, r, val.gpr, TB_TYPE_PTR);
                }

                assert(val.type == VAL_GPR);

                // move ownership
                ctx->gpr_allocator[val.gpr] = r;
                fast_kill_reg(ctx, f, n->array_access.base);
                break;
            }
            case TB_LOAD: {
                Val addr;
                if (ctx->tile.mapping == n->load.address) {
                    // if we can defer the LOAD into a SIGN_EXT that's kinda better
                    if (f->nodes[n->next].type == TB_SIGN_EXT &&
                        f->nodes[n->next].unary.src == r) {
                        break;
                    }
                    addr = fast_get_tile_mapping(ctx, f, n->load.address);
                } else {
                    addr = fast_eval_address(ctx, f, n->load.address);
                }

                if (TB_IS_FLOAT_TYPE(dt) || dt.width) {
                    Val dst = val_xmm(dt, fast_alloc_xmm(ctx, f, r));
                    fast_def_xmm(ctx, f, r, dst.xmm, dt);

                    uint8_t flags = legalize_float(dt);
                    INST2SSE(FP_MOV, &dst, &addr, flags);
                } else {
                    LegalInt l = legalize_int(dt);

                    GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                    fast_def_gpr(ctx, f, r, dst_gpr, l.dt);

                    Val dst = val_gpr(dt, dst_gpr);
                    INST2(MOV, &dst, &addr, l.dt);

                    if (l.mask) fast_mask_out(ctx, f, l, &dst);
                }

                fast_kill_reg(ctx, f, n->load.address);
                break;
            }
            case TB_STORE: {
                Val addr;
                if (ctx->tile.mapping == n->store.address) {
                    addr = fast_get_tile_mapping(ctx, f, n->store.address);
                } else {
                    addr = fast_eval_address(ctx, f, n->store.address);
                }

                if (dt.width || TB_IS_FLOAT_TYPE(dt)) {
                    fast_folded_op_sse(ctx, f, FP_MOV, &addr, n->store.value);
                } else {
                    fast_folded_op(ctx, f, MOV, &addr, n->store.value);
                }
                break;
            }
            case TB_INITIALIZE: {
                TB_Reg addr = n->mem_op.dst;

                TB_Initializer* i = n->init.src;

                assert(i->obj_count == 0);
                Val src = val_imm(TB_TYPE_I32, 0);
                fast_memset_const_size(ctx, f, addr, &src, i->size, true);

                fast_kill_reg(ctx, f, addr);
                break;
            }
            case TB_MEMSET: {
                TB_Reg dst_reg  = n->mem_op.dst;
                TB_Reg val_reg  = n->mem_op.src;
                TB_Reg size_reg = n->mem_op.size;

                // memset on constant size
                if (f->nodes[size_reg].type == TB_INTEGER_CONST &&
                    f->nodes[size_reg].integer.num_words == 1) {
                    int64_t sz = f->nodes[size_reg].integer.single_word;
                    assert(sz > 0 && "Cannot memset on negative numbers or zero");

                    {
                        LegalInt l = legalize_int(dt);
                        Val src = val_gpr(dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));

                        // convert byte into pattern
                        //  XY
                        //  vv
                        //  XYXYXYXY
                        INST2(XOR, &src, &src, TB_TYPE_I32);

                        if (!tb_node_is_constant_zero(f, val_reg)) {
                            fast_folded_op(ctx, f, MOV, &src, val_reg);
                            if (l.mask) fast_mask_out(ctx, f, l, &src);

                            // imul dst, index, 0x10101010
                            *ctx->header.out++ = rex(true, src.gpr, src.gpr, 0);
                            *ctx->header.out++ = 0x69;
                            *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, src.gpr, src.gpr);

                            *((uint32_t*)ctx->header.out) = 0x10101010;
                            ctx->header.out += 4;

                            fast_memset_const_size(ctx, f, dst_reg, &src, sz, false);
                        } else {
                            fast_memset_const_size(ctx, f, dst_reg, &src, sz, true);
                        }

                        fast_kill_temp_gpr(ctx, f, src.gpr);
                    }

                    assert(dst_reg != val_reg);
                    assert(dst_reg != size_reg);
                    assert(val_reg != size_reg);
                    fast_kill_reg(ctx, f, dst_reg);
                    fast_kill_reg(ctx, f, val_reg);
                    fast_kill_reg(ctx, f, size_reg);
                    break;
                }

                // rep stosb, ol' reliable
                fast_evict_gpr(ctx, f, RAX);
                fast_evict_gpr(ctx, f, RCX);
                fast_evict_gpr(ctx, f, RDI);

                {
                    Val param = val_gpr(dt, RAX);
                    fast_folded_op(ctx, f, MOV, &param, val_reg);
                    ctx->gpr_allocator[RAX] = TB_TEMP_REG;
                    ctx->gpr_available -= 1;
                }

                {
                    Val param = val_gpr(dt, RDI);
                    fast_folded_op(ctx, f, MOV, &param, dst_reg);
                    ctx->gpr_allocator[RDI] = TB_TEMP_REG;
                    ctx->gpr_available -= 1;
                }

                {
                    Val param = val_gpr(dt, RCX);
                    fast_folded_op(ctx, f, MOV, &param, size_reg);
                    ctx->gpr_allocator[RCX] = TB_TEMP_REG;
                    ctx->gpr_available -= 1;
                }

                // rep stosb
                *ctx->header.out++ = 0xF3;
                *ctx->header.out++ = 0xAA;

                // free up stuff
                ctx->gpr_allocator[RAX] = TB_NULL_REG;
                ctx->gpr_allocator[RCX] = TB_NULL_REG;
                ctx->gpr_allocator[RDI] = TB_NULL_REG;
                ctx->gpr_available += 3;
                break;
            }
            case TB_MEMCPY: {
                TB_Reg dst_reg  = n->mem_op.dst;
                TB_Reg src_reg  = n->mem_op.src;
                TB_Reg size_reg = n->mem_op.size;

                // rep stosb, ol' reliable
                fast_evict_gpr(ctx, f, RDI);
                fast_evict_gpr(ctx, f, RSI);
                fast_evict_gpr(ctx, f, RCX);

                {
                    Val param = val_gpr(dt, RDI);
                    fast_folded_op(ctx, f, MOV, &param, dst_reg);
                    ctx->gpr_allocator[RDI] = TB_TEMP_REG;
                    ctx->gpr_available -= 1;
                }

                {
                    Val param = val_gpr(dt, RSI);
                    fast_folded_op(ctx, f, MOV, &param, src_reg);
                    ctx->gpr_allocator[RSI] = TB_TEMP_REG;
                    ctx->gpr_available -= 1;
                }

                {
                    Val param = val_gpr(dt, RCX);
                    fast_folded_op(ctx, f, MOV, &param, size_reg);
                    ctx->gpr_allocator[RCX] = TB_TEMP_REG;
                    ctx->gpr_available -= 1;
                }

                // rep movsb
                *ctx->header.out++ = 0xF3;
                *ctx->header.out++ = 0xA4;

                // free up stuff
                ctx->gpr_allocator[RDI] = TB_NULL_REG;
                ctx->gpr_allocator[RSI] = TB_NULL_REG;
                ctx->gpr_allocator[RCX] = TB_NULL_REG;
                ctx->gpr_available += 3;
                break;
            }

            // Integer binary operations
            case TB_AND:
            case TB_OR:
            case TB_XOR:
            case TB_ADD:
            case TB_SUB:
            case TB_MUL: {
                // simple scalar ops
                const static Inst2Type ops[] = { AND, OR, XOR, ADD, SUB, IMUL };

                Val dst;
                if (ctx->use_count[n->i_arith.a] == 1 && ctx->addresses[n->i_arith.a].type == ADDRESS_DESC_GPR) {
                    dst = val_gpr(dt, ctx->addresses[n->i_arith.a].gpr);
                    fast_def_gpr(ctx, f, r, dst.gpr, dt);

                    // rename a -> dst
                    ctx->gpr_allocator[dst.gpr] = r;
                    fast_folded_op(ctx, f, ops[reg_type - TB_AND], &dst, n->i_arith.b);

                    if (n->i_arith.a != n->i_arith.b) {
                        fast_kill_reg(ctx, f, n->i_arith.b);
                    }
                } else {
                    GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                    fast_def_gpr(ctx, f, r, dst_gpr, dt);

                    dst = val_gpr(dt, dst_gpr);
                    fast_folded_op(ctx, f, MOV, &dst, n->i_arith.a);
                    fast_folded_op(ctx, f, ops[reg_type - TB_AND], &dst, n->i_arith.b);

                    if (n->i_arith.a == n->i_arith.b) {
                        fast_kill_reg(ctx, f, n->i_arith.a);
                    } else {
                        fast_kill_reg(ctx, f, n->i_arith.a);
                        fast_kill_reg(ctx, f, n->i_arith.b);
                    }
                }
                break;
            }
            case TB_UDIV:
            case TB_SDIV:
            case TB_UMOD:
            case TB_SMOD: {
                assert(dt.width == 0 && "TODO: Implement vector integer division and modulo");

                bool is_signed = (reg_type == TB_SDIV || reg_type == TB_SMOD);
                bool is_div    = (reg_type == TB_UDIV || reg_type == TB_SDIV);

                fast_evict_gpr(ctx, f, RAX);
                fast_evict_gpr(ctx, f, RDX);

                ctx->gpr_allocator[RAX] = TB_TEMP_REG;
                ctx->gpr_allocator[RDX] = TB_TEMP_REG;
                ctx->gpr_available -= 2;

                LegalInt l = legalize_int(dt);

                // MOV rax, a
                Val rax = val_gpr(l.dt, RAX);
                fast_folded_op(ctx, f, MOV, &rax, n->i_arith.a);

                if (is_signed) {
                    // cqo/cdq
                    if (dt.type == TB_PTR || (dt.type == TB_INT && l.dt.data == 64)) {
                        *ctx->header.out++ = 0x48;
                    }
                    *ctx->header.out++ = 0x99;
                } else {
                    // xor rdx, rdx
                    *ctx->header.out++ = 0x31;
                    *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, RDX, RDX);
                }

                {
                    Val tmp = val_gpr(l.dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));

                    fast_folded_op(ctx, f, MOV, &tmp, n->i_arith.b);
                    INST1(IDIV, &tmp);

                    fast_kill_temp_gpr(ctx, f, tmp.gpr);
                }

                if (n->i_arith.a == n->i_arith.b) {
                    fast_kill_reg(ctx, f, n->i_arith.a);
                } else {
                    fast_kill_reg(ctx, f, n->i_arith.a);
                    fast_kill_reg(ctx, f, n->i_arith.b);
                }

                // the return value is in RAX for division
                // and RDX for modulo
                fast_def_gpr(ctx, f, r, is_div ? RAX : RDX, l.dt);

                if (l.mask) {
                    Val dst = val_gpr(l.dt, is_div ? RAX : RDX);
                    fast_mask_out(ctx, f, l, &dst);
                }

                // free the other piece of the divmod result
                ctx->gpr_allocator[is_div ? RAX : RDX] = r;
                ctx->gpr_allocator[is_div ? RDX : RAX] = TB_NULL_REG;
                ctx->gpr_available += 1;
                break;
            }
            case TB_SHR:
            case TB_SHL:
            case TB_SAR: {
                LegalInt l = legalize_int(dt);
                int bits_in_type = l.dt.type == TB_PTR ? 64 : l.dt.data;

                if (f->nodes[n->i_arith.b].type == TB_INTEGER_CONST &&
                    f->nodes[n->i_arith.b].integer.num_words == 1) {
                    uint64_t imm = f->nodes[n->i_arith.b].integer.single_word;
                    assert(imm < 64);

                    Val dst = val_gpr(l.dt, fast_alloc_gpr(ctx, f, r));
                    fast_def_gpr(ctx, f, r, dst.gpr, l.dt);
                    fast_folded_op(ctx, f, MOV, &dst, n->i_arith.a);

                    // C1 /4       shl r/m, imm
                    // C1 /5       shr r/m, imm
                    // C1 /7       sar r/m, imm
                    if (bits_in_type == 16) *ctx->header.out++ = 0x66;
                    *ctx->header.out++ = rex(bits_in_type == 64, 0x00, dst.gpr, 0x00);
                    *ctx->header.out++ = (bits_in_type == 8 ? 0xC0 : 0xC1);
                    switch (reg_type) {
                        case TB_SHL: *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0x04, dst.gpr); break;
                        case TB_SHR: *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0x05, dst.gpr); break;
                        case TB_SAR: *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0x07, dst.gpr); break;
                        default: tb_unreachable();
                    }
                    *ctx->header.out++ = imm;

                    if (l.mask) fast_mask_out(ctx, f, l, &dst);

                    fast_kill_reg(ctx, f, n->i_arith.a);
                    fast_kill_reg(ctx, f, n->i_arith.b);
                    break;
                }

                // we'll be using this bad boy
                fast_evict_gpr(ctx, f, RCX);
                ctx->gpr_allocator[RCX] = TB_TEMP_REG;
                ctx->gpr_available -= 1;

                Val dst = val_gpr(l.dt, fast_alloc_gpr(ctx, f, r));
                fast_def_gpr(ctx, f, r, dst.gpr, l.dt);
                fast_folded_op(ctx, f, MOV, &dst, n->i_arith.a);

                // MOV rcx, b
                Val rcx = val_gpr(dt, RCX);
                fast_folded_op(ctx, f, MOV, &rcx, n->i_arith.b);

                // D2 /4       shl r/m, cl
                // D2 /5       shr r/m, cl
                // D2 /7       sar r/m, cl
                if (bits_in_type == 16) *ctx->header.out++ = 0x66;
                *ctx->header.out++ = rex(bits_in_type == 64, 0x00, dst.gpr, 0x00);
                *ctx->header.out++ = (bits_in_type == 8 ? 0xD2 : 0xD3);
                switch (reg_type) {
                    case TB_SHL: *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0x04, dst.gpr); break;
                    case TB_SHR: *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0x05, dst.gpr); break;
                    case TB_SAR: *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0x07, dst.gpr); break;
                    default: tb_unreachable();
                }

                if (l.mask) fast_mask_out(ctx, f, l, &dst);

                // free up RCX
                ctx->gpr_allocator[RCX] = TB_NULL_REG;
                ctx->gpr_available += 1;

                fast_kill_reg(ctx, f, n->i_arith.a);
                fast_kill_reg(ctx, f, n->i_arith.b);
                break;
            }

            // Float binary operators
            case TB_FADD:
            case TB_FSUB:
            case TB_FMUL:
            case TB_FDIV: {
                // simple scalar ops
                const static Inst2FPType ops[] = { FP_ADD, FP_SUB, FP_MUL, FP_DIV };

                if (ctx->use_count[n->f_arith.a] == 1 &&
                    ctx->addresses[n->f_arith.a].type == ADDRESS_DESC_XMM) {
                    // recycle a for the destination
                    Val dst = val_xmm(dt, ctx->addresses[n->f_arith.a].xmm);

                    // move ownership
                    ctx->xmm_allocator[dst.xmm] = r;

                    fast_def_xmm(ctx, f, r, dst.xmm, dt);
                    fast_folded_op_sse(ctx, f, ops[reg_type - TB_FADD], &dst, n->f_arith.b);

                    if (n->f_arith.a != n->f_arith.b) {
                        fast_kill_reg(ctx, f, n->f_arith.b);
                    }
                } else {
                    Val dst = val_xmm(dt, fast_alloc_xmm(ctx, f, r));
                    fast_def_xmm(ctx, f, r, dst.xmm, dt);

                    fast_folded_op_sse(ctx, f, FP_MOV, &dst, n->f_arith.a);
                    fast_folded_op_sse(ctx, f, ops[reg_type - TB_FADD], &dst, n->f_arith.b);

                    if (n->f_arith.a == n->f_arith.b) {
                        fast_kill_reg(ctx, f, n->f_arith.a);
                    } else {
                        fast_kill_reg(ctx, f, n->f_arith.a);
                        fast_kill_reg(ctx, f, n->f_arith.b);
                    }
                }
                break;
            }

            case TB_CMP_EQ:
            case TB_CMP_NE:
            case TB_CMP_SLT:
            case TB_CMP_SLE:
            case TB_CMP_ULT:
            case TB_CMP_ULE:
            case TB_CMP_FLT:
            case TB_CMP_FLE: {
                TB_DataType cmp_dt = n->cmp.dt;
                assert(cmp_dt.width == 0 && "TODO: Implement vector compares");

                // TODO(NeGate): add some simple const folding here... maybe?
                // if (cmp XX (a, b)) should return a FLAGS because the IF
                // will handle it properly
                bool returns_flags = ctx->use_count[r] == 1 &&
                    f->nodes[n->next].type == TB_IF &&
                    f->nodes[n->next].if_.cond == r;

                Val val = { 0 };
                if (!returns_flags) {
                    val = val_gpr(TB_TYPE_I8, fast_alloc_gpr(ctx, f, r));
                    fast_def_gpr(ctx, f, r, val.gpr, dt);

                    // xor temp, temp
                    if (val.gpr >= 8) {
                        *ctx->header.out++ = rex(false, val.gpr, val.gpr, 0);
                    }
                    *ctx->header.out++ = 0x31;
                    *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, val.gpr, val.gpr);
                }

                Cond cc;
                if (TB_IS_FLOAT_TYPE(cmp_dt)) {
                    Val compare_tmp = val_xmm(cmp_dt, fast_alloc_xmm(ctx, f, TB_TEMP_REG));

                    fast_folded_op_sse(ctx, f, FP_MOV, &compare_tmp, n->i_arith.a);
                    fast_folded_op_sse(ctx, f, FP_UCOMI, &compare_tmp, n->i_arith.b);

                    switch (reg_type) {
                        case TB_CMP_EQ:  cc = E; break;
                        case TB_CMP_NE:  cc = NE; break;
                        case TB_CMP_FLT: cc = B; break;
                        case TB_CMP_FLE: cc = BE; break;
                        default: tb_unreachable();
                    }

                    fast_kill_temp_xmm(ctx, f, compare_tmp.xmm);
                } else {
                    cmp_dt = legalize_int(cmp_dt).dt;

                    bool invert = false;
                    TB_Reg lhs = n->i_arith.a, rhs = n->i_arith.b;
                    if (f->nodes[n->i_arith.a].type == TB_INTEGER_CONST) {
                        tb_swap(TB_Reg, lhs, rhs);
                        invert = true;
                    }

                    if (ctx->use_count[lhs] == 1 &&
                        ctx->addresses[lhs].type == ADDRESS_DESC_GPR) {
                        Val val = val_gpr(cmp_dt, ctx->addresses[lhs].gpr);
                        fast_folded_op(ctx, f, CMP, &val, rhs);
                    } else {
                        Val temp = val_gpr(cmp_dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));

                        fast_folded_op(ctx, f, MOV, &temp, lhs);
                        fast_folded_op(ctx, f, CMP, &temp, rhs);

                        fast_kill_temp_gpr(ctx, f, temp.gpr);
                    }

                    switch (reg_type) {
                        case TB_CMP_EQ: cc = E; break;
                        case TB_CMP_NE: cc = NE; break;
                        case TB_CMP_SLT: cc = invert ? G : L; break;
                        case TB_CMP_SLE: cc = invert ? GE : LE; break;
                        case TB_CMP_ULT: cc = invert ? A : B; break;
                        case TB_CMP_ULE: cc = invert ? NB : BE; break;
                        default: tb_unreachable();
                    }
                }

                if (!returns_flags) {
                    // printf("r%d: setcc -> %s\n", r, GPR_NAMES[val.gpr]);

                    // setcc v
                    assert(val.type == VAL_GPR);
                    *ctx->header.out++ = (val.gpr >= 8) ? 0x41 : 0x40;
                    *ctx->header.out++ = 0x0F;
                    *ctx->header.out++ = 0x90 + cc;
                    *ctx->header.out++ = mod_rx_rm(MOD_DIRECT, 0, val.gpr);
                } else {
                    fast_def_flags(ctx, f, r, cc, TB_TYPE_BOOL);
                }

                if (n->cmp.a == n->cmp.b) {
                    fast_kill_reg(ctx, f, n->cmp.a);
                } else {
                    fast_kill_reg(ctx, f, n->cmp.a);
                    fast_kill_reg(ctx, f, n->cmp.b);
                }
                break;
            }

            case TB_BITCAST: {
                assert(dt.width == 0 && "TODO: Implement vector bitcast");

                Val src = fast_eval(ctx, f, n->unary.src);
                assert(get_data_type_size(dt) == get_data_type_size(src.dt));

                bool is_src_int = src.dt.type == TB_INT || src.dt.type == TB_PTR;
                bool is_dst_int = dt.type == TB_INT || dt.type == TB_PTR;

                if (is_src_int == is_dst_int) {
                    // just doesn't do anything really
                    tb_todo();
                } else {
                    int bits_in_type = 64;
                    if (dt.type == TB_INT) bits_in_type = dt.data;
                    else if (src.dt.type == TB_INT) bits_in_type = src.dt.data;

                    // movd/q
                    *ctx->header.out++ = 0x66;

                    Val val;
                    bool is_64bit = bits_in_type > 32;
                    bool int2float = is_src_int && !is_dst_int;
                    if (int2float) {
                        // int -> float
                        assert(src.type == VAL_GPR || src.type == VAL_MEM);
                        val = val_xmm(dt, fast_alloc_xmm(ctx, f, r));
                        fast_def_xmm(ctx, f, r, val.xmm, dt);
                    } else {
                        // float -> int
                        assert(src.type == VAL_XMM || src.type == VAL_MEM);
                        val = val_gpr(dt, fast_alloc_gpr(ctx, f, r));
                        fast_def_gpr(ctx, f, r, val.gpr, dt);
                    }

                    bool src_needs_rex = false;
                    if (src.type == VAL_GPR || src.type == VAL_XMM) {
                        src_needs_rex = (src.gpr >= 8);
                    } else if (src.type == VAL_MEM) {
                        // index isn't required
                        if (src.mem.index != GPR_NONE) {
                            src_needs_rex |= (src.mem.index >= 8);
                        }
                        src_needs_rex |= (src.mem.base >= 8);
                    }

                    if (is_64bit || val.xmm >= 8 || src_needs_rex) {
                        *ctx->header.out++ = rex(is_64bit, src.gpr, val.gpr, 0);
                    }

                    *ctx->header.out++ = 0x0F;
                    *ctx->header.out++ = int2float ? 0x7E : 0x6E;

                    // val.gpr and val.xmm alias so it's irrelevant which one we pick
                    emit_memory_operand(&ctx->header, val.gpr, &src);

                    fast_kill_reg(ctx, f, n->unary.src);
                }
                break;
            }
            case TB_FLOAT2INT:
            case TB_FLOAT2UINT: {
                assert(dt.width == 0 && "TODO: Implement vector float2int");

                TB_DataType src_dt = f->nodes[n->unary.src].dt;
                assert(src_dt.type == TB_FLOAT);

                Val src = val_xmm(src_dt, fast_alloc_xmm(ctx, f, TB_TEMP_REG));
                fast_folded_op_sse(ctx, f, FP_MOV, &src, n->unary.src);

                assert(src.type == VAL_MEM || src.type == VAL_GLOBAL || src.type == VAL_XMM);
                Val val = val_gpr(dt, fast_alloc_gpr(ctx, f, r));
                fast_def_gpr(ctx, f, r, val.gpr, dt);

                // it's either 32bit or 64bit conversion
                // F3 0F 2D /r            CVTSS2SI xmm1, r/m32
                // F3 REX.W 0F 2D /r      CVTSS2SI xmm1, r/m64
                // F2 0F 2D /r            CVTSD2SI xmm1, r/m32
                // F2 REX.W 0F 2D /r      CVTSD2SI xmm1, r/m64
                if (src.dt.width == 0) {
                    *ctx->header.out++ = (src.dt.data == TB_FLT_64) ? 0xF2 : 0xF3;
                } else if (src.dt.data == TB_FLT_64) {
                    // packed double
                    *ctx->header.out++ = 0x66;
                }

                uint8_t rx = val.gpr;
                uint8_t base, index;
                if (src.type == VAL_MEM) {
                    base = src.mem.base;
                    index = src.mem.index != GPR_NONE ? src.mem.index : 0;
                } else if (src.type == VAL_XMM) {
                    base = src.xmm;
                    index = 0;
                } else tb_todo();

                bool is_64bit = (dt.data > 32 || reg_type == TB_FLOAT2UINT);
                if (is_64bit || rx >= 8 || base >= 8 || index >= 8) {
                    *ctx->header.out++ = rex(is_64bit, rx, base, index);
                }

                *ctx->header.out++ = 0x0F;
                *ctx->header.out++ = 0x2D;
                emit_memory_operand(&ctx->header, rx, &src);

                fast_kill_temp_gpr(ctx, f, src.gpr);
                fast_kill_reg(ctx, f, n->unary.src);
                break;
            }
            case TB_UINT2FLOAT:
            case TB_INT2FLOAT: {
                assert(dt.width == 0 && "TODO: Implement vector int2float");
                TB_DataType src_dt = f->nodes[n->unary.src].dt;

                Val src = val_gpr(src_dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
                fast_folded_op(ctx, f, MOV, &src, n->unary.src);

                assert(src.type == VAL_MEM || src.type == VAL_GLOBAL || src.type == VAL_GPR);
                Val val = val_xmm(dt, fast_alloc_xmm(ctx, f, r));
                fast_def_xmm(ctx, f, r, val.xmm, dt);

                if (reg_type == TB_UINT2FLOAT && dt.data <= 32) {
                    // zero extend 32bit value to 64bit
                    INST2(MOV, &src, &src, TB_TYPE_I32);
                }

                // it's either 32bit or 64bit conversion
                // F3       0F 2A /r      CVTSI2SS xmm1, r/m32
                // F3 REX.W 0F 2A /r      CVTSI2SS xmm1, r/m64
                // F2       0F 2A /r      CVTSI2SD xmm1, r/m32
                // F2 REX.W 0F 2A /r      CVTSI2SD xmm1, r/m64
                if (dt.width == 0) {
                    *ctx->header.out++ = (dt.data == TB_FLT_64) ? 0xF2 : 0xF3;
                } else if (dt.data == TB_FLT_64) {
                    // packed double
                    *ctx->header.out++ = 0x66;
                }

                uint8_t rx = val.xmm;
                uint8_t base, index;
                if (src.type == VAL_MEM) {
                    base  = src.mem.base;
                    index = src.mem.index != GPR_NONE ? src.mem.index : 0;
                } else if (src.type == VAL_GPR) {
                    base  = src.gpr;
                    index = 0;
                } else tb_unreachable();

                bool is_64bit = (dt.data > 32 || reg_type == TB_UINT2FLOAT);
                if (is_64bit || rx >= 8 || base >= 8 || index >= 8) {
                    *ctx->header.out++ = rex(is_64bit, rx, base, index);
                }

                *ctx->header.out++ = 0x0F;
                *ctx->header.out++ = 0x2A;
                emit_memory_operand(&ctx->header, rx, &src);

                fast_kill_temp_gpr(ctx, f, src.gpr);
                fast_kill_reg(ctx, f, n->unary.src);
                break;
            }
            // realistically TRUNCATE doesn't need to do shit on integers :p
            case TB_TRUNCATE: {
                assert(dt.width == 0 && "TODO: Implement vector truncate");

                if (TB_IS_FLOAT_TYPE(dt)) {
                    Val src = fast_eval(ctx, f, n->unary.src);

                    Val val = val_xmm(dt, fast_alloc_xmm(ctx, f, r));
                    fast_def_xmm(ctx, f, r, val.xmm, dt);

                    uint8_t flags = legalize_float(src.dt);
                    INST2SSE(FP_CVT, &val, &src, flags);
                } else {
                    // we probably want some recycling eventually...
                    Val val = val_gpr(dt, fast_alloc_gpr(ctx, f, r));
                    fast_def_gpr(ctx, f, r, val.gpr, dt);

                    fast_folded_op(ctx, f, MOV, &val, n->unary.src);
                }
                fast_kill_reg(ctx, f, n->unary.src);
                break;
            }
            case TB_NOT:
            case TB_NEG: {
                assert(dt.width == 0 && "TODO: Implement vector negate");
                bool is_not = reg_type == TB_NOT;

                if (TB_IS_FLOAT_TYPE(dt)) {
                    assert(!is_not && "TODO");

                    // .LCPI0_0:
                    //   .quad   0x8000000000000000
                    //   .quad   0x8000000000000000
                    // ...
                    // xorps   xmm0, xmmword ptr [rip + .LCPI0_0]
                    XMM dst_xmm = fast_alloc_xmm(ctx, f, r);
                    Val val = val_xmm(dt, dst_xmm);
                    fast_def_xmm(ctx, f, r, dst_xmm, dt);

                    fast_folded_op_sse(ctx, f, FP_MOV, &val, n->unary.src);

                    if (dst_xmm >= 8) {
                        *ctx->header.out++ = rex(true, dst_xmm, dst_xmm, 0);
                        *ctx->header.out++ = (dt.data == TB_FLT_64 ? 0xF2 : 0xF3);
                    }
                    *ctx->header.out++ = 0x0F;
                    *ctx->header.out++ = 0x57;
                    *ctx->header.out++ = ((dst_xmm & 7) << 3) | RBP;

                    void* payload = NULL;
                    if (dt.data == TB_FLT_64) {
                        uint64_t* rdata_payload = tb_platform_arena_alloc(2 * sizeof(uint64_t));
                        rdata_payload[0] = (1ull << 63ull);
                        rdata_payload[1] = (1ull << 63ull);
                        payload = rdata_payload;
                    } else {
                        uint32_t* rdata_payload = tb_platform_arena_alloc(4 * sizeof(uint32_t));
                        rdata_payload[0] = (1ull << 31ull);
                        rdata_payload[1] = (1ull << 31ull);
                        rdata_payload[2] = (1ull << 31ull);
                        rdata_payload[3] = (1ull << 31ull);
                        payload = rdata_payload;
                    }

                    uint32_t disp = tb_emit_const_patch(f->module, f, GET_CODE_POS(),
                        payload, 2 * sizeof(uint64_t), s_local_thread_id);

                    *((uint32_t*) ctx->header.out) = disp;
                    ctx->header.out += 4;
                } else {
                    // we probably want some recycling eventually...
                    GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                    fast_def_gpr(ctx, f, r, dst_gpr, dt);
                    Val val = val_gpr(dt, dst_gpr);

                    fast_folded_op(ctx, f, MOV, &val, n->unary.src);
                    INST1(is_not ? NOT : NEG, &val);
                }

                fast_kill_reg(ctx, f, n->unary.src);
                break;
            }
            case TB_PTR2INT: {
                assert(dt.width == 0 && "TODO: Implement vector zero extend");
                // TB_DataType src_dt = f->nodes[n->unary.src].dt;
                // bool sign_ext = (reg_type == TB_SIGN_EXT);

                GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                fast_def_gpr(ctx, f, r, dst_gpr, dt);
                Val val = val_gpr(dt, dst_gpr);

                // make sure to zero it out if it's not a 64bit integer
                if (dt.data < 64) {
                    INST2(XOR, &val, &val, TB_TYPE_I32);
                }

                fast_folded_op(ctx, f, MOV, &val, n->unary.src);
                fast_kill_reg(ctx, f, n->unary.src);
                break;
            }
            case TB_INT2PTR:
            case TB_SIGN_EXT:
            case TB_ZERO_EXT: {
                assert(dt.width == 0 && "TODO: Implement vector zero extend");
                TB_DataType src_dt = f->nodes[n->unary.src].dt;
                bool sign_ext = (reg_type == TB_SIGN_EXT);

                // Figure out if we can do it trivially
                int bits;
                if (!TB_NEXT_BIGGEST(&bits, dt.data, 8, 16, 32, 64)) {
                    // support bigger types
                    tb_todo();
                }

                // means we're using the MOV, MOVSX or MOVZX
                Inst2Type op = MOV;

                // figure out if we can use the cool instructions
                // or if we gotta emulate it like a bitch
                LegalInt l = legalize_int(src_dt);
                int bits_in_type = l.dt.type == TB_PTR ? 64 : l.dt.data;

                if (reg_type == TB_ZERO_EXT && bits_in_type >= 32 &&
                    ctx->use_count[n->unary.src] == 1 &&
                    ctx->addresses[n->unary.src].type == ADDRESS_DESC_GPR) {
                    Val src = fast_eval(ctx, f, n->unary.src);

                    // move ownership
                    ctx->gpr_allocator[src.gpr] = r;
                    fast_def_gpr(ctx, f, r, src.gpr, dt);
                } else {
                    if (bits_in_type == 64) {
                        op = MOV;
                    } else if (bits_in_type == 32) {
                        op = (sign_ext ? MOVSXD : MOV);
                    } else if (bits_in_type == 16) {
                        op = (sign_ext ? MOVSXW : MOVZXW);
                    } else if (bits_in_type == 8) {
                        op = (sign_ext ? MOVSXB : MOVZXB);
                    } else if (bits_in_type == 1) {
                        op = MOVZXB;
                    } else {
                        tb_todo();
                    }

                    GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                    fast_def_gpr(ctx, f, r, dst_gpr, dt);
                    Val val = val_gpr(dt, dst_gpr);

                    TB_Reg src = n->unary.src;
                    if (f->nodes[src].type == TB_LOAD &&
                        f->nodes[src].load.address == ctx->tile.mapping) {
                        Val addr = fast_get_tile_mapping(ctx, f, ctx->tile.mapping);

                        INST2(op, &val, &addr, dt);
                    } else {
                        fast_folded_op(ctx, f, op, &val, n->unary.src);
                    }

                    if (l.mask) fast_mask_out(ctx, f, l, &val);
                    fast_kill_reg(ctx, f, n->unary.src);
                }
                break;
            }
            case TB_FLOAT_EXT: {
                Val src = fast_eval(ctx, f, n->unary.src);

                Val val = val_xmm(dt, fast_alloc_xmm(ctx, f, r));
                fast_def_xmm(ctx, f, r, val.xmm, dt);

                uint8_t flags = legalize_float(src.dt);
                if (!TB_DATA_TYPE_EQUALS(src.dt, dt)) {
                    INST2SSE(FP_CVT, &val, &src, flags);
                } else {
                    INST2SSE(FP_MOV, &val, &src, flags);
                }

                fast_kill_reg(ctx, f, n->unary.src);
                break;
            }

            case TB_CALL:
            case TB_ECALL:
            case TB_VCALL: {
                int param_start = n->call.param_start;
                int param_count = n->call.param_end - n->call.param_start;

                // Evict the GPRs that are caller saved
                uint16_t caller_saved = (ctx->is_sysv ? SYSV_ABI_CALLER_SAVED : WIN64_ABI_CALLER_SAVED);
                const GPR* parameter_gprs = ctx->is_sysv ? SYSV_GPR_PARAMETERS : WIN64_GPR_PARAMETERS;

                // evaluate parameters
                loop(j, param_count) {
                    TB_Reg      param_reg = f->vla.data[param_start + j];
                    TB_DataType param_dt  = f->nodes[param_reg].dt;

                    if (TB_IS_FLOAT_TYPE(param_dt) || param_dt.width) {
                        if (j < 4) {
                            // since we evict now we don't need to later
                            fast_evict_xmm(ctx, f, j);

                            Val dst = val_xmm(param_dt, j);

                            // move to parameter XMM and reserve it
                            fast_folded_op_sse(ctx, f, FP_MOV, &dst, param_reg);
                        } else {
                            Val dst = val_base_disp(param_dt, RSP, 8 * j);
                            fast_folded_op(ctx, f, MOV, &dst, param_reg);
                        }
                    } else {
                        // Win64 has 4 GPR parameters (RCX, RDX, R8, R9)
                        // SysV has 6 of them (RDI, RSI, RDX, RCX, R8, R9)
                        if ((ctx->is_sysv && j < 6) || j < 4) {
                            // don't evict if the guy in the slot is based
                            if (ctx->gpr_allocator[parameter_gprs[j]] != param_reg) {
                                // since we evict now we don't need to later
                                fast_evict_gpr(ctx, f, parameter_gprs[j]);
                                caller_saved &= ~(1u << parameter_gprs[j]);
                            }

                            Val dst = val_gpr(param_dt, parameter_gprs[j]);

                            // move to parameter GPR and reserve it
                            fast_folded_op(ctx, f, MOV, &dst, param_reg);
                        } else {
                            Val dst = val_base_disp(param_dt, RSP, 8 * j);
                            fast_folded_op(ctx, f, MOV, &dst, param_reg);
                        }
                    }

                    fast_kill_reg(ctx, f, param_reg);

                    if (TB_IS_FLOAT_TYPE(param_dt) || param_dt.width) {
                        if (j < 4) {
                            if (ctx->xmm_allocator[j] == 0) ctx->xmm_available -= 1;
                            ctx->xmm_allocator[j] = TB_TEMP_REG;
                        }
                    } else {
                        if ((ctx->is_sysv && j < 6) || j < 4) {
                            if (ctx->gpr_allocator[parameter_gprs[j]] == 0) ctx->gpr_available -= 1;
                            ctx->gpr_allocator[parameter_gprs[j]] = TB_TEMP_REG;
                        }
                    }
                }

                // Spill anything else
                loop(j, 16) if (caller_saved & (1u << j)) { fast_evict_gpr(ctx, f, j); }

                // TODO(NeGate): Evict the XMMs that are caller saved
                loop_range(j, ctx->is_sysv ? 0 : 5, 16) {
                    fast_evict_xmm(ctx, f, j);
                }

                // reserve return value
                if (ctx->is_sysv && (TB_IS_FLOAT_TYPE(dt) || dt.width)) {
                    // evict XMM0
                    fast_evict_xmm(ctx, f, XMM0);
                }

                // CALL instruction and patch
                if (reg_type == TB_CALL) {
                    const TB_Function* target = n->call.target;
                    tb_emit_call_patch(f->module, f, target, GET_CODE_POS() + 1, s_local_thread_id);

                    // CALL rel32
                    ctx->header.out[0] = 0xE8;
                    *((uint32_t*)&ctx->header.out[1]) = 0x0;
                    ctx->header.out += 5;
                } else if (reg_type == TB_ECALL) {
                    const TB_External* target = n->ecall.target;

                    tb_emit_ecall_patch(f->module, f, target, GET_CODE_POS() + 1, s_local_thread_id);

                    // CALL rel32
                    ctx->header.out[0] = 0xE8;
                    *((uint32_t*)&ctx->header.out[1]) = 0x0;
                    ctx->header.out += 5;
                } else if (reg_type == TB_VCALL) {
                    Val target = fast_eval_address(ctx, f, n->vcall.target);

                    // call r/m64
                    assert(target.type == VAL_MEM && target.mem.index == GPR_NONE && target.mem.disp == 0);
                    target = val_gpr(TB_TYPE_PTR, target.mem.base);
                    INST1(CALL_RM, &target);

                    fast_kill_reg(ctx, f, n->vcall.target);
                }

                // get rid of all those reserved TEMP_REGs
                loop(i, 16) if (ctx->gpr_allocator[i] == TB_TEMP_REG) {
                    ctx->gpr_allocator[i] = TB_NULL_REG;
                    ctx->gpr_available += 1;
                }
                loop(i, 16) if (ctx->xmm_allocator[i] == TB_TEMP_REG) {
                    ctx->xmm_allocator[i] = TB_NULL_REG;
                    ctx->xmm_available += 1;
                }

                // the return value
                if (dt.width || TB_IS_FLOAT_TYPE(dt)) {
                    if (ctx->xmm_allocator[XMM0] == 0) ctx->xmm_available -= 1;
                    ctx->xmm_allocator[XMM0] = r;
                    fast_def_xmm(ctx, f, r, XMM0, dt);
                } else {
                    int bits_in_type = dt.type == TB_PTR ? 8 : dt.data;

                    if (bits_in_type > 0) {
                        if (ctx->gpr_allocator[RAX] == 0) ctx->gpr_available -= 1;
                        ctx->gpr_allocator[RAX] = r;

                        fast_def_gpr(ctx, f, r, RAX, dt);
                    }
                }
                break;
            }

            case TB_ATOMIC_TEST_AND_SET: {
                assert(0 && "Atomic flag test & set not supported yet.");
                break;
            }
            case TB_ATOMIC_CLEAR: {
                assert(0 && "Atomic flag clear not supported yet.");
                break;
            }
            case TB_ATOMIC_LOAD: {
                Val addr;
                if (ctx->tile.mapping == n->atomic.addr) {
                    // if we can defer the LOAD into a SIGN_EXT that's kinda better
                    if (f->nodes[n->next].type == TB_SIGN_EXT &&
                        f->nodes[n->next].unary.src == r) {
                        break;
                    }
                    addr = fast_get_tile_mapping(ctx, f, n->atomic.addr);
                } else {
                    addr = fast_eval_address(ctx, f, n->atomic.addr);
                }

                if (TB_IS_FLOAT_TYPE(dt) || dt.width) {
                    Val dst = val_xmm(dt, fast_alloc_xmm(ctx, f, r));
                    fast_def_xmm(ctx, f, r, dst.xmm, dt);

                    uint8_t flags = legalize_float(dt);
                    INST2SSE(FP_MOV, &dst, &addr, flags);
                } else {
                    LegalInt l = legalize_int(dt);

                    GPR dst_gpr = fast_alloc_gpr(ctx, f, r);
                    fast_def_gpr(ctx, f, r, dst_gpr, l.dt);

                    Val dst = val_gpr(dt, dst_gpr);
                    INST2(MOV, &dst, &addr, l.dt);

                    if (l.mask) fast_mask_out(ctx, f, l, &dst);
                }

                fast_kill_reg(ctx, f, n->atomic.addr);
                break;
            }
            case TB_ATOMIC_XCHG:
            case TB_ATOMIC_ADD:
            case TB_ATOMIC_SUB:
            case TB_ATOMIC_AND:
            case TB_ATOMIC_XOR:
            case TB_ATOMIC_OR: {
                const static int tbl[]       = { MOV, ADD, SUB, AND, XOR, OR };
                const static int fetch_tbl[] = { XCHG, XADD, XADD, 0, 0, 0 };

                Val addr;
                if (ctx->tile.mapping == n->atomic.addr) {
                    addr = fast_get_tile_mapping(ctx, f, n->atomic.addr);
                } else {
                    addr = fast_eval_address(ctx, f, n->atomic.addr);
                }

                // sometimes we only need to do the operation atomic without
                // a fetch, then things get... fancy
                if (ctx->use_count[r]) {
                    if (reg_type == TB_ATOMIC_XOR || reg_type == TB_ATOMIC_OR ||
                        reg_type == TB_ATOMIC_AND) {
                        assert(0 && "TODO: Atomic operations with fetch.");
                        break;
                    }
                }

                LegalInt l = legalize_int(dt);

                Val tmp;
                if (ctx->use_count[r] == 0) {
                    tmp = val_gpr(l.dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
                } else {
                    tmp = val_gpr(l.dt, fast_alloc_gpr(ctx, f, r));
                    fast_def_gpr(ctx, f, r, tmp.gpr, l.dt);
                }
                fast_folded_op(ctx, f, MOV, &tmp, n->atomic.src);

                if (l.mask) fast_mask_out(ctx, f, l, &tmp);

                if (ctx->use_count[r] && reg_type == TB_ATOMIC_SUB) {
                    assert(l.mask == 0);

                    // there's no atomic_fetch_sub in x64, we just negate
                    // the src
                    INST1(NEG, &tmp);
                }

                // LOCK prefix is not needed on XCHG because
                // it's actually a MOV which is naturally atomic
                // when aligned.
                if (reg_type != TB_ATOMIC_XCHG) {
                    *ctx->header.out++ = 0xF0;
                }

                int op = (ctx->use_count[r] ? fetch_tbl : tbl)[reg_type - TB_ATOMIC_XCHG];
                INST2(op, &addr, &tmp, l.dt);
                if (ctx->use_count[r] == 0) {
                    fast_kill_temp_gpr(ctx, f, tmp.gpr);
                }

                if (n->atomic.addr == n->atomic.src) {
                    fast_kill_reg(ctx, f, n->atomic.addr);
                } else {
                    fast_kill_reg(ctx, f, n->atomic.addr);
                    fast_kill_reg(ctx, f, n->atomic.src);
                }
                break;
            }
            case TB_ATOMIC_CMPXCHG: {
                tb_assume(f->nodes[n->next].type == TB_ATOMIC_CMPXCHG2);
                if (ctx->use_count[n->next] != 0) {
                    tb_function_print(f, tb_default_print_callback, stdout);
                }
                tb_assume(ctx->use_count[n->next] == 0);

                // we'll be using RAX for CMPXCHG crap
                fast_evict_gpr(ctx, f, RAX);
                ctx->gpr_allocator[RAX] = TB_TEMP_REG;
                ctx->gpr_available -= 1;

                TB_Reg expected = n->atomic.src;
                TB_Reg desired  = f->nodes[n->next].atomic.src;

                Val addr;
                if (ctx->tile.mapping == n->atomic.addr) {
                    addr = fast_get_tile_mapping(ctx, f, n->atomic.addr);
                } else {
                    addr = fast_eval_address(ctx, f, n->atomic.addr);
                }

                LegalInt l = legalize_int(dt);
                int bits_in_type = l.dt.type == TB_PTR ? 64 : l.dt.data;

                // mov tmpgpr, desired
                Val desired_val = val_gpr(l.dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
                fast_folded_op(ctx, f, MOV, &desired_val, desired);

                // mov RAX, expected
                Val rax = val_gpr(l.dt, RAX);
                fast_folded_op(ctx, f, MOV, &rax, expected);

                // LOCK CMPXCHG
                bool is_64bit = (bits_in_type > 32 || reg_type == TB_FLOAT2UINT);

                assert(is_value_mem(&addr));
                if (addr.type == VAL_MEM) {
                    uint8_t rex_index = addr.mem.index != GPR_NONE ? addr.mem.index : 0;

                    if (desired_val.gpr >= 8 && addr.mem.base >= 8 && rex_index >= 8) {
                        *ctx->header.out++ = rex(is_64bit, desired_val.gpr, addr.mem.base, rex_index);
                    }
                } else {
                    if (desired_val.gpr >= 8) {
                        *ctx->header.out++ = rex(is_64bit, desired_val.gpr, 0, 0);
                    }
                }
                *ctx->header.out++ = 0xF0;
                *ctx->header.out++ = 0xB0 | (bits_in_type <= 8 ? 1 : 0);
                emit_memory_operand(&ctx->header, desired_val.gpr, &addr);

                if (expected == desired) {
                    fast_kill_reg(ctx, f, expected);
                } else {
                    fast_kill_reg(ctx, f, expected);
                    fast_kill_reg(ctx, f, desired);
                }

                fast_def_gpr(ctx, f, r, RAX, l.dt);

                // the old value is in RAX
                ctx->gpr_allocator[RAX] = r;
                ctx->gpr_available += 1;
                break;
            }
            case TB_ATOMIC_CMPXCHG2: break;

            default: tb_todo();
        }

        if (ctx->temp_load_reg != GPR_NONE) {
            fast_kill_temp_gpr(ctx, f, ctx->temp_load_reg);
            ctx->temp_load_reg = GPR_NONE;
        }
        ctx->register_barrier = ctx->ordinal[r];
    }

    // tile mapping cannot cross BB boundaries
    if (ctx->tile.mapping) {
        fast_spill_tile(ctx, f);
    }
}

static void fast_eval_terminator_phis(X64_FastCtx* restrict ctx, TB_Function* f, TB_Reg from, TB_Reg from_terminator, TB_Reg to, TB_Reg to_terminator) {
    TB_FOR_EACH_NODE_RANGE(n, f, to, to_terminator) {
        TB_Reg r = n - f->nodes;

        if (tb_node_is_phi_node(f, r)) {
            TB_DataType dt = n->dt;

            int count = tb_node_get_phi_width(f, r);
            TB_PhiInput* inputs = tb_node_get_phi_inputs(f, r);

            loop(j, count) {
                if (inputs[j].label == from) {
                    TB_Reg src = inputs[j].val;

                    if (src != TB_NULL_REG) {
                        Val dst;
                        if (ctx->addresses[r].type == ADDRESS_DESC_NONE) {
                            // TODO(NeGate): Fix up PHI node spill slot recycling
                            /*if (ctx->use_count[src] == 1 && ctx->addresses[src].type == ADDRESS_DESC_SPILL) {
                                //printf("%s:r%u: recycle!\n", f->name, r);

                                ctx->addresses[r] = ctx->addresses[src];
                                ctx->addresses[src].type = ADDRESS_DESC_NONE;

                                dst = fast_eval(ctx, f, r);
                                continue;
                            } else {*/
                            int size = get_data_type_size(dt);
                            int pos  = STACK_ALLOC(size, size);

                            dst = val_stack(dt, pos);
                            fast_def_spill(ctx, f, r, pos, dt);
                            //}
                        } else {
                            assert(ctx->addresses[r].type == ADDRESS_DESC_SPILL);
                            dst = val_stack(dt, ctx->addresses[r].spill);
                        }

                        if (dt.width || TB_IS_FLOAT_TYPE(dt)) {
                            // Handle vector and float types
                            fast_folded_op_sse(ctx, f, FP_MOV, &dst, src);
                        } else {
                            fast_folded_op(ctx, f, MOV, &dst, src);
                        }
                    }
                }
            }
        }
    }
}

static FunctionTallySimple tally_memory_usage_simple(TB_Function* restrict f) {
    size_t locals_count = 0;
    size_t return_count = 0;
    size_t label_patch_count = 0;
    size_t line_info_count = 0;

    TB_FOR_EACH_NODE(n, f) {
        TB_NodeTypeEnum t = n->type;

        if (t == TB_RET) return_count++;
        else if (t == TB_LOCAL) locals_count++;
        else if (t == TB_IF) label_patch_count += 2;
        else if (t == TB_GOTO) label_patch_count++;
        else if (t == TB_LINE_INFO) line_info_count++;
        else if (t == TB_SWITCH) {
            label_patch_count += 1 + ((n->switch_.entries_end - n->switch_.entries_start) / 2);
        }
    }

    // parameters are locals too... ish
    locals_count += f->prototype->param_count;

    size_t align_mask = _Alignof(long double) - 1;
    size_t tally      = 0;

    // context
    tally += sizeof(X64_FastCtx) + (f->node_count * sizeof(AddressDesc));
    tally = (tally + align_mask) & ~align_mask;

    // ordinal
    tally += f->node_count * sizeof(int);
    tally = (tally + align_mask) & ~align_mask;

    // use_count
    tally += f->node_count * sizeof(TB_Reg);
    tally = (tally + align_mask) & ~align_mask;

    // intervals
    tally += f->node_count * sizeof(TB_Reg);
    tally = (tally + align_mask) & ~align_mask;

    // labels
    tally += f->label_count * sizeof(uint32_t);
    tally = (tally + align_mask) & ~align_mask;

    // label_patches
    tally += label_patch_count * sizeof(LabelPatch);
    tally = (tally + align_mask) & ~align_mask;

    // ret_patches
    tally += return_count * sizeof(ReturnPatch);
    tally = (tally + align_mask) & ~align_mask;

    return (FunctionTallySimple) {
        .memory_usage = tally,
        .line_info_count = line_info_count,
        .locals_count = locals_count,
        .return_count = return_count,
        .label_patch_count = label_patch_count
    };
}

// entry point to the x64 fast isel, it's got some nice features like when the
// temporary storage can't fit the necessary memory, it'll fallback to the heap
// to avoid just crashing.
TB_FunctionOutput x64_fast_compile_function(TB_FunctionID id, TB_Function* restrict f, const TB_FeatureSet* features, uint8_t* out, size_t local_thread_id) {
    s_local_thread_id = local_thread_id;

    TB_TemporaryStorage* tls = tb_tls_allocate();

    // Allocate all the memory we'll need
    bool is_ctx_heap_allocated = false;
    X64_FastCtx* restrict ctx = NULL;
    {
        // if we can't fit our memory usage into memory, we fallback
        FunctionTallySimple tally = tally_memory_usage_simple(f);
        is_ctx_heap_allocated = !tb_tls_can_fit(tls, tally.memory_usage);

        size_t ctx_size = sizeof(X64_FastCtx) + (f->node_count * sizeof(AddressDesc));
        if (is_ctx_heap_allocated) {
            ctx = tb_platform_heap_alloc(ctx_size);
            *ctx = (X64_FastCtx) {
                .header = {
                    .out           = out,
                    .start_out     = out,
                    .labels        = tb_platform_heap_alloc(f->label_count * sizeof(uint32_t)),
                    .label_patches = tb_platform_heap_alloc(tally.label_patch_count * sizeof(LabelPatch)),
                    .ret_patches   = tb_platform_heap_alloc(tally.return_count * sizeof(ReturnPatch))
                },
                .ordinal = tb_platform_heap_alloc(f->node_count * sizeof(int)),
                .use_count = tb_platform_heap_alloc(f->node_count * sizeof(TB_Reg))
            };
        } else {
            ctx = tb_tls_push(tls, ctx_size);
            *ctx = (X64_FastCtx) {
                .header = {
                    .out           = out,
                    .start_out     = out,
                    .labels        = tb_tls_push(tls, f->label_count * sizeof(uint32_t)),
                    .label_patches = tb_tls_push(tls, tally.label_patch_count * sizeof(LabelPatch)),
                    .ret_patches   = tb_tls_push(tls, tally.return_count * sizeof(ReturnPatch))
                },
                .ordinal = tb_tls_push(tls, f->node_count * sizeof(int)),
                .use_count = tb_tls_push(tls, f->node_count * sizeof(TB_Reg))
            };
        }

        ctx->header.f = f;

        f->line_count = 0;
        f->lines      = tb_platform_arena_alloc(tally.line_info_count * sizeof(TB_Line));

        ctx->gpr_available = 14;
        ctx->xmm_available = 16;
        ctx->temp_load_reg = GPR_NONE;
        ctx->is_sysv = (f->module->target_abi == TB_ABI_SYSTEMV);
        memset(ctx->addresses, 0, f->node_count * sizeof(AddressDesc));
    }

    // Analyze function for stack, use counts and phi nodes
    tb_function_calculate_use_count(f, ctx->use_count);

    int counter = 0;
    TB_FOR_EACH_NODE(n, f) {
        ctx->ordinal[n - f->nodes] = counter++;
    }

    // Create phi lookup table for later evaluation stages
    // and calculate the maximum parameter usage for a call
    size_t caller_usage = 0;
    TB_FOR_EACH_NODE(n, f) {
        if (EITHER3(n->type, TB_CALL, TB_ECALL, TB_VCALL)) {
            int param_usage = CALL_NODE_PARAM_COUNT(n);
            if (caller_usage < param_usage) { caller_usage = param_usage; }
        }
    }

    // On Win64 if we have at least one parameter in any of it's calls, the
    // caller must reserve 32bytes called the shadow space.
    if (!ctx->is_sysv && caller_usage > 0 && caller_usage < 4) caller_usage = 4;

    const TB_FunctionPrototype* restrict proto = f->prototype;
    loop(i, (size_t)proto->param_count) {
        TB_DataType dt = proto->params[i];
        TB_Reg r = TB_FIRST_PARAMETER_REG + i;

        // Allocate space in stack
        assert(get_data_type_size(dt) <= 8 && "Parameter too big");

        if (dt.width || TB_IS_FLOAT_TYPE(dt)) {
            // xmm parameters
            if (i < 4) {
                fast_def_xmm(ctx, f, r, (XMM)i, dt);
                ctx->xmm_allocator[(XMM)i] = r;
                ctx->xmm_available -= 1;
            } else
                fast_def_stack(ctx, f, r, 16 + (i * 8), dt);
        } else {
            // gpr parameters
            if (ctx->is_sysv && i < 6) {
                fast_def_gpr(ctx, f, r, (GPR)SYSV_GPR_PARAMETERS[i], dt);
                ctx->gpr_allocator[(GPR)SYSV_GPR_PARAMETERS[i]] = r;
                ctx->gpr_available -= 1;
            } else if (i < 4) {
                fast_def_gpr(ctx, f, r, (GPR)WIN64_GPR_PARAMETERS[i], dt);
                ctx->gpr_allocator[(GPR)WIN64_GPR_PARAMETERS[i]] = r;
                ctx->gpr_available -= 1;
            } else {
                fast_def_stack(ctx, f, r, 16 + (i * 8), dt);
            }
        }
    }

    if (proto->param_count) { ctx->header.stack_usage += 16 + (proto->param_count * 8); }

    if (proto->has_varargs) {
        const GPR* parameter_gprs = ctx->is_sysv ? SYSV_GPR_PARAMETERS : WIN64_GPR_PARAMETERS;

        // spill the rest of the parameters (assumes they're all in the GPRs)
        size_t gpr_count = ctx->is_sysv ? 6 : 4;
        size_t extra_param_count = proto->param_count > gpr_count ? 0 : gpr_count - proto->param_count;

        loop(i, extra_param_count) {
            size_t param_num = proto->param_count + i;

            Val dst = val_stack(TB_TYPE_I64, 16 + (param_num * 8));
            Val src = val_gpr(TB_TYPE_I64, parameter_gprs[param_num]);
            INST2(MOV, &dst, &src, TB_TYPE_I64);
        }
    }

    // printf("STACK MAP: %s\n", f->name);

    // Just the splitting point between parameters
    // and locals in the stack.
    TB_FOR_EACH_NODE(n, f) {
        TB_Reg r = n - f->nodes;

        if (n->type == TB_PARAM_ADDR) {
            int id = n->param_addr.param - TB_FIRST_PARAMETER_REG;
            TB_DataType dt = n->dt;

            if (dt.width || TB_IS_FLOAT_TYPE(dt)) {
                tb_todo();
            } else {
                // don't keep a reference of it in GPR if it's in memory
                if (ctx->is_sysv && id < 6) {
                    Val dst = val_stack(TB_TYPE_I64, 16 + (id * 8));
                    Val src = val_gpr(TB_TYPE_I64, SYSV_GPR_PARAMETERS[id]);
                    INST2(MOV, &dst, &src, TB_TYPE_I64);

                    ctx->gpr_allocator[SYSV_GPR_PARAMETERS[id]] = TB_NULL_REG;
                    ctx->gpr_available += 1;
                } else if (id < 4) {
                    Val dst = val_stack(TB_TYPE_I64, 16 + (id * 8));
                    Val src = val_gpr(TB_TYPE_I64, WIN64_GPR_PARAMETERS[id]);
                    INST2(MOV, &dst, &src, TB_TYPE_I64);

                    ctx->gpr_allocator[WIN64_GPR_PARAMETERS[id]] = TB_NULL_REG;
                    ctx->gpr_available += 1;
                }
            }

            fast_def_stack(ctx, f, r, 16 + (id * 8), n->dt);
        } else if (n->type == TB_LOCAL) {
            uint32_t size  = n->local.size;
            uint32_t align = n->local.alignment;
            int pos = STACK_ALLOC(size, align);

            fast_def_stack(ctx, f, r, pos, n->dt);

            // if (n->local.name) printf("  [rbp - %#x]\t%s\n", -pos, n->local.name);
        }
    }

    #if 0
    if (strcmp(f->name, "cuikpp_define_empty_slice") == 0) {
        tb_function_print(f, tb_default_print_callback, stdout);
        printf("\n\n\n");
    }
    #endif

    // Evaluate basic blocks
    TB_Reg bb = 1;
    do {
        assert(f->nodes[bb].type == TB_LABEL);
        TB_Node* start = &f->nodes[bb];

        TB_Reg bb_end = start->label.terminator;
        TB_Node* end = &f->nodes[bb_end];

        // Define label position
        TB_Label label_id = start->label.id;
        ctx->header.labels[label_id] = GET_CODE_POS();

        // Generate instructions
        fast_eval_basic_block(ctx, f, bb, bb_end);

        // Evaluate the terminator
        TB_Node* next_bb = end;

        if (end->type != TB_LABEL) next_bb = &f->nodes[next_bb->next];
        TB_Reg next_bb_reg = next_bb - f->nodes;

        if (end->type == TB_RET) {
            TB_DataType dt = end->dt;

            // Evaluate return value
            if (end->ret.value) {
                if (dt.type == TB_FLOAT) {
                    Val dst = val_xmm(dt, XMM0);
                    fast_folded_op_sse(ctx, f, FP_MOV, &dst, end->ret.value);
                } else if ((dt.type == TB_INT && dt.data > 0) || dt.type == TB_PTR) {
                    Val dst = val_gpr(dt, RAX);
                    fast_folded_op(ctx, f, MOV, &dst, end->ret.value);
                } else tb_todo();
            }

            // Only jump if we aren't literally about to end the function
            if (next_bb != f->nodes) {
                RET_JMP();
            }
        } else if (end->type == TB_IF) {
            TB_Label if_true = end->if_.if_true;
            TB_Label if_false = end->if_.if_false;

            // Save out PHI nodes
            {
                TB_Reg if_true_reg = tb_find_reg_from_label(f, if_true);
                TB_Reg if_false_reg = tb_find_reg_from_label(f, if_false);

                TB_Reg if_true_reg_end = f->nodes[if_true_reg].label.terminator;
                TB_Reg if_false_reg_end = f->nodes[if_false_reg].label.terminator;

                fast_eval_terminator_phis(ctx, f, bb, bb_end, if_true_reg, if_true_reg_end);
                fast_eval_terminator_phis(ctx, f, bb, bb_end, if_false_reg, if_false_reg_end);
            }

            Cond cc = fast_eval_cond(ctx, f, end->if_.cond);
            fast_evict_everything(ctx, f);

            // Reorder the targets to avoid an extra JMP
            TB_Label fallthrough_label = 0;
            if (next_bb != f->nodes) { fallthrough_label = next_bb->label.id; }
            bool has_fallthrough = fallthrough_label == if_false;

            // flip the condition and the labels if
            // it allows for fallthrough
            if (fallthrough_label == if_true) {
                tb_swap(TB_Label, if_true, if_false);
                cc ^= 1;

                has_fallthrough = true;
            }

            // JCC .true
            // JMP .false # elidable if it points to the next instruction
            JCC(cc, if_true);
            if (!has_fallthrough) JMP(if_false);
        } else if (end->type == TB_LABEL) {
            // save out PHI nodes
            TB_Reg next_terminator = end->label.terminator;
            fast_eval_terminator_phis(ctx, f, bb, bb_end, bb_end, next_terminator);

            fast_evict_everything(ctx, f);
        } else if (end->type == TB_UNREACHABLE) {
            *ctx->header.out++ = 0x0F;
            *ctx->header.out++ = 0x0B;
        } else if (end->type == TB_GOTO) {
            // save out PHI nodes
            TB_Label target_label = end->goto_.label;
            TB_Reg target = tb_find_reg_from_label(f, target_label);
            TB_Reg target_end = f->nodes[target].label.terminator;

            fast_eval_terminator_phis(ctx, f, bb, bb_end, target, target_end);
            fast_evict_everything(ctx, f);

            TB_Label fallthrough_label = 0;
            if (next_bb != f->nodes) { fallthrough_label = next_bb->label.id; }
            bool has_fallthrough = fallthrough_label == target_label;

            if (!has_fallthrough) JMP(end->goto_.label);
        } else if (end->type == TB_SWITCH) {
            static_assert(_Alignof(TB_SwitchEntry) == _Alignof(TB_Reg), "We don't want any unaligned accesses");
            TB_Node* switch_key = &f->nodes[end->switch_.key];

            if (switch_key->type == TB_INTEGER_CONST &&
                switch_key->integer.num_words == 1) {
                size_t entry_count = (end->switch_.entries_end - end->switch_.entries_start) / 2;
                uint64_t key_imm = switch_key->integer.single_word;

                TB_Label target_label = end->switch_.default_label;
                loop(i, entry_count) {
                    TB_SwitchEntry* entry = (TB_SwitchEntry*)&f->vla.data[end->switch_.entries_start + (i * 2)];

                    if (entry->key == key_imm) {
                        target_label = entry->value;
                        break;
                    }
                }

                TB_Reg target = tb_find_reg_from_label(f, target_label);
                TB_Reg target_end = f->nodes[target].label.terminator;
                fast_eval_terminator_phis(ctx, f, bb, bb_end, target, target_end);
                fast_evict_everything(ctx, f);

                TB_Label fallthrough_label = next_bb->label.id;
                if (fallthrough_label != target) {
                    JMP(target_label);
                }
            } else {
                LegalInt l = legalize_int(end->dt);

                Val key = val_gpr(l.dt, fast_alloc_gpr(ctx, f, TB_TEMP_REG));
                fast_folded_op(ctx, f, MOV, &key, end->switch_.key);
                if (l.mask) fast_mask_out(ctx, f, l, &key);
                fast_kill_temp_gpr(ctx, f, key.gpr);

                fast_evict_everything(ctx, f);

                // Shitty if-chain
                // CMP key, 0
                // JE .case0
                // CMP key, 10
                // JE .case10
                // JMP .default
                size_t entry_count = (end->switch_.entries_end - end->switch_.entries_start) / 2;
                loop(i, entry_count) {
                    TB_SwitchEntry* entry = (TB_SwitchEntry*)&f->vla.data[end->switch_.entries_start + (i * 2)];
                    Val operand = val_imm(l.dt, entry->key);

                    INST2(CMP, &key, &operand, l.dt);
                    JCC(E, entry->value);
                }

                JMP(end->switch_.default_label);
            }
        } else tb_todo();

        // Next Basic block
        bb = next_bb_reg;
    } while (bb != TB_NULL_REG);

    // Fix up stack usage
    // Tally up any saved XMM registers
    ctx->header.stack_usage += tb_popcount((ctx->header.regs_to_save >> 16) & 0xFFFF) * 16;

    // allocate callee parameter space
    ctx->header.stack_usage += caller_usage * 8;

    // Align stack usage to 16bytes and add 8 bytes for the return address
    if (ctx->header.stack_usage > 0) {
        ctx->header.stack_usage = align_up(ctx->header.stack_usage + 8, 16) + 8;
    } else {
        ctx->header.stack_usage = 8;
    }

    // Resolve internal relocations
    loop(i, ctx->header.ret_patch_count) {
        uint32_t pos = ctx->header.ret_patches[i];
        PATCH4(pos, GET_CODE_POS() - (pos + 4));
    }

    loop(i, ctx->header.label_patch_count) {
        uint32_t pos = ctx->header.label_patches[i].pos;
        uint32_t target_lbl = ctx->header.label_patches[i].target_lbl;

        PATCH4(pos, ctx->header.labels[target_lbl] - (pos + 4));
    }

    if (f->line_count > 0) {
        f->lines[0].pos = 0;
    }

    TB_FunctionOutput func_out = {
        .linkage = f->linkage,
        .code = ctx->header.start_out,
        .code_size = ctx->header.out - ctx->header.start_out,
        .stack_usage = ctx->header.stack_usage,
        .prologue_epilogue_metadata = ctx->header.regs_to_save
    };

    if (is_ctx_heap_allocated) {
        tb_platform_heap_free(ctx->use_count);

        tb_platform_heap_free(ctx->header.labels);
        tb_platform_heap_free(ctx->header.label_patches);
        tb_platform_heap_free(ctx->header.ret_patches);
        tb_platform_heap_free(ctx);
    }
    return func_out;
}
