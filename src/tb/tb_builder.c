// IR BUILDER
// 
// Handles generating the TB_Function IR via C functions. 
// Note that these functions can perform certain simple
// optimizations while the generation happens to improve
// the machine code output or later analysis stages.
#include "tb_internal.h"

TB_API TB_FunctionID tb_function_get_id(TB_Module* m, TB_Function* f) {
	intptr_t id = f - m->functions.data;
	assert(id == (TB_FunctionID) id);
	return id;
}

TB_API TB_Function* tb_function_from_id(TB_Module* m, TB_FunctionID id) {
	assert(id < m->functions.count);
	return &m->functions.data[id];
}

TB_API TB_RegTypeEnum tb_node_get_type(TB_Function* f, TB_Register r) {
	assert(r < f->nodes.count);
	return f->nodes.type[r];
}

TB_API TB_DataType tb_node_get_data_type(TB_Function* f, TB_Register r) {
	assert(r < f->nodes.count);
	return f->nodes.dt[r];
}

TB_API void tb_get_function_get_local_info(TB_Function* f, TB_Register r, int* size, int* align) {
	assert(f->nodes.type[r] == TB_LOCAL);
	
	*size = f->nodes.payload[r].local.size;
	*align = f->nodes.payload[r].local.alignment;
}

TB_API bool tb_node_is_conditional(TB_Function* f, TB_Register r) {
	return f->nodes.type[r] == TB_IF;
}

TB_API bool tb_node_is_terminator(TB_Function* f, TB_Register r) {
	return f->nodes.type[r] == TB_IF ||
		f->nodes.type[r] == TB_GOTO ||
		f->nodes.type[r] == TB_RET ||
		f->nodes.type[r] == TB_LABEL;
}

TB_API bool tb_node_is_label(TB_Function* f, TB_Register r) {
	return f->nodes.type[r] == TB_LABEL;
}

TB_API TB_Register tb_node_get_last_register(TB_Function* f) {
	return f->nodes.count - 1;
}

TB_API TB_Register tb_node_load_get_address(TB_Function* f, TB_Register r) {
	assert(f->nodes.type[r] == TB_LOAD);
	
	return f->nodes.payload[r].load.address;
}

TB_API TB_Register tb_node_arith_get_left(TB_Function* f, TB_Register r) {
	assert(f->nodes.type[r] >= TB_AND && f->nodes.type[r] <= TB_CMP_FLE);
	
	// TODO(NeGate): They share position in the union
	static_assert(offsetof(TB_RegPayload, cmp.a) == offsetof(TB_RegPayload, i_arith.a), "TB_RegPayload::cmp.a should alias TB_RegPayload::i_arith.a");
	static_assert(offsetof(TB_RegPayload, f_arith.a) == offsetof(TB_RegPayload, i_arith.a), "TB_RegPayload::f_arith.a should alias TB_RegPayload::i_arith.a");
	
	return f->nodes.payload[r].i_arith.a;
}

TB_API TB_Register tb_node_arith_get_right(TB_Function* f, TB_Register r) {
	assert(f->nodes.type[r] >= TB_AND && f->nodes.type[r] <= TB_CMP_FLE);
	
	// TODO(NeGate): They share position in the union
	static_assert(offsetof(TB_RegPayload, cmp.b) == offsetof(TB_RegPayload, i_arith.b), "TB_RegPayload::cmp.b should alias TB_RegPayload::i_arith.b");
	static_assert(offsetof(TB_RegPayload, f_arith.b) == offsetof(TB_RegPayload, i_arith.b), "TB_RegPayload::f_arith.b should alias TB_RegPayload::i_arith.b");
	
	return f->nodes.payload[r].i_arith.b;
}

static TB_Register tb_make_reg(TB_Function* f, int type, TB_DataType dt) {
	// Cannot add registers to terminated basic blocks, except labels
	// which start new basic blocks
	assert(f);
	if (type != TB_LABEL && f->current_label == 0) {
		fprintf(stderr, "Cannot create node without parent basic block\n");
		tb_function_print(f, tb_default_print_callback, stderr);
		fprintf(stderr, "\n\n\n");
		abort();
	}
    
	if (f->nodes.count + 1 >= f->nodes.capacity) {
		tb_resize_node_stream(f, tb_next_pow2(f->nodes.count + 1));
	}
	
	TB_Register r = f->nodes.count++;
	f->nodes.type[r] = type;
	f->nodes.dt[r] = dt;
	return r;
}

