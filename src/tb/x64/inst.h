// This file is responsible for generating normal x64 instructions

inline static uint8_t mod_rx_rm(uint8_t mod, uint8_t rx, uint8_t rm) {
	return ((mod & 3) << 6) | ((rx & 7) << 3) | (rm & 7);
}

inline static uint8_t rex(bool is_64bit, uint8_t rx, uint8_t base, uint8_t index) {
	return 0x40 | (is_64bit ? 8 : 0) | (base >> 3) | ((index >> 3) << 1) | ((rx >> 3) << 2);
}

inline static void emit_memory_operand(Ctx* ctx, uint8_t rx, const Val* restrict a) {
	// Operand encoding
	if (a->type == VAL_GPR || a->type == VAL_XMM) {
		emit(mod_rx_rm(MOD_DIRECT, rx, a->gpr));
	} else if (a->type == VAL_MEM) {
		GPR base = a->mem.base;
		GPR index = a->mem.index;
		uint8_t scale = a->mem.scale;
		int32_t disp = a->mem.disp;
		
		bool needs_index = (index != GPR_NONE) || (base & 7) == RSP;
		
		// If it needs an index, it'll put RSP into the base slot
		// and write the real base into the SIB
		uint8_t mod = MOD_INDIRECT_DISP32;
		if (disp == 0) mod = MOD_INDIRECT;
		else if (disp == (int8_t)disp) mod = MOD_INDIRECT_DISP8;
		
		emit(mod_rx_rm(mod, rx, needs_index ? RSP : base));
		if (needs_index) {
			emit(mod_rx_rm(scale, (base & 7) == RSP ? RSP : index, base));
		}
		
		if (mod == MOD_INDIRECT_DISP8) emit((int8_t)disp);
		else if (mod == MOD_INDIRECT_DISP32) emit4(disp);
	} else if (a->type == VAL_GLOBAL) {
		emit(((rx & 7) << 3) | RBP);
		emit4(0x0);
		
		tb_emit_global_patch(ctx->f->module, ctx->function_id,
							 code_pos() - 4, a->global.id,
							 s_local_thread_id);
	} else tb_unreachable();
}

inline static void inst1(Ctx* ctx, Inst1 op, const Val* r) {
    if (r->type == VAL_GPR) {
		emit(rex(true, 0x00, r->gpr, 0x00));
		emit((op >> 8) & 0xFF);
		emit(mod_rx_rm(MOD_DIRECT, op & 0xFF, r->gpr));
	} else if (r->type == VAL_MEM) {
		GPR base = r->mem.base;
		GPR index = r->mem.index;
		uint8_t scale = r->mem.scale;
		int32_t disp = r->mem.disp;
		
		bool needs_index = (index != GPR_NONE) || (base & 7) == RSP;
		
		emit(rex(true, 0x00, base, index != GPR_NONE ? index : 0));
		emit((op >> 8) & 0xFF);
		
		// If it needs an index, it'll put RSP into the base slot
		// and write the real base into the SIB
		uint8_t mod = MOD_INDIRECT_DISP32;
		if (disp == 0) mod = MOD_INDIRECT_DISP8;
		else if (disp == (int8_t)disp) mod = MOD_INDIRECT_DISP8;
		
		emit(mod_rx_rm(mod, op & 0xFF, needs_index ? RSP : base));
		if (needs_index) {
			emit(mod_rx_rm(scale, (base & 7) == RSP ? RSP : index, base));
		}
		
		if (mod == MOD_INDIRECT_DISP8) emit((int8_t)disp);
		else if (mod == MOD_INDIRECT_DISP32) emit4((int32_t)disp);
	} else tb_unreachable();
}

