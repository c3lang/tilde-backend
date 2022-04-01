// IR BUILDER
// 
// Handles generating the TB_Function IR via C functions. 
// Note that these functions can perform certain simple
// optimizations while the generation happens to improve
// the machine code output or later analysis stages.
#include "tb_internal.h"

TB_API TB_FunctionID tb_function_get_id(TB_Module* m, TB_Function* f) {
	intptr_t id = f - m->functions.data;
	tb_assume(id == (TB_FunctionID) id);
	return id;
}

TB_API TB_Function* tb_function_from_id(TB_Module* m, TB_FunctionID id) {
	tb_assume(id < m->functions.count);
	return &m->functions.data[id];
}

TB_API TB_Node* tb_function_get_node(TB_Function* f, TB_Reg r) {
	tb_assume(r >= 0 && r < f->nodes.count);
	return &f->nodes.data[r];
}

TB_API void tb_get_function_get_local_info(TB_Function* f, TB_Reg r, int* size, int* align) {
	tb_assume(f->nodes.data[r].type == TB_LOCAL);
	
	*size = f->nodes.data[r].local.size;
	*align = f->nodes.data[r].local.alignment;
}

TB_API bool tb_node_is_conditional(TB_Function* f, TB_Reg r) {
	return f->nodes.data[r].type == TB_IF;
}

TB_API bool tb_node_is_terminator(TB_Function* f, TB_Reg r) {
	return f->nodes.data[r].type == TB_IF ||
		f->nodes.data[r].type == TB_GOTO ||
		f->nodes.data[r].type == TB_RET ||
		f->nodes.data[r].type == TB_LABEL;
}

TB_API bool tb_node_is_label(TB_Function* f, TB_Reg r) {
	return f->nodes.data[r].type == TB_LABEL;
}

TB_API TB_Reg tb_node_get_last_register(TB_Function* f) {
	return f->nodes.count - 1;
}

TB_API TB_Reg tb_node_load_get_address(TB_Function* f, TB_Reg r) {
	tb_assume(f->nodes.data[r].type == TB_LOAD);
	
	return f->nodes.data[r].load.address;
}

TB_API TB_Reg tb_node_arith_get_left(TB_Function* f, TB_Reg r) {
	tb_assume(f->nodes.data[r].type >= TB_AND && f->nodes.data[r].type <= TB_CMP_FLE);
	
	// TODO(NeGate): They share position in the union
	static_assert(offsetof(TB_Node, cmp.a) == offsetof(TB_Node, i_arith.a), "TB_RegPayload::cmp.a should alias TB_RegPayload::i_arith.a");
	static_assert(offsetof(TB_Node, f_arith.a) == offsetof(TB_Node, i_arith.a), "TB_RegPayload::f_arith.a should alias TB_RegPayload::i_arith.a");
	
	return f->nodes.data[r].i_arith.a;
}

TB_API TB_Reg tb_node_arith_get_right(TB_Function* f, TB_Reg r) {
	tb_assume(f->nodes.data[r].type >= TB_AND && f->nodes.data[r].type <= TB_CMP_FLE);
	
	// TODO(NeGate): They share position in the union
	static_assert(offsetof(TB_Node, cmp.b) == offsetof(TB_Node, i_arith.b), "TB_RegPayload::cmp.b should alias TB_RegPayload::i_arith.b");
	static_assert(offsetof(TB_Node, f_arith.b) == offsetof(TB_Node, i_arith.b), "TB_RegPayload::f_arith.b should alias TB_RegPayload::i_arith.b");
	
	return f->nodes.data[r].i_arith.b;
}

TB_API bool tb_node_is_constant_int(TB_Function* f, TB_Reg r, uint64_t imm) {
	if (f->nodes.data[r].type == TB_UNSIGNED_CONST || f->nodes.data[r].type == TB_SIGNED_CONST) {
		return (f->nodes.data[r].uint.value == imm);
	}
	
	return false;
}

TB_API bool tb_node_get_constant_int(TB_Function* f, TB_Reg r, uint64_t* imm, bool* is_signed) {
	TB_Node* restrict n = &f->nodes.data[r];
	
	if (n->type == TB_UNSIGNED_CONST || n->type == TB_SIGNED_CONST) {
		*imm = n->uint.value;
		
		if (is_signed) {
			*is_signed = n->type == TB_SIGNED_CONST;
		}
		return true;
	}
	
	return false;
}