uint64_t tb_fold_add(TB_ArithmaticBehavior ab, TB_DataType dt, uint64_t a, uint64_t b) {
	uint64_t shift = 64 - (8 << (dt.type - TB_I8));
	uint64_t mask = (~0ull) >> shift;
	
	uint64_t sum;
	if (tb_add_overflow(a << shift, b << shift, &sum)) {
		sum >>= shift;
		
		if (ab == TB_CAN_WRAP) sum &= mask;
		else if (ab == TB_SATURATED_UNSIGNED) sum = mask;
		//else if (ab == TB_WRAP_CHECK) { printf("warp check!!!\n"); }
	}
	
	sum = (sum >> shift) & mask;
	return sum;
}

uint64_t tb_fold_sub(TB_ArithmaticBehavior ab, TB_DataType dt, uint64_t a, uint64_t b) {
	uint64_t shift = 64 - (8 << (dt.type - TB_I8));
	uint64_t mask = (~0ull) >> shift;
	
	uint64_t sum;
	if (tb_sub_overflow(a << shift, b << shift, &sum)) {
		sum = (sum >> shift) & mask;
		
		if (ab == TB_CAN_WRAP) sum &= mask;
		else if (ab == TB_SATURATED_UNSIGNED) sum = 0;
		//else if (ab == TB_WRAP_CHECK) { printf("warp check!!!\n"); }
	} else {
		sum = (sum >> shift) & mask;
	}
	
	return sum;
}

uint64_t tb_fold_mul(TB_ArithmaticBehavior ab, TB_DataType dt, uint64_t a, uint64_t b) {
	uint64_t shift = 64 - (8 << (dt.type - TB_I8));
	uint64_t mask = (~0ull) >> shift;
	
	uint64_t sum;
	if (tb_mul_overflow(a << shift, b << shift, &sum)) {
		sum = (sum >> shift) & mask;
		
		if (ab == TB_CAN_WRAP) sum &= mask;
		else if (ab == TB_SATURATED_UNSIGNED) sum = 0;
		//else if (ab == TB_WRAP_CHECK) { printf("warp check!!!\n"); }
	} else {
		sum = (sum >> shift) & mask;
	}
	
	return sum;
}

uint64_t tb_fold_div(TB_DataType dt, uint64_t a, uint64_t b) {
	uint64_t shift = 64 - (8 << (dt.type - TB_I8));
	uint64_t mask = (~0ull) >> shift;
	
	if (b == 0) return 0;
	uint64_t q = (a << shift) / (b << shift);
	
	return (q >> shift) & mask; 
}

static TB_Register tb_bin_arith(TB_Function* f, int type, TB_ArithmaticBehavior arith_behavior, TB_Register a, TB_Register b) {
	//assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	if (!TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b])) {
		tb_function_print(f, tb_default_print_callback, stderr);
		abort();
	}
	
	TB_Register r = tb_make_reg(f, type, f->nodes.dt[a]);
	f->nodes.payload[r].i_arith.arith_behavior = arith_behavior;
	f->nodes.payload[r].i_arith.a = a;
	f->nodes.payload[r].i_arith.b = b;
	return r;
}

static TB_Register tb_bin_farith(TB_Function* f, int type, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	
	TB_Register r = tb_make_reg(f, type, f->nodes.dt[a]);
	f->nodes.payload[r].f_arith.a = a;
	f->nodes.payload[r].f_arith.b = b;
	return r;
}

TB_API TB_Register tb_inst_trunc(TB_Function* f, TB_Register src, TB_DataType dt) {
	assert(f->nodes.dt[src].width == dt.width);
	
	TB_Register r = tb_make_reg(f, TB_TRUNCATE, dt);
	f->nodes.payload[r].trunc = src;
	return r;
}

TB_API TB_Register tb_inst_int2ptr(TB_Function* f, TB_Register src) {
	assert(f->nodes.dt[src].width == 0);
	
	TB_Register r = tb_make_reg(f, TB_INT2PTR, TB_TYPE_PTR);
	f->nodes.payload[r].ptrcast = src;
	return r;
}

TB_API TB_Register tb_inst_ptr2int(TB_Function* f, TB_Register src, TB_DataType dt) {
	assert(dt.width == 0);
	assert(f->nodes.dt[src].width == 0);
	
	TB_Register r = tb_make_reg(f, TB_PTR2INT, dt);
	f->nodes.payload[r].ptrcast = src;
	return r;
}

TB_API TB_Register tb_inst_int2float(TB_Function* f, TB_Register src, TB_DataType dt) {
	assert(f->nodes.dt[src].width == dt.width);
	
	if (f->nodes.type[src] == TB_SIGNED_CONST) {
		return tb_inst_float(f, dt, f->nodes.payload[src].s_const);
	} else if (f->nodes.type[src] == TB_UNSIGNED_CONST) {
		return tb_inst_float(f, dt, f->nodes.payload[src].u_const);
	}
	
	TB_Register r = tb_make_reg(f, TB_INT2FLOAT, dt);
	f->nodes.payload[r].cvt.src = src;
	return r;
}