inline static void inst2(Ctx* ctx, Inst2Type op, const Val* a, const Val* b, int dt_type) {
	assert(op < (sizeof(inst2_tbl) / sizeof(inst2_tbl[0])));
	const Inst2* inst = &inst2_tbl[op];
	
	bool dir = b->type == VAL_MEM || b->type == VAL_GLOBAL;
	if (dir || inst->op == 0xAF || inst->ext == EXT_DEF2) tb_swap(a, b);
	
	// operand size
	uint8_t sz = (dt_type != TB_I8);
	
	// uses an immediate value that works as
	// a sign extended 8 bit number
	bool short_imm = (dt_type != TB_I8 && 
					  b->type == VAL_IMM &&
					  b->imm == (int8_t)b->imm &&
					  inst->op_i == 0x80);
	
	// All instructions that go through here are
	// based on the ModRxRm encoding so we do need
	// an RX and an RM (base, index, shift, disp)
	uint8_t base = 0;
	uint8_t rx = 0xFF;
	if (inst->ext == EXT_NONE || inst->ext == EXT_DEF || inst->ext == EXT_DEF2) {
		assert(dt_type == TB_I8 || dt_type == TB_I16 || dt_type == TB_I32 || dt_type == TB_I64 || dt_type == TB_PTR);
		
		// the destination can only be a GPR, no direction flag
		bool is_gpr_only_dst = (inst->op & 1);
		bool dir_flag = dir != is_gpr_only_dst;
		
		// Address size prefix
		if (dt_type == TB_I16 && inst->ext != EXT_DEF2) {
			emit(0x66);
		}
		
		// RX
		if (b->type == VAL_GPR) rx = b->gpr;
		else if (b->type == VAL_IMM) rx = inst->rx_i;
		else __builtin_unreachable();
		
		// RM & REX
		bool is_64bit = (dt_type == TB_I64 || dt_type == TB_PTR);
		
		if (a->type == VAL_GPR) {
			base = a->gpr;
			
			if (base >= 8 || rx >= 8 || is_64bit) {
				emit(rex(is_64bit, rx, base, 0));
			}
		} else if (a->type == VAL_MEM) {
			base = a->mem.base;
			
			uint8_t rex_index = (a->mem.index != GPR_NONE ? a->mem.index : 0);
			if (base >= 8 || rx >= 8 || rex_index >= 8 || is_64bit) {
				emit(rex(is_64bit, rx, base, rex_index));
			}
		} else if (a->type == VAL_GLOBAL) {
			base = RBP;
			if (rx >= 8 || is_64bit) emit(rex(is_64bit, rx, base, 0));
		} else __builtin_unreachable();
		
		// Opcode
		if (inst->ext == EXT_DEF || inst->ext == EXT_DEF2) {
			// DEF instructions can only be 32bit and 64bit... maybe?
			sz = 0;
			emit(0x0F);
		}
		
		if (b->type == VAL_IMM && inst->op_i == 0 && inst->rx_i == 0) {
			// No immediate version
			tb_unreachable();
		}
		
		// Immediates have a custom opcode
		uint8_t opcode = b->type == VAL_IMM ? inst->op_i : inst->op;
		if (short_imm) opcode |= 2;
		
		emit(opcode | sz | (dir_flag ? 2 : 0));
	}
	else tb_unreachable();
	
	// We forgot a case!
	assert(rx != 0xFF);
	emit_memory_operand(ctx, rx, a);
	
	if (b->type == VAL_IMM) {
		if (dt_type == TB_I8 || short_imm) {
			assert(b->imm == (int8_t)b->imm);
			emit((int8_t)b->imm);
		} else if (dt_type == TB_I16) {
			assert(b->imm == (int16_t)b->imm);
			emit2((int16_t)b->imm);
		} else {
			assert(dt_type == TB_I32 || dt_type == TB_I64 || dt_type == TB_PTR);
			emit4((int32_t)b->imm);
		}
	}
}

inline static void inst2sse(Ctx* ctx, Inst2FPType op, const Val* a, const Val* b, uint8_t flags) {
	const static uint8_t OPCODES[] = {
		[FP_MOV] = 0x10,
		[FP_ADD] = 0x58,
		[FP_MUL] = 0x59,
		[FP_SUB] = 0x5C,
		[FP_DIV] = 0x5E,
		[FP_CMP] = 0xC2,
		[FP_CVT] = 0x5A,
		[FP_SQRT] = 0x51,
		[FP_RSQRT] = 0x52,
		[FP_AND] = 0x54,
		[FP_OR] = 0x56,
		[FP_XOR] = 0x57,
	};
	
	// most SSE instructions (that aren't mov__) are mem src only
	bool supports_mem_dst = (op == FP_MOV);
	bool dir = is_value_mem(a);
	if (supports_mem_dst && dir) {
		tb_swap(a, b);
	}
	
	uint8_t rx = a->xmm;
	
	uint8_t base, index;
	if (b->type == VAL_MEM) {
		base = b->mem.base;
		index = b->mem.index != GPR_NONE ? b->mem.index : 0;
	} else if (b->type == VAL_XMM) {
		base = b->xmm;
		index = 0;
	} else if (b->type == VAL_GLOBAL) {
		base = 0;
		index = 0;
	} else tb_todo();
	
	if (rx >= 8 || base >= 8 || index >= 8) {
		emit(rex(false, rx, base, index));
	}
	
	if ((flags & INST2FP_PACKED) == 0) {
		emit(flags & INST2FP_DOUBLE ? 0xF2 : 0xF3);
	} else if (flags & INST2FP_DOUBLE) {
		// packed double
		emit(0x66);
	}
	
	// extension prefix
	emit(0x0F);
	
	emit(OPCODES[op] + (supports_mem_dst ? dir : 0));
	emit_memory_operand(ctx, rx, b);
}

inline static void jcc(Ctx* ctx, Cond cc, int label) {
	size_t pos = ctx->out - ctx->start_out;
	ctx->label_patches[ctx->label_patch_count++] = (LabelPatch){
		.pos = pos + 2, .target_lbl = label
	};
	
	emit(0x0F);
	emit(0x80 + (uint8_t)cc);
	emit4(0x0);
}

inline static void jmp(Ctx* ctx, int label) {
	size_t pos = ctx->out - ctx->start_out;
	ctx->label_patches[ctx->label_patch_count++] = (LabelPatch){
		.pos = pos + 1, .target_lbl = label 
	};
	
	emit(0xE9);
	emit4(0x0);
}