static TB_Reg tb_make_reg(TB_Function* f, int type, TB_DataType dt) {
	// Cannot add registers to terminated basic blocks, except labels
	// which start new basic blocks
	tb_assume(f);
	
	if (type != TB_LABEL && f->current_label == 0) {
		fprintf(stderr, "Cannot create node without parent basic block\n");
		tb_function_print(f, tb_default_print_callback, stderr);
		fprintf(stderr, "\n\n\n");
		abort();
	}
    
	tb_function_reserve_nodes(f, 1);
	
	TB_Reg r = f->nodes.count++;
	f->nodes.data[r] = (TB_Node) {
		.type = type,
		.dt = dt,
		.next = 0
	};
	
	f->nodes.data[f->nodes.end].next = r;
	f->nodes.end = r;
	
	// map the scope attribute
	f->attrib_map[r].attrib = f->active_attrib;
	return r;
}

static TB_Reg tb_bin_arith(TB_Function* f, int type, TB_ArithmaticBehavior arith_behavior, TB_Reg a, TB_Reg b) {
	//tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	if (!TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt)) {
		tb_function_print(f, tb_default_print_callback, stderr);
		abort();
	}
	
	TB_Reg r = tb_make_reg(f, type, f->nodes.data[a].dt);
	f->nodes.data[r].i_arith.arith_behavior = arith_behavior;
	f->nodes.data[r].i_arith.a = a;
	f->nodes.data[r].i_arith.b = b;
	return r;
}

static TB_Reg tb_bin_farith(TB_Function* f, int type, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	
	TB_Reg r = tb_make_reg(f, type, f->nodes.data[a].dt);
	f->nodes.data[r].f_arith.a = a;
	f->nodes.data[r].f_arith.b = b;
	return r;
}

static TB_AttributeID tb_make_attrib(TB_Function* f, int extra) {
	if (f->attrib_pool_count + extra >= f->attrib_pool_capacity) {
		f->attrib_pool_capacity = (f->attrib_pool_count + extra) * 2;
		
		f->attrib_pool = realloc(f->attrib_pool, sizeof(TB_Attrib) * f->attrib_pool_capacity);
		if (!f->attrib_pool) tb_panic("Out of memory");
	}
	
	return f->attrib_pool_count++;
}

TB_API void tb_inst_set_scope(TB_Function* f, TB_AttributeID scope) {
	f->active_attrib = scope;
}

TB_API TB_AttributeID tb_inst_get_scope(TB_Function* f) {
	return f->active_attrib;
}

TB_API TB_AttributeID tb_function_attrib_restrict(TB_Function* f, TB_AttributeID scope) {
	TB_AttributeID id = tb_make_attrib(f, 1);
	
	f->attrib_pool[id] = (TB_Attrib){ .type = TB_ATTRIB_RESTRICT, .ref = scope };
	return id;
}

TB_API TB_AttributeID tb_function_attrib_scope(TB_Function* f, TB_AttributeID parent_scope) {
	TB_AttributeID id = tb_make_attrib(f, 1);
	
	f->attrib_pool[id] = (TB_Attrib){ .type = TB_ATTRIB_SCOPE, .ref = parent_scope };
	return id;
}

TB_API void tb_function_append_attrib(TB_Function* f, TB_Reg r, TB_AttributeID a) {
	if (f->attrib_map[r].next == NULL) {
		// empty chain
		f->attrib_map[r].attrib = a;
	} else {
		// TODO(NeGate): this code path will a slow thing if abused...
		TB_AttribList* new_link = malloc(sizeof(TB_AttribList));
		new_link->attrib = a;
		new_link->next = NULL;
		
		TB_AttribList* chain = f->attrib_map[r].next;
		do {
			chain = chain->next;
		} while (chain != NULL);
		
		chain->next = new_link;
	}
}