TB_API TB_Register tb_inst_float2int(TB_Function* f, TB_Register src, TB_DataType dt) {
	assert(f->nodes.dt[src].width == dt.width);
	
	TB_Register r = tb_make_reg(f, TB_FLOAT2INT, dt);
	f->nodes.payload[r].cvt.src = src;
	return r;
}

TB_API TB_Register tb_inst_fpxt(TB_Function* f, TB_Register src, TB_DataType dt) {
	assert(dt.width == f->nodes.dt[src].width);
	
	TB_Register r = tb_make_reg(f, TB_FLOAT_EXT, dt);
	f->nodes.payload[r].ext = src;
	return r;
}

TB_API TB_Register tb_inst_sxt(TB_Function* f, TB_Register src, TB_DataType dt) {
	assert(dt.width == f->nodes.dt[src].width);
	
	TB_Register r = tb_make_reg(f, TB_SIGN_EXT, dt);
	f->nodes.payload[r].ext = src;
	return r;
}

TB_API TB_Register tb_inst_zxt(TB_Function* f, TB_Register src, TB_DataType dt) {
	assert(dt.width == f->nodes.dt[src].width);
	
	TB_Register r = tb_make_reg(f, TB_ZERO_EXT, dt);
	f->nodes.payload[r].ext = src;
	return r;
}

TB_API TB_Register tb_inst_bitcast(TB_Function* f, TB_Register src, TB_DataType dt) {
	// TODO(NeGate): Do some size checks
	TB_Register r = tb_make_reg(f, TB_BITCAST, dt);
	f->nodes.payload[r].cvt.src = src;
	return r;
}

TB_API TB_Register tb_inst_param(TB_Function* f, int param_id) {
	assert(param_id < f->prototype->param_count);
	return 2 + param_id;
}

TB_API TB_Register tb_inst_param_addr(TB_Function* f, int param_id) {
	assert(param_id < f->prototype->param_count);
	
	TB_Register param = 2 + param_id;
	int param_size = f->nodes.payload[param].param.size;
	
	TB_Register r = tb_make_reg(f, TB_PARAM_ADDR, TB_TYPE_PTR);
	f->nodes.payload[r].param_addr.param = param;
	f->nodes.payload[r].param_addr.size = param_size;
	f->nodes.payload[r].param_addr.alignment = param_size;
	return r;
}

TB_API void tb_inst_debugbreak(TB_Function* f) {
	tb_make_reg(f, TB_DEBUGBREAK, TB_TYPE_VOID);
}

TB_API void tb_inst_loc(TB_Function* f, TB_FileID file, int line) {
	assert(line >= 0);
	if (f->nodes.type[f->nodes.count - 1] == TB_LINE_INFO) {
		return;
	}
	
	TB_Register r = tb_make_reg(f, TB_LINE_INFO, TB_TYPE_VOID);
	f->nodes.payload[r].line_info.file = file;
	f->nodes.payload[r].line_info.line = line;
	f->nodes.payload[r].line_info.pos = 0;
	
	f->module->line_info_count++;
}

TB_API TB_Register tb_inst_local(TB_Function* f, uint32_t size, TB_CharUnits alignment) {
	assert(size > 0);
	assert(alignment > 0 && tb_is_power_of_two(alignment));
	
	TB_Register r = tb_make_reg(f, TB_LOCAL, TB_TYPE_PTR);
	f->nodes.payload[r].local.alignment = alignment;
	f->nodes.payload[r].local.size = size;
	return r;
}

TB_API TB_Register tb_inst_restrict(TB_Function* f, TB_Register value) {
	TB_Register r = tb_make_reg(f, TB_RESTRICT, TB_TYPE_PTR);
	f->nodes.payload[r].restrict_ = value;
	return r;
}

TB_API TB_Register tb_inst_load(TB_Function* f, TB_DataType dt, TB_Register addr, TB_CharUnits alignment) {
	assert(f->current_label);
	
	TB_Register r = tb_make_reg(f, TB_LOAD, dt);
	f->nodes.payload[r].load.address = addr;
	f->nodes.payload[r].load.alignment = alignment;
	return r;
}

TB_API void tb_inst_store(TB_Function* f, TB_DataType dt, TB_Register addr, TB_Register val, uint32_t alignment) {
	assert(addr);
	assert(val);
	
	TB_Register r = tb_make_reg(f, TB_STORE, dt);
	f->nodes.payload[r].store.address = addr;
	f->nodes.payload[r].store.value = val;
	f->nodes.payload[r].store.alignment = alignment;
	return;
}

TB_API TB_Register tb_inst_volatile_load(TB_Function* f, TB_DataType dt, TB_Register addr, TB_CharUnits alignment) {
	assert(f->current_label);
	
	TB_Register r = tb_make_reg(f, TB_LOAD, dt);
	f->nodes.payload[r].load.address = addr;
	f->nodes.payload[r].load.alignment = alignment;
	f->nodes.payload[r].load.is_volatile = true;
	return r;
}

TB_API void tb_inst_volatile_store(TB_Function* f, TB_DataType dt, TB_Register addr, TB_Register val, TB_CharUnits alignment) {
	TB_Register r = tb_make_reg(f, TB_STORE, dt);
	f->nodes.payload[r].store.address = addr;
	f->nodes.payload[r].store.value = val;
	f->nodes.payload[r].store.alignment = alignment;
	f->nodes.payload[r].store.is_volatile = true;
}

TB_API void tb_inst_initialize_mem(TB_Function* f, TB_Register addr, TB_InitializerID src) {
	TB_Register r = tb_make_reg(f, TB_INITIALIZE, TB_TYPE_PTR);
	f->nodes.payload[r].init.addr = addr;
	f->nodes.payload[r].init.id = src;
}

TB_API TB_Register tb_inst_bool(TB_Function* f, bool imm) {
	TB_Register r = tb_make_reg(f, TB_UNSIGNED_CONST, TB_TYPE_BOOL);
	f->nodes.payload[r].u_const = imm;
	return r;
}

TB_API TB_Register tb_inst_ptr(TB_Function* f, uint64_t imm) {
	TB_Register r = tb_make_reg(f, TB_UNSIGNED_CONST, TB_TYPE_PTR);
	f->nodes.payload[r].u_const = imm;
	return r;
}

TB_API TB_Register tb_inst_uint(TB_Function* f, TB_DataType dt, uint64_t imm) {
	assert(dt.type == TB_BOOL || dt.type == TB_PTR || (dt.type >= TB_I8 && dt.type <= TB_I64));
	
	TB_Register r = tb_make_reg(f, TB_UNSIGNED_CONST, dt);
	f->nodes.payload[r].u_const = imm;
	return r;
}

TB_API TB_Register tb_inst_sint(TB_Function* f, TB_DataType dt, int64_t imm) {
	assert(dt.type == TB_BOOL || dt.type == TB_PTR || (dt.type >= TB_I8 && dt.type <= TB_I64));
	
	TB_Register r = tb_make_reg(f, TB_SIGNED_CONST, dt);
	f->nodes.payload[r].s_const = imm;
	return r;
}

TB_API TB_Register tb_inst_float(TB_Function* f, TB_DataType dt, double imm) {
	TB_Register r = tb_make_reg(f, TB_FLOAT_CONST, dt);
	f->nodes.payload[r].f_const = imm;
	return r;
}

TB_API TB_Register tb_inst_cstring(TB_Function* f, const char* str) {
	size_t len = strlen(str);
	char* newstr = tb_platform_arena_alloc(len + 1);
	memcpy(newstr, str, len);
	newstr[len] = '\0';
	
	TB_Register r = tb_make_reg(f, TB_STRING_CONST, TB_TYPE_PTR);
	f->nodes.payload[r].str_const.len = len + 1;
	f->nodes.payload[r].str_const.data = newstr;
	return r;
}

TB_API TB_Register tb_inst_string(TB_Function* f, size_t len, const char* str) {
	char* newstr = tb_platform_arena_alloc(len);
	memcpy(newstr, str, len);
	
	TB_Register r = tb_make_reg(f, TB_STRING_CONST, TB_TYPE_PTR);
	f->nodes.payload[r].str_const.len = len;
	f->nodes.payload[r].str_const.data = newstr;
	return r;
}

TB_API TB_Register tb_inst_array_access(TB_Function* f, TB_Register base, TB_Register index, uint32_t stride) {
	TB_Register r = tb_make_reg(f, TB_ARRAY_ACCESS, TB_TYPE_PTR);
	f->nodes.payload[r].array_access.base = base;
	f->nodes.payload[r].array_access.index = index;
	f->nodes.payload[r].array_access.stride = stride;
	return r;
}

TB_API TB_Register tb_inst_member_access(TB_Function* f, TB_Register base, int32_t offset) {
	TB_Register r = tb_make_reg(f, TB_MEMBER_ACCESS, TB_TYPE_PTR);
	f->nodes.payload[r].member_access.base = base;
	f->nodes.payload[r].member_access.offset = offset;
	return r;
}

TB_API TB_Register tb_inst_get_func_address(TB_Function* f, const TB_Function* target) {
	TB_Register r = tb_make_reg(f, TB_FUNC_ADDRESS, TB_TYPE_PTR);
	f->nodes.payload[r].func_addr = target;
	return r;
}