TB_API TB_Reg tb_inst_trunc(TB_Function* f, TB_Reg src, TB_DataType dt) {
	tb_assume(f->nodes.data[src].dt.width == dt.width);
	
	TB_Reg r = tb_make_reg(f, TB_TRUNCATE, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_int2ptr(TB_Function* f, TB_Reg src) {
	tb_assume(f->nodes.data[src].dt.width == 0);
	
	TB_Reg r = tb_make_reg(f, TB_INT2PTR, TB_TYPE_PTR);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_ptr2int(TB_Function* f, TB_Reg src, TB_DataType dt) {
	tb_assume(dt.width == 0);
	tb_assume(f->nodes.data[src].dt.width == 0);
	
	TB_Reg r = tb_make_reg(f, TB_PTR2INT, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_int2float(TB_Function* f, TB_Reg src, TB_DataType dt) {
	tb_assume(f->nodes.data[src].dt.width == dt.width);
	
	if (f->nodes.data[src].type == TB_SIGNED_CONST) {
		return tb_inst_float(f, dt, f->nodes.data[src].sint.value);
	} else if (f->nodes.data[src].type == TB_UNSIGNED_CONST) {
		return tb_inst_float(f, dt, f->nodes.data[src].uint.value);
	}
	
	TB_Reg r = tb_make_reg(f, TB_INT2FLOAT, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_float2int(TB_Function* f, TB_Reg src, TB_DataType dt) {
	tb_assume(f->nodes.data[src].dt.width == dt.width);
	
	TB_Reg r = tb_make_reg(f, TB_FLOAT2INT, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_fpxt(TB_Function* f, TB_Reg src, TB_DataType dt) {
	tb_assume(dt.width == f->nodes.data[src].dt.width);
	
	TB_Reg r = tb_make_reg(f, TB_FLOAT_EXT, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_sxt(TB_Function* f, TB_Reg src, TB_DataType dt) {
	tb_assume(dt.width == f->nodes.data[src].dt.width);
	
	TB_Reg r = tb_make_reg(f, TB_SIGN_EXT, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_zxt(TB_Function* f, TB_Reg src, TB_DataType dt) {
	tb_assume(dt.width == f->nodes.data[src].dt.width);
	
	TB_Reg r = tb_make_reg(f, TB_ZERO_EXT, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_bitcast(TB_Function* f, TB_Reg src, TB_DataType dt) {
	// TODO(NeGate): Do some size checks
	TB_Reg r = tb_make_reg(f, TB_BITCAST, dt);
	f->nodes.data[r].unary.src = src;
	return r;
}

TB_API TB_Reg tb_inst_param(TB_Function* f, int param_id) {
	tb_assume(param_id < f->prototype->param_count);
	return 2 + param_id;
}

TB_API TB_Reg tb_inst_param_addr(TB_Function* f, int param_id) {
	tb_assume(param_id < f->prototype->param_count);
	
	TB_Reg param = 2 + param_id;
	int param_size = f->nodes.data[param].param.size;
	
	TB_Reg r = tb_make_reg(f, TB_PARAM_ADDR, TB_TYPE_PTR);
	f->nodes.data[r].param_addr.param = param;
	f->nodes.data[r].param_addr.size = param_size;
	f->nodes.data[r].param_addr.alignment = param_size;
	return r;
}

TB_API void tb_inst_debugbreak(TB_Function* f) {
	tb_make_reg(f, TB_DEBUGBREAK, TB_TYPE_VOID);
}

TB_API void tb_inst_loc(TB_Function* f, TB_FileID file, int line) {
	tb_assume(line >= 0);
	if (f->nodes.data[f->nodes.count - 1].type == TB_LINE_INFO) {
		return;
	}
	
	TB_Reg r = tb_make_reg(f, TB_LINE_INFO, TB_TYPE_VOID);
	f->nodes.data[r].line_info.file = file;
	f->nodes.data[r].line_info.line = line;
}

TB_API TB_Reg tb_inst_local(TB_Function* f, uint32_t size, TB_CharUnits alignment) {
	tb_assume(size > 0);
	tb_assume(alignment > 0 && tb_is_power_of_two(alignment));
	
	TB_Reg r = tb_make_reg(f, TB_LOCAL, TB_TYPE_PTR);
	f->nodes.data[r].local.alignment = alignment;
	f->nodes.data[r].local.size = size;
	return r;
}

TB_API TB_Reg tb_inst_restrict(TB_Function* f, TB_Reg value) {
	return tb_make_reg(f, TB_RESTRICT, TB_TYPE_PTR);
}

TB_API TB_Reg tb_inst_load(TB_Function* f, TB_DataType dt, TB_Reg addr, TB_CharUnits alignment) {
	tb_assume(f->current_label);
	
	TB_Reg r = tb_make_reg(f, TB_LOAD, dt);
	f->nodes.data[r].load = (struct TB_NodeLoad) {
		.address = addr,
		.alignment = alignment
	};
	return r;
}

TB_API void tb_inst_store(TB_Function* f, TB_DataType dt, TB_Reg addr, TB_Reg val, uint32_t alignment) {
	tb_assume(addr);
	tb_assume(val);
	
	TB_Reg r = tb_make_reg(f, TB_STORE, dt);
	f->nodes.data[r].store = (struct TB_NodeStore) {
		.address = addr,
		.value = val,
		.alignment = alignment
	};
	return;
}

TB_API TB_Reg tb_inst_volatile_load(TB_Function* f, TB_DataType dt, TB_Reg addr, TB_CharUnits alignment) {
	tb_assume(f->current_label);
	
	TB_Reg r = tb_make_reg(f, TB_LOAD, dt);
	f->nodes.data[r].store = (struct TB_NodeStore) {
		.address = addr,
		.alignment = alignment,
		.is_volatile = true
	};
	return r;
}

TB_API void tb_inst_volatile_store(TB_Function* f, TB_DataType dt, TB_Reg addr, TB_Reg val, TB_CharUnits alignment) {
	TB_Reg r = tb_make_reg(f, TB_STORE, dt);
	f->nodes.data[r].store = (struct TB_NodeStore) {
		.address = addr,
		.value = val,
		.alignment = alignment,
		.is_volatile = true
	};
}

TB_API void tb_inst_initialize_mem(TB_Function* f, TB_Reg addr, TB_InitializerID src) {
	TB_Reg r = tb_make_reg(f, TB_INITIALIZE, TB_TYPE_PTR);
	f->nodes.data[r].init.addr = addr;
	f->nodes.data[r].init.id = src;
}

TB_API TB_Reg tb_inst_bool(TB_Function* f, bool imm) {
	TB_Reg r = tb_make_reg(f, TB_UNSIGNED_CONST, TB_TYPE_BOOL);
	f->nodes.data[r].uint.value = imm;
	return r;
}

TB_API TB_Reg tb_inst_ptr(TB_Function* f, uint64_t imm) {
	TB_Reg r = tb_make_reg(f, TB_UNSIGNED_CONST, TB_TYPE_PTR);
	f->nodes.data[r].uint.value = imm;
	return r;
}

TB_API TB_Reg tb_inst_uint(TB_Function* f, TB_DataType dt, uint64_t imm) {
	tb_assume(dt.type == TB_BOOL || dt.type == TB_PTR || (dt.type >= TB_I8 && dt.type <= TB_I64));
	
	TB_Reg r = tb_make_reg(f, TB_UNSIGNED_CONST, dt);
	f->nodes.data[r].uint.value = imm;
	return r;
}

TB_API TB_Reg tb_inst_sint(TB_Function* f, TB_DataType dt, int64_t imm) {
	tb_assume(dt.type == TB_BOOL || dt.type == TB_PTR || (dt.type >= TB_I8 && dt.type <= TB_I64));
	
	TB_Reg r = tb_make_reg(f, TB_SIGNED_CONST, dt);
	f->nodes.data[r].sint.value = imm;
	return r;
}

TB_API TB_Reg tb_inst_float(TB_Function* f, TB_DataType dt, double imm) {
	TB_Reg r = tb_make_reg(f, TB_FLOAT_CONST, dt);
	f->nodes.data[r].flt.value = imm;
	return r;
}

TB_API TB_Reg tb_inst_cstring(TB_Function* f, const char* str) {
	size_t len = strlen(str);
	char* newstr = tb_platform_arena_alloc(len + 1);
	memcpy(newstr, str, len);
	newstr[len] = '\0';
	
	TB_Reg r = tb_make_reg(f, TB_STRING_CONST, TB_TYPE_PTR);
	f->nodes.data[r].string = (struct TB_NodeString) {
		.length = len + 1,
		.data = newstr
	};
	return r;
}

TB_API TB_Reg tb_inst_string(TB_Function* f, size_t len, const char* str) {
	char* newstr = tb_platform_arena_alloc(len);
	memcpy(newstr, str, len);
	
	TB_Reg r = tb_make_reg(f, TB_STRING_CONST, TB_TYPE_PTR);
	f->nodes.data[r].string = (struct TB_NodeString) {
		.length = len,
		.data = newstr
	};
	return r;
}

TB_API TB_Reg tb_inst_array_access(TB_Function* f, TB_Reg base, TB_Reg index, uint32_t stride) {
	TB_Reg r = tb_make_reg(f, TB_ARRAY_ACCESS, TB_TYPE_PTR);
	f->nodes.data[r].array_access.base = base;
	f->nodes.data[r].array_access.index = index;
	f->nodes.data[r].array_access.stride = stride;
	return r;
}

TB_API TB_Reg tb_inst_member_access(TB_Function* f, TB_Reg base, int32_t offset) {
	TB_Reg r = tb_make_reg(f, TB_MEMBER_ACCESS, TB_TYPE_PTR);
	f->nodes.data[r].member_access.base = base;
	f->nodes.data[r].member_access.offset = offset;
	return r;
}

TB_API TB_Reg tb_inst_get_func_address(TB_Function* f, const TB_Function* target) {
	TB_Reg r = tb_make_reg(f, TB_FUNC_ADDRESS, TB_TYPE_PTR);
	f->nodes.data[r].func.value = target;
	return r;
}

TB_API TB_Reg tb_inst_get_extern_address(TB_Function* f, TB_ExternalID target) {
	TB_Reg r = tb_make_reg(f, TB_EXTERN_ADDRESS, TB_TYPE_PTR);
	f->nodes.data[r].external.value = target;
	return r;
}

TB_API TB_Reg tb_inst_get_global_address(TB_Function* f, TB_GlobalID target) {
	TB_Reg r = tb_make_reg(f, TB_GLOBAL_ADDRESS, TB_TYPE_PTR);
	f->nodes.data[r].global = (struct TB_NodeGlobal){ target };
	return r;
}

TB_Reg* tb_vla_reserve(TB_Function* f, size_t count) {
	// Reserve space for the arguments
	if (f->vla.count + count >= f->vla.capacity) {
		f->vla.capacity = f->vla.capacity ? tb_next_pow2(f->vla.count + count) : 16;
		f->vla.data = tb_platform_heap_realloc(f->vla.data, f->vla.capacity * sizeof(TB_Reg));
	}
	
	return &f->vla.data[f->vla.count];
}

TB_API TB_Reg tb_inst_call(TB_Function* f, TB_DataType dt, const TB_Function* target, size_t param_count, const TB_Reg* params) {
	int param_start = f->vla.count;
	
	TB_Reg* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, params, param_count * sizeof(TB_Reg));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Reg r = tb_make_reg(f, TB_CALL, dt);
	f->nodes.data[r].call = (struct TB_NodeFunctionCall) { param_start, param_end, target };
	return r;
}

TB_API TB_Reg tb_inst_vcall(TB_Function* f, TB_DataType dt, const TB_Reg target, size_t param_count, const TB_Reg* params) {
	int param_start = f->vla.count;
	
	TB_Reg* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, params, param_count * sizeof(TB_Reg));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Reg r = tb_make_reg(f, TB_VCALL, dt);
	f->nodes.data[r].vcall = (struct TB_NodeDynamicCall) { param_start, param_end, target };
	return r;
}

TB_API TB_Reg tb_inst_ecall(TB_Function* f, TB_DataType dt, const TB_ExternalID target, size_t param_count, const TB_Reg* params) {
	int param_start = f->vla.count;
	
	TB_Reg* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, params, param_count * sizeof(TB_Reg));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Reg r = tb_make_reg(f, TB_ECALL, dt);
	f->nodes.data[r].ecall = (struct TB_NodeExternCall) { param_start, param_end, target };
	return r;
}

TB_API void tb_inst_memset(TB_Function* f, TB_Reg dst, TB_Reg val, TB_Reg size, TB_CharUnits align) {
	TB_Reg r = tb_make_reg(f, TB_MEMSET, TB_TYPE_PTR);
	f->nodes.data[r].mem_op = (struct TB_NodeMemoryOp) { dst, val, size, align };
}

TB_API void tb_inst_memcpy(TB_Function* f, TB_Reg dst, TB_Reg src, TB_Reg size, TB_CharUnits align) {
	TB_Reg r = tb_make_reg(f, TB_MEMCPY, TB_TYPE_PTR);
	f->nodes.data[r].mem_op = (struct TB_NodeMemoryOp) { dst, src, size, align };
}

TB_API void tb_inst_memclr(TB_Function* f, TB_Reg addr, TB_CharUnits size, TB_CharUnits align) {
	TB_Reg r = tb_make_reg(f, TB_MEMCLR, TB_TYPE_PTR);
	f->nodes.data[r].clear = (struct TB_NodeMemoryClear) { addr, size, align };
}

TB_API TB_Reg tb_inst_not(TB_Function* f, TB_Reg n) {
	TB_DataType dt = f->nodes.data[n].dt;
	
	TB_Reg r = tb_make_reg(f, TB_NOT, dt);
	f->nodes.data[r].unary = (struct TB_NodeUnary) { n };
	return r;
}

TB_API TB_Reg tb_inst_neg(TB_Function* f, TB_Reg n) {
	TB_DataType dt = f->nodes.data[n].dt;
	
	if (f->nodes.data[n].type == TB_SIGNED_CONST) {
		return tb_inst_sint(f, dt, -f->nodes.data[n].sint.value);
	} else if (f->nodes.data[n].type == TB_FLOAT_CONST) {
		return tb_inst_float(f, dt, -f->nodes.data[n].flt.value);
	}
	
	TB_Reg r = tb_make_reg(f, TB_NEG, dt);
	f->nodes.data[r].unary = (struct TB_NodeUnary) { n };
	return r;
}

TB_API TB_Reg tb_inst_and(TB_Function* f, TB_Reg a, TB_Reg b) {
	// bitwise operators can't wrap
	return tb_bin_arith(f, TB_AND, TB_ASSUME_NUW, a, b);
}

TB_API TB_Reg tb_inst_or(TB_Function* f, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	TB_DataType dt = f->nodes.data[a].dt;
	
	if ((f->nodes.data[a].type == TB_SIGNED_CONST || f->nodes.data[a].type == TB_UNSIGNED_CONST) && 
		(f->nodes.data[b].type == TB_SIGNED_CONST || f->nodes.data[b].type == TB_UNSIGNED_CONST)) {
		return tb_inst_sint(f, dt, f->nodes.data[a].uint.value | f->nodes.data[b].uint.value);
	}
	
	return tb_bin_arith(f, TB_OR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Reg tb_inst_xor(TB_Function* f, TB_Reg a, TB_Reg b) {
	// bitwise operators can't wrap
	return tb_bin_arith(f, TB_XOR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Reg tb_inst_select(TB_Function* f, TB_Reg cond, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_SELECT, dt);
	f->nodes.data[r].select = (struct TB_NodeSelect) { a, b, cond };
	return r;
}

TB_API TB_Reg tb_inst_add(TB_Function* f, TB_Reg a, TB_Reg b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_ADD, arith_behavior, a, b);
}

TB_API TB_Reg tb_inst_sub(TB_Function* f, TB_Reg a, TB_Reg b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_SUB, arith_behavior, a, b);
}

TB_API TB_Reg tb_inst_mul(TB_Function* f, TB_Reg a, TB_Reg b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_MUL, arith_behavior, a, b);
}

TB_API TB_Reg tb_inst_div(TB_Function* f, TB_Reg a, TB_Reg b, bool signedness) {
	if (f->nodes.data[b].type == TB_SIGNED_CONST && f->nodes.data[b].sint.value == 1) {
		return a;
	} else if (f->nodes.data[b].type == TB_UNSIGNED_CONST && f->nodes.data[b].uint.value == 1) {
		return a;
	}
	
	// division can't wrap or overflow
	return tb_bin_arith(f, signedness ? TB_SDIV : TB_UDIV, TB_ASSUME_NUW, a, b);
}

TB_API TB_Reg tb_inst_mod(TB_Function* f, TB_Reg a, TB_Reg b, bool signedness) {
	// modulo can't wrap or overflow
	return tb_bin_arith(f, signedness ? TB_SMOD : TB_UMOD, TB_ASSUME_NUW, a, b);
}

TB_API TB_Reg tb_inst_shl(TB_Function* f, TB_Reg a, TB_Reg b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_SHL, arith_behavior, a, b);
}

////////////////////////////////
// Atomics
////////////////////////////////
TB_API TB_Reg tb_inst_atomic_test_and_set(TB_Function* f, TB_Reg addr, TB_MemoryOrder order) {
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_TEST_AND_SET, TB_TYPE_BOOL);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = TB_NULL_REG;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Reg tb_inst_atomic_clear(TB_Function* f, TB_Reg addr, TB_MemoryOrder order) {
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_CLEAR, TB_TYPE_BOOL);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = TB_NULL_REG;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Reg tb_inst_atomic_xchg(TB_Function* f, TB_Reg addr, TB_Reg src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.data[src].dt;
	
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_XCHG, dt);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = src;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Reg tb_inst_atomic_add(TB_Function* f, TB_Reg addr, TB_Reg src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.data[src].dt;
	
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_ADD, dt);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = src;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Reg tb_inst_atomic_sub(TB_Function* f, TB_Reg addr, TB_Reg src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.data[src].dt;
	
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_SUB, dt);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = src;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Reg tb_inst_atomic_and(TB_Function* f, TB_Reg addr, TB_Reg src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.data[src].dt;
	
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_AND, dt);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = src;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Reg tb_inst_atomic_xor(TB_Function* f, TB_Reg addr, TB_Reg src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.data[src].dt;
	
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_XOR, dt);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = src;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Reg tb_inst_atomic_or(TB_Function* f, TB_Reg addr, TB_Reg src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.data[src].dt;
	
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_OR, dt);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = src;
	f->nodes.data[r].atomic.order = order;
	f->nodes.data[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_CmpXchgResult tb_inst_atomic_cmpxchg(TB_Function* f, TB_Reg addr, TB_Reg expected, TB_Reg desired, TB_MemoryOrder succ, TB_MemoryOrder fail) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[desired].dt, f->nodes.data[expected].dt));
	TB_DataType dt = f->nodes.data[desired].dt;
	
	TB_Reg r = tb_make_reg(f, TB_ATOMIC_CMPXCHG, TB_TYPE_BOOL);
	TB_Reg r2 = tb_make_reg(f, TB_ATOMIC_CMPXCHG2, dt);
	
	tb_assume(r+1 == r2);
	f->nodes.data[r].atomic.addr = addr;
	f->nodes.data[r].atomic.src = expected;
	f->nodes.data[r].atomic.order = succ;
	f->nodes.data[r].atomic.order2 = fail;
	
	f->nodes.data[r2].atomic.addr = addr;
	f->nodes.data[r2].atomic.src = desired;
	f->nodes.data[r2].atomic.order = succ;
	f->nodes.data[r2].atomic.order2 = fail;
	return (TB_CmpXchgResult) { .success = r, .old_value = r2 };
}

// TODO(NeGate): Maybe i should split the bitshift operations into a separate kind of
// operator that has different arithmatic behaviors, maybe like trap on a large shift amount
TB_API TB_Reg tb_inst_sar(TB_Function* f, TB_Reg a, TB_Reg b) {
	// shift right can't wrap or overflow
	return tb_bin_arith(f, TB_SAR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Reg tb_inst_shr(TB_Function* f, TB_Reg a, TB_Reg b) {
	// shift right can't wrap or overflow
	return tb_bin_arith(f, TB_SHR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Reg tb_inst_fadd(TB_Function* f, TB_Reg a, TB_Reg b) {
	return tb_bin_farith(f, TB_FADD, a, b);
}

TB_API TB_Reg tb_inst_fsub(TB_Function* f, TB_Reg a, TB_Reg b) {
	return tb_bin_farith(f, TB_FSUB, a, b);
}

TB_API TB_Reg tb_inst_fmul(TB_Function* f, TB_Reg a, TB_Reg b) {
	return tb_bin_farith(f, TB_FMUL, a, b);
}

TB_API TB_Reg tb_inst_fdiv(TB_Function* f, TB_Reg a, TB_Reg b) {
	return tb_bin_farith(f, TB_FDIV, a, b);
}

TB_API TB_Reg tb_inst_va_start(TB_Function* f, TB_Reg a) {
	assert(f->nodes.data[a].type == TB_PARAM_ADDR);
	
	TB_Reg r = tb_make_reg(f, TB_VA_START, TB_TYPE_PTR);
	f->nodes.data[r].unary = (struct TB_NodeUnary) { a };
	return r;
}

TB_API TB_Reg tb_inst_x86_sqrt(TB_Function* f, TB_Reg a) {
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_X86INTRIN_SQRT, dt);
	f->nodes.data[r].unary = (struct TB_NodeUnary) { a };
	return r;
}

TB_API TB_Reg tb_inst_x86_rsqrt(TB_Function* f, TB_Reg a) {
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_X86INTRIN_RSQRT, dt);
	f->nodes.data[r].unary = (struct TB_NodeUnary) { a };
	return r;
}

TB_API TB_Reg tb_inst_cmp_eq(TB_Function* f, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_CMP_EQ, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = a;
	f->nodes.data[r].cmp.b = b;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_ne(TB_Function* f, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_CMP_NE, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = a;
	f->nodes.data[r].cmp.b = b;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_ilt(TB_Function* f, TB_Reg a, TB_Reg b, bool signedness) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_INTEGER_TYPE(f->nodes.data[a].dt.type) || f->nodes.data[a].dt.type == TB_PTR);
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, signedness ? TB_CMP_SLT : TB_CMP_ULT, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = a;
	f->nodes.data[r].cmp.b = b;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_ile(TB_Function* f, TB_Reg a, TB_Reg b, bool signedness) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_INTEGER_TYPE(f->nodes.data[a].dt.type) || f->nodes.data[a].dt.type == TB_PTR);
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, signedness ? TB_CMP_SLE : TB_CMP_ULE, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = a;
	f->nodes.data[r].cmp.b = b;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_igt(TB_Function* f, TB_Reg a, TB_Reg b, bool signedness) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_INTEGER_TYPE(f->nodes.data[a].dt.type) || f->nodes.data[a].dt.type == TB_PTR);
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, signedness ? TB_CMP_SLT : TB_CMP_ULT, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = b;
	f->nodes.data[r].cmp.b = a;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_ige(TB_Function* f, TB_Reg a, TB_Reg b, bool signedness) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_INTEGER_TYPE(f->nodes.data[a].dt.type) || f->nodes.data[a].dt.type == TB_PTR);
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, signedness ? TB_CMP_SLE : TB_CMP_ULE, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = b;
	f->nodes.data[r].cmp.b = a;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_flt(TB_Function* f, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_FLOAT_TYPE(f->nodes.data[a].dt.type));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_CMP_FLT, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = a;
	f->nodes.data[r].cmp.b = b;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_fle(TB_Function* f, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_FLOAT_TYPE(f->nodes.data[a].dt.type));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_CMP_FLE, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = a;
	f->nodes.data[r].cmp.b = b;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_fgt(TB_Function* f, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_FLOAT_TYPE(f->nodes.data[a].dt.type));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_CMP_FLT, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = b;
	f->nodes.data[r].cmp.b = a;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_cmp_fge(TB_Function* f, TB_Reg a, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	tb_assume(TB_IS_FLOAT_TYPE(f->nodes.data[a].dt.type));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_CMP_FLE, TB_TYPE_BOOL);
	f->nodes.data[r].cmp.a = b;
	f->nodes.data[r].cmp.b = a;
	f->nodes.data[r].cmp.dt = dt;
	return r;
}

TB_API TB_Reg tb_inst_phi2(TB_Function* f, TB_Label a_label, TB_Reg a, TB_Label b_label, TB_Reg b) {
	tb_assume(TB_DATA_TYPE_EQUALS(f->nodes.data[a].dt, f->nodes.data[b].dt));
	TB_DataType dt = f->nodes.data[a].dt;
	
	TB_Reg r = tb_make_reg(f, TB_PHI2, dt);
	f->nodes.data[r].phi2.a_label = tb_find_reg_from_label(f, a_label);
	f->nodes.data[r].phi2.a = a;
	f->nodes.data[r].phi2.b_label = tb_find_reg_from_label(f, b_label);
	f->nodes.data[r].phi2.b = b;
	
	return r;
}

TB_API TB_Label tb_inst_new_label_id(TB_Function* f) {
	return f->label_count++;
}

TB_API TB_Reg tb_inst_label(TB_Function* f, TB_Label id) {
	tb_assume(id >= 1 && id < f->label_count);
	
	TB_Reg r = tb_make_reg(f, TB_LABEL, TB_TYPE_PTR);
	f->nodes.data[r].label = (struct TB_NodeLabel){ .id = id };
	
	if (f->current_label) {
		f->nodes.data[f->current_label].label.terminator = r;
	}
	
	f->current_label = r;
	return r;
}

TB_API void tb_inst_goto(TB_Function* f, TB_Label id) {
	tb_assume(id >= 0 && id < f->label_count);
	if (f->current_label == TB_NULL_REG) {
		// Was placed after a terminator instruction,
		// just omit this to avoid any issues since it's
		// not a big deal for example:
		// RET x
		// ~~GOTO .L5~~
		// .L4:
		return;
	}
	
	TB_Reg r = tb_make_reg(f, TB_GOTO, TB_TYPE_VOID);
	f->nodes.data[r].goto_.label = id;
	
	tb_assume(f->current_label);
	f->nodes.data[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
}

TB_API TB_Reg tb_inst_if(TB_Function* f, TB_Reg cond, TB_Label if_true, TB_Label if_false) {
	TB_Reg r = tb_make_reg(f, TB_IF, TB_TYPE_VOID);
	f->nodes.data[r].if_.cond = cond;
	f->nodes.data[r].if_.if_true = if_true;
	f->nodes.data[r].if_.if_false = if_false;
	
	tb_assume(f->current_label);
	f->nodes.data[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
	return r;
}

TB_API void tb_inst_switch(TB_Function* f, TB_DataType dt, TB_Reg key, TB_Label default_label, size_t entry_count, const TB_SwitchEntry* entries) {
	// the switch entries are 2 slots each
	size_t param_count = entry_count * 2;
	int param_start = f->vla.count;
	
	TB_Reg* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, entries, param_count * sizeof(TB_Reg));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Reg r = tb_make_reg(f, TB_SWITCH, dt);
	f->nodes.data[r].switch_.key = key;
	f->nodes.data[r].switch_.default_label = default_label;
	f->nodes.data[r].switch_.entries_start = param_start;
	f->nodes.data[r].switch_.entries_end = param_end;
	
	tb_assume(f->current_label);
	f->nodes.data[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
}

TB_API void tb_inst_ret(TB_Function* f, TB_Reg value) {
	TB_Reg r = tb_make_reg(f, TB_RET, f->prototype->return_dt);
	f->nodes.data[r].ret.value = value;
	
	tb_assume(f->current_label);
	f->nodes.data[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
}

#if !TB_STRIP_LABELS
void tb_emit_label_symbol(TB_Module* m, uint32_t func_id, uint32_t label_id, size_t pos) {
	tb_assume(pos < UINT32_MAX);
	TB_LabelSymbol sym = {
		.func_id = func_id,
		.label_id = label_id,
		.pos = pos
	};
	
	arrput(m->label_symbols, sym);
}
#endif