TB_API TB_Register tb_inst_get_extern_address(TB_Function* f, TB_ExternalID target) {
	TB_Register r = tb_make_reg(f, TB_EFUNC_ADDRESS, TB_TYPE_PTR);
	f->nodes.payload[r].efunc_addr = target;
	return r;
}

TB_API TB_Register tb_inst_get_global_address(TB_Function* f, TB_GlobalID target) {
	TB_Register r = tb_make_reg(f, TB_GLOBAL_ADDRESS, TB_TYPE_PTR);
	f->nodes.payload[r].global_addr = target;
	return r;
}

TB_Register* tb_vla_reserve(TB_Function* f, size_t count) {
	// Reserve space for the arguments
	if (f->vla.count + count >= f->vla.capacity) {
		f->vla.capacity = f->vla.capacity ? tb_next_pow2(f->vla.count + count) : 16;
		f->vla.data = tb_platform_heap_realloc(f->vla.data, f->vla.capacity * sizeof(TB_Register));
	}
	
	return &f->vla.data[f->vla.count];
}

TB_API TB_Register tb_inst_call(TB_Function* f, TB_DataType dt, const TB_Function* target, size_t param_count, const TB_Register* params) {
	int param_start = f->vla.count;
	
	TB_Register* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, params, param_count * sizeof(TB_Register));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Register r = tb_make_reg(f, TB_CALL, dt);
	f->nodes.payload[r].call = (struct TB_NodeFunctionCall) { param_start, param_end, target };
	return r;
}

TB_API TB_Register tb_inst_vcall(TB_Function* f, TB_DataType dt, const TB_Register target, size_t param_count, const TB_Register* params) {
	int param_start = f->vla.count;
	
	TB_Register* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, params, param_count * sizeof(TB_Register));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Register r = tb_make_reg(f, TB_VCALL, dt);
	f->nodes.payload[r].vcall = (struct TB_NodeDynamicCall) { param_start, param_end, target };
	return r;
}

TB_API TB_Register tb_inst_ecall(TB_Function* f, TB_DataType dt, const TB_ExternalID target, size_t param_count, const TB_Register* params) {
	int param_start = f->vla.count;
	
	TB_Register* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, params, param_count * sizeof(TB_Register));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Register r = tb_make_reg(f, TB_ECALL, dt);
	f->nodes.payload[r].ecall = (struct TB_NodeExternCall) { param_start, param_end, target };
	return r;
}

TB_API void tb_inst_memset(TB_Function* f, TB_Register dst, TB_Register val, TB_Register size, TB_CharUnits align) {
	TB_Register r = tb_make_reg(f, TB_MEMSET, TB_TYPE_PTR);
	f->nodes.payload[r].mem_op = (struct TB_NodeMemXXX) { dst, val, size, align };
}

TB_API void tb_inst_memcpy(TB_Function* f, TB_Register dst, TB_Register src, TB_Register size, TB_CharUnits align) {
	TB_Register r = tb_make_reg(f, TB_MEMCPY, TB_TYPE_PTR);
	f->nodes.payload[r].mem_op = (struct TB_NodeMemXXX) { dst, src, size, align };
}

TB_API void tb_inst_memclr(TB_Function* f, TB_Register addr, TB_CharUnits size, TB_CharUnits align) {
	TB_Register r = tb_make_reg(f, TB_MEMCLR, TB_TYPE_PTR);
	f->nodes.payload[r].clear = (struct TB_NodeMemClear) { addr, size, align };
}

TB_API TB_Register tb_inst_not(TB_Function* f, TB_Register n) {
	TB_DataType dt = f->nodes.dt[n];
	
	TB_Register r = tb_make_reg(f, TB_NOT, dt);
	f->nodes.payload[r].unary = (struct TB_NodeUnary) { n };
	return r;
}

TB_API TB_Register tb_inst_neg(TB_Function* f, TB_Register n) {
	TB_DataType dt = f->nodes.dt[n];
	
	if (f->nodes.type[n] == TB_SIGNED_CONST) {
		return tb_inst_sint(f, dt, -f->nodes.payload[n].s_const);
	} else if (f->nodes.type[n] == TB_FLOAT_CONST) {
		return tb_inst_float(f, dt, -f->nodes.payload[n].f_const);
	}
	
	TB_Register r = tb_make_reg(f, TB_NEG, dt);
	f->nodes.payload[r].unary = (struct TB_NodeUnary) { n };
	return r;
}

TB_API TB_Register tb_inst_and(TB_Function* f, TB_Register a, TB_Register b) {
	// bitwise operators can't wrap
	return tb_bin_arith(f, TB_AND, TB_ASSUME_NUW, a, b);
}

TB_API TB_Register tb_inst_or(TB_Function* f, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	TB_DataType dt = f->nodes.dt[a];
	
	if ((f->nodes.type[a] == TB_SIGNED_CONST || f->nodes.type[a] == TB_UNSIGNED_CONST) && 
		(f->nodes.type[b] == TB_SIGNED_CONST || f->nodes.type[b] == TB_UNSIGNED_CONST)) {
		return tb_inst_sint(f, dt, f->nodes.payload[a].u_const | f->nodes.payload[b].u_const);
	}
	
	return tb_bin_arith(f, TB_OR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Register tb_inst_xor(TB_Function* f, TB_Register a, TB_Register b) {
	// bitwise operators can't wrap
	return tb_bin_arith(f, TB_XOR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Register tb_inst_select(TB_Function* f, TB_Register cond, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_SELECT, dt);
	f->nodes.payload[r].select = (struct TB_NodeSelect) { a, b, cond };
	return r;
}

TB_API TB_Register tb_inst_add(TB_Function* f, TB_Register a, TB_Register b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_ADD, arith_behavior, a, b);
}

TB_API TB_Register tb_inst_sub(TB_Function* f, TB_Register a, TB_Register b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_SUB, arith_behavior, a, b);
}

TB_API TB_Register tb_inst_mul(TB_Function* f, TB_Register a, TB_Register b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_MUL, arith_behavior, a, b);
}

TB_API TB_Register tb_inst_div(TB_Function* f, TB_Register a, TB_Register b, bool signedness) {
	if (f->nodes.type[b] == TB_SIGNED_CONST && f->nodes.payload[b].s_const == 1) {
		return a;
	} else if (f->nodes.type[b] == TB_UNSIGNED_CONST && f->nodes.payload[b].u_const == 1) {
		return a;
	}
	
	// division can't wrap or overflow
	return tb_bin_arith(f, signedness ? TB_SDIV : TB_UDIV, TB_ASSUME_NUW, a, b);
}

TB_API TB_Register tb_inst_mod(TB_Function* f, TB_Register a, TB_Register b, bool signedness) {
	// modulo can't wrap or overflow
	return tb_bin_arith(f, signedness ? TB_SMOD : TB_UMOD, TB_ASSUME_NUW, a, b);
}

TB_API TB_Register tb_inst_shl(TB_Function* f, TB_Register a, TB_Register b, TB_ArithmaticBehavior arith_behavior) {
	return tb_bin_arith(f, TB_SHL, arith_behavior, a, b);
}

////////////////////////////////
// Atomics
////////////////////////////////
TB_API TB_Register tb_inst_atomic_test_and_set(TB_Function* f, TB_Register addr, TB_MemoryOrder order) {
	TB_Register r = tb_make_reg(f, TB_ATOMIC_TEST_AND_SET, TB_TYPE_BOOL);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = TB_NULL_REG;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Register tb_inst_atomic_clear(TB_Function* f, TB_Register addr, TB_MemoryOrder order) {
	TB_Register r = tb_make_reg(f, TB_ATOMIC_CLEAR, TB_TYPE_BOOL);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = TB_NULL_REG;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Register tb_inst_atomic_xchg(TB_Function* f, TB_Register addr, TB_Register src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.dt[src];
	
	TB_Register r = tb_make_reg(f, TB_ATOMIC_XCHG, dt);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = src;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Register tb_inst_atomic_add(TB_Function* f, TB_Register addr, TB_Register src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.dt[src];
	
	TB_Register r = tb_make_reg(f, TB_ATOMIC_ADD, dt);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = src;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Register tb_inst_atomic_sub(TB_Function* f, TB_Register addr, TB_Register src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.dt[src];
	
	TB_Register r = tb_make_reg(f, TB_ATOMIC_SUB, dt);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = src;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Register tb_inst_atomic_and(TB_Function* f, TB_Register addr, TB_Register src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.dt[src];
	
	TB_Register r = tb_make_reg(f, TB_ATOMIC_AND, dt);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = src;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Register tb_inst_atomic_xor(TB_Function* f, TB_Register addr, TB_Register src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.dt[src];
	
	TB_Register r = tb_make_reg(f, TB_ATOMIC_XOR, dt);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = src;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_Register tb_inst_atomic_or(TB_Function* f, TB_Register addr, TB_Register src, TB_MemoryOrder order) {
	TB_DataType dt = f->nodes.dt[src];
	
	TB_Register r = tb_make_reg(f, TB_ATOMIC_OR, dt);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = src;
	f->nodes.payload[r].atomic.order = order;
	f->nodes.payload[r].atomic.order2 = TB_MEM_ORDER_SEQ_CST;
	return r;
}

TB_API TB_CmpXchgResult tb_inst_atomic_cmpxchg(TB_Function* f, TB_Register addr, TB_Register expected, TB_Register desired, TB_MemoryOrder succ, TB_MemoryOrder fail) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[desired], f->nodes.dt[expected]));
	TB_DataType dt = f->nodes.dt[desired];
	
	TB_Register r = tb_make_reg(f, TB_ATOMIC_CMPXCHG, TB_TYPE_BOOL);
	TB_Register r2 = tb_make_reg(f, TB_ATOMIC_CMPXCHG2, dt);
	
	assert(r+1 == r2);
	f->nodes.payload[r].atomic.addr = addr;
	f->nodes.payload[r].atomic.src = expected;
	f->nodes.payload[r].atomic.order = succ;
	f->nodes.payload[r].atomic.order2 = fail;
	
	f->nodes.payload[r2].atomic.addr = addr;
	f->nodes.payload[r2].atomic.src = desired;
	f->nodes.payload[r2].atomic.order = succ;
	f->nodes.payload[r2].atomic.order2 = fail;
	return (TB_CmpXchgResult) { .success = r, .old_value = r2 };
}

// TODO(NeGate): Maybe i should split the bitshift operations into a separate kind of
// operator that has different arithmatic behaviors, maybe like trap on a large shift amount
TB_API TB_Register tb_inst_sar(TB_Function* f, TB_Register a, TB_Register b) {
	// shift right can't wrap or overflow
	return tb_bin_arith(f, TB_SAR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Register tb_inst_shr(TB_Function* f, TB_Register a, TB_Register b) {
	// shift right can't wrap or overflow
	return tb_bin_arith(f, TB_SHR, TB_ASSUME_NUW, a, b);
}

TB_API TB_Register tb_inst_fadd(TB_Function* f, TB_Register a, TB_Register b) {
	return tb_bin_farith(f, TB_FADD, a, b);
}

TB_API TB_Register tb_inst_fsub(TB_Function* f, TB_Register a, TB_Register b) {
	return tb_bin_farith(f, TB_FSUB, a, b);
}

TB_API TB_Register tb_inst_fmul(TB_Function* f, TB_Register a, TB_Register b) {
	return tb_bin_farith(f, TB_FMUL, a, b);
}

TB_API TB_Register tb_inst_fdiv(TB_Function* f, TB_Register a, TB_Register b) {
	return tb_bin_farith(f, TB_FDIV, a, b);
}

TB_API TB_Register tb_inst_x86_sqrt(TB_Function* f, TB_Register a) {
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_X86INTRIN_SQRT, dt);
	f->nodes.payload[r].unary = (struct TB_NodeUnary) { a };
	return r;
}

TB_API TB_Register tb_inst_x86_rsqrt(TB_Function* f, TB_Register a) {
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_X86INTRIN_RSQRT, dt);
	f->nodes.payload[r].unary = (struct TB_NodeUnary) { a };
	return r;
}

TB_API TB_Register tb_inst_cmp_eq(TB_Function* f, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_CMP_EQ, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = a;
	f->nodes.payload[r].cmp.b = b;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_ne(TB_Function* f, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_CMP_NE, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = a;
	f->nodes.payload[r].cmp.b = b;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_ilt(TB_Function* f, TB_Register a, TB_Register b, bool signedness) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_INTEGER_TYPE(f->nodes.dt[a].type) || f->nodes.dt[a].type == TB_PTR);
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, signedness ? TB_CMP_SLT : TB_CMP_ULT, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = a;
	f->nodes.payload[r].cmp.b = b;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_ile(TB_Function* f, TB_Register a, TB_Register b, bool signedness) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_INTEGER_TYPE(f->nodes.dt[a].type) || f->nodes.dt[a].type == TB_PTR);
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, signedness ? TB_CMP_SLE : TB_CMP_ULE, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = a;
	f->nodes.payload[r].cmp.b = b;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_igt(TB_Function* f, TB_Register a, TB_Register b, bool signedness) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_INTEGER_TYPE(f->nodes.dt[a].type) || f->nodes.dt[a].type == TB_PTR);
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, signedness ? TB_CMP_SLT : TB_CMP_ULT, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = b;
	f->nodes.payload[r].cmp.b = a;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_ige(TB_Function* f, TB_Register a, TB_Register b, bool signedness) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_INTEGER_TYPE(f->nodes.dt[a].type) || f->nodes.dt[a].type == TB_PTR);
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, signedness ? TB_CMP_SLE : TB_CMP_ULE, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = b;
	f->nodes.payload[r].cmp.b = a;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_flt(TB_Function* f, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_FLOAT_TYPE(f->nodes.dt[a].type));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_CMP_FLT, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = a;
	f->nodes.payload[r].cmp.b = b;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_fle(TB_Function* f, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_FLOAT_TYPE(f->nodes.dt[a].type));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_CMP_FLE, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = a;
	f->nodes.payload[r].cmp.b = b;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_fgt(TB_Function* f, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_FLOAT_TYPE(f->nodes.dt[a].type));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_CMP_FLT, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = b;
	f->nodes.payload[r].cmp.b = a;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_cmp_fge(TB_Function* f, TB_Register a, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	assert(TB_IS_FLOAT_TYPE(f->nodes.dt[a].type));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_CMP_FLE, TB_TYPE_BOOL);
	f->nodes.payload[r].cmp.a = b;
	f->nodes.payload[r].cmp.b = a;
	f->nodes.payload[r].cmp.dt = dt;
	return r;
}

TB_API TB_Register tb_inst_phi2(TB_Function* f, TB_Label a_label, TB_Register a, TB_Label b_label, TB_Register b) {
	assert(TB_DATA_TYPE_EQUALS(f->nodes.dt[a], f->nodes.dt[b]));
	TB_DataType dt = f->nodes.dt[a];
	
	TB_Register r = tb_make_reg(f, TB_PHI2, dt);
	f->nodes.payload[r].phi2.a_label = tb_find_reg_from_label(f, a_label);
	f->nodes.payload[r].phi2.a = a;
	f->nodes.payload[r].phi2.b_label = tb_find_reg_from_label(f, b_label);
	f->nodes.payload[r].phi2.b = b;
	
	return r;
}

TB_API TB_Label tb_inst_new_label_id(TB_Function* f) {
	return f->label_count++;
}

TB_API TB_Register tb_inst_label(TB_Function* f, TB_Label id) {
	assert(id >= 0 && id < f->label_count);
	
	TB_Register r = tb_make_reg(f, TB_LABEL, TB_TYPE_PTR);
	f->nodes.payload[r].label.id = id;
	f->nodes.payload[r].label.terminator = TB_NULL_REG;
	f->nodes.payload[r].label.is_loop = false;
	
	if (f->current_label) {
		f->nodes.payload[f->current_label].label.terminator = r;
	}
	
	f->current_label = r;
	return r;
}

TB_API void tb_inst_goto(TB_Function* f, TB_Label id) {
	assert(id >= 0 && id < f->label_count);
	if (f->current_label == TB_NULL_REG) {
		// Was placed after a terminator instruction,
		// just omit this to avoid any issues since it's
		// not a big deal for example:
		// RET x
		// ~~GOTO .L5~~
		// .L4:
		return;
	}
	
	TB_Register r = tb_make_reg(f, TB_GOTO, TB_TYPE_VOID);
	f->nodes.payload[r].goto_.label = id;
	
	assert(f->current_label);
	f->nodes.payload[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
}

TB_API TB_Register tb_inst_if(TB_Function* f, TB_Register cond, TB_Label if_true, TB_Label if_false) {
	TB_Register r = tb_make_reg(f, TB_IF, TB_TYPE_VOID);
	f->nodes.payload[r].if_.cond = cond;
	f->nodes.payload[r].if_.if_true = if_true;
	f->nodes.payload[r].if_.if_false = if_false;
	
	assert(f->current_label);
	f->nodes.payload[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
	return r;
}

TB_API void tb_inst_switch(TB_Function* f, TB_DataType dt, TB_Register key, TB_Label default_label, size_t entry_count, const TB_SwitchEntry* entries) {
	// the switch entries are 2 slots each
	size_t param_count = entry_count * 2;
	int param_start = f->vla.count;
	
	TB_Register* vla = tb_vla_reserve(f, param_count);
	memcpy(vla, entries, param_count * sizeof(TB_Register));
	f->vla.count += param_count;
	
	int param_end = f->vla.count;
	
	TB_Register r = tb_make_reg(f, TB_SWITCH, dt);
	f->nodes.payload[r].switch_.key = key;
	f->nodes.payload[r].switch_.default_label = default_label;
	f->nodes.payload[r].switch_.entries_start = param_start;
	f->nodes.payload[r].switch_.entries_end = param_end;
	
	assert(f->current_label);
	f->nodes.payload[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
}

TB_API void tb_inst_ret(TB_Function* f, TB_Register value) {
	TB_Register r = tb_make_reg(f, TB_RET, f->prototype->return_dt);
	f->nodes.payload[r].ret.value = value;
	
	assert(f->current_label);
	f->nodes.payload[f->current_label].label.terminator = r;
	f->current_label = TB_NULL_REG;
}

#if !TB_STRIP_LABELS
void tb_emit_label_symbol(TB_Module* m, uint32_t func_id, uint32_t label_id, size_t pos) {
	assert(pos < UINT32_MAX);
	TB_LabelSymbol sym = {
		.func_id = func_id,
		.label_id = label_id,
		.pos = pos
	};
	
	arrput(m->label_symbols, sym);
}
#endif
