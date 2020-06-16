// Copyright (c) Facebook, Inc. and its affiliates.
// SPDX-License-Identifier: GPL-3.0+

#include <byteswap.h>
#include <dwarf.h>
#include <elfutils/libdwfl.h>
#include <endian.h>
#include <inttypes.h>
#include <stdlib.h>

#include "internal.h"
#include "helpers.h"
#include "object.h"
#include "program.h"
#include "read.h"
#include "string_builder.h"
#include "vector.h"
#include "symbol.h"
#include "print.h"

struct drgn_stack_object {
	struct drgn_stack_frame frame;
	Dwarf_Die die;
};

struct drgn_frame_symbol {
	bool parameter;
	const char *name;
	Dwarf_Die var_die;
};

DEFINE_VECTOR(drgn_frame_symbol_vector, struct drgn_frame_symbol);

enum drgn_frame_state_flags {
	DRGN_FRAME_STATE_PARAMS_EVALUATED = 0x1,
	DRGN_FRAME_STATE_SYMBOLS_EVALUATED = 0x2,
	DRGN_FRAME_STATE_BIAS_VALID = 0x4,
};

struct drgn_frame_state {
	struct drgn_frame_symbol_vector variables;
	struct drgn_frame_symbol_vector parameters;
	uint64_t bias;
	unsigned flags;
};

struct drgn_stack_trace {
	struct drgn_program *prog;
	union {
		size_t capacity;
		Dwfl_Thread *thread;
	};
	struct drgn_frame_state *states;
	size_t num_frames;
	Dwfl_Frame *frames[];
};

static inline struct drgn_frame_state *
trace_frame_state(const struct drgn_stack_trace *trace, size_t frame)
{
	return &trace->states[frame];
}

static inline struct drgn_frame_state *
stack_frame_state(const struct drgn_stack_frame *frame)
{
	return trace_frame_state(frame->trace, frame->i);
}

struct drgn_frame_symbol *
drgn_frame_state_variables(const struct drgn_stack_frame *frame, size_t *count)
{
	struct drgn_frame_state *state = stack_frame_state(frame);

	*count = state->variables.size;
	return state->variables.data;
}

struct drgn_frame_symbol *
drgn_frame_state_parameters(const struct drgn_stack_frame *frame, size_t *count)
{
	struct drgn_frame_state *state = stack_frame_state(frame);

	*count = state->parameters.size;
	return state->parameters.data;
}

LIBDRGN_PUBLIC void drgn_stack_trace_destroy(struct drgn_stack_trace *trace)
{
	dwfl_detach_thread(trace->thread);
	free(trace);
}

LIBDRGN_PUBLIC
size_t drgn_stack_trace_num_frames(struct drgn_stack_trace *trace)
{
	return trace->num_frames;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_format_stack_trace(struct drgn_stack_trace *trace, char **ret)
{
	struct drgn_error *err;
	struct string_builder str = {};
	struct drgn_stack_frame frame = { .trace = trace, };

	for (; frame.i < trace->num_frames; frame.i++) {
		Dwarf_Addr pc;
		bool isactivation;
		Dwfl_Module *module;
		struct drgn_symbol sym;

		if (!string_builder_appendf(&str, "#%-2zu ", frame.i)) {
			err = &drgn_enomem;
			goto err;
		}

		dwfl_frame_pc(trace->frames[frame.i], &pc, &isactivation);
		module = dwfl_frame_module(trace->frames[frame.i]);
		if (module &&
		    drgn_program_find_symbol_by_address_internal(trace->prog,
								 pc - !isactivation,
								 module,
								 &sym)) {
			if (!string_builder_appendf(&str,
						    "%s+0x%" PRIx64 "/0x%" PRIx64,
						    sym.name, pc - sym.address,
						    sym.size)) {
				err = &drgn_enomem;
				goto err;
			}
		} else {
			if (!string_builder_appendf(&str, "0x%" PRIx64, pc)) {
				err = &drgn_enomem;
				goto err;
			}
		}

		if (frame.i != trace->num_frames - 1 &&
		    !string_builder_appendc(&str, '\n')) {
			err = &drgn_enomem;
			goto err;
		}
	}
	if (!string_builder_finalize(&str, ret)) {
		err = &drgn_enomem;
		goto err;
	}
	return NULL;

err:
	free(str.str);
	return err;
}

LIBDRGN_PUBLIC uint64_t drgn_stack_frame_pc(struct drgn_stack_frame frame)
{
	Dwarf_Addr pc;

	dwfl_frame_pc(frame.trace->frames[frame.i], &pc, NULL);
	return pc;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_symbol(struct drgn_stack_frame frame, struct drgn_symbol **ret)
{
	Dwarf_Addr pc;
	bool isactivation;
	Dwfl_Module *module;
	struct drgn_symbol *sym;

	dwfl_frame_pc(frame.trace->frames[frame.i], &pc, &isactivation);
	if (!isactivation)
		pc--;
	module = dwfl_frame_module(frame.trace->frames[frame.i]);
	if (!module)
		return drgn_error_symbol_not_found(pc);
	sym = malloc(sizeof(*sym));
	if (!sym)
		return &drgn_enomem;
	if (!drgn_program_find_symbol_by_address_internal(frame.trace->prog, pc,
							  module, sym)) {
		free(sym);
		return drgn_error_symbol_not_found(pc);
	}
	*ret = sym;
	return NULL;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_register(struct drgn_stack_frame frame,
			  enum drgn_register_number regno, uint64_t *ret)
{
	Dwarf_Addr value;

	if (!dwfl_frame_register(frame.trace->frames[frame.i], regno, &value)) {
		return drgn_error_create(DRGN_ERROR_LOOKUP,
					 "register value is not known");
	}
	*ret = value;
	return NULL;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_register_by_name(struct drgn_stack_frame frame,
				  const char *name, uint64_t *ret)
{
	const struct drgn_register *reg;

	reg = drgn_architecture_register_by_name(frame.trace->prog->platform.arch,
						 name);
	if (!reg) {
		return drgn_error_format(DRGN_ERROR_LOOKUP,
					 "unknown register '%s'", name);
	}
	return drgn_stack_frame_register(frame, reg->number, ret);
}

static struct drgn_error *
evaluate_dwarf_location(Dwarf_Attribute *location, uint64_t pc,
			Dwarf_Op **ops, size_t *len)
{
	ptrdiff_t offset = 0;
	uint64_t base, begin, end;
	bool found = false;

	while ((offset = dwarf_getlocations(location, offset, &base,
					    &begin, &end,
					    ops, len)) > 0) {
		if (pc >= begin && pc < end)
			return NULL;
	}

	if (offset < 0)
		return drgn_error_libdw();

	return drgn_error_format(DRGN_ERROR_VAR_LOCATION_UNAVAILABLE,
				 "debuginfo not available for this location");
}


static bool dwarf_expr_result_is_memory(const Dwarf_Op *ops, size_t len)
{
	bool is_memory = true;
	bool is_stack_memory = false;
	int i;

	for (i = 0; i < len; i++) {
		switch (ops[i].atom) {
		case DW_OP_reg0 ... DW_OP_reg31:
		case DW_OP_regx:
		case DW_OP_implicit_value:
			is_memory = false;
			break;
		case DW_OP_stack_value:
			is_memory = false;
			is_stack_memory = true;
			break;
		case DW_OP_implicit_pointer:
		case DW_OP_GNU_implicit_pointer:
			is_memory = is_stack_memory = false;
			break;
		case DW_OP_fbreg:
			is_memory = is_stack_memory = true;
			break;
		};
	}

	return is_stack_memory;
}

static struct drgn_error *
stack_frame_symbol_location(struct drgn_stack_frame frame, Dwarf_Die *var_die,
			    Dwarf_Addr *address, bool *is_reference)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	Dwfl_Frame *dframe = frame.trace->frames[frame.i];
	uint64_t pc = drgn_stack_frame_pc(frame);
	Dwarf_Attribute attr_mem, *location;
	Dwarf_Addr base, begin, end;
	Dwarf_Op *ops;
	size_t len;
	int i;
	ptrdiff_t offset = 0;
	bool found = false;
	struct drgn_error *err;

	if (!(state->flags & DRGN_FRAME_STATE_BIAS_VALID)) {
		Dwfl_Module *module = dwfl_frame_module(dframe);
		Dwarf *dwarf;

		if (!module || !dwfl_module_getdwarf(module, &state->bias))
			return drgn_error_format(DRGN_ERROR_OTHER,
					"couldn't determine bias from frame");

		state->flags |= DRGN_FRAME_STATE_BIAS_VALID;
	}

	location = dwarf_attr_integrate(var_die, DW_AT_location, &attr_mem);
	if (!location)
		return drgn_error_format(DRGN_ERROR_VAR_OPTIMIZED_OUT,
					 "variable was optimized out");

	err = evaluate_dwarf_location(location, pc - state->bias, &ops, &len);
	if (err)
		return err;

	if (!dwfl_frame_eval_expr(dframe, ops, len, address)) {
		/* It'd be nice if libdwfl exported error codes */
		int dwflerr = dwfl_errno();
		const char *msg = dwfl_errmsg(dwflerr);

#ifndef DEBUG
		/*
		 * This can happen near the outermost frames where
		 * the registers weren't populated or saved.
		 *
		 * This hack will work until elfutils gets proper i18n.
		 */
		if (!strcmp(msg, "Invalid register"))
			return drgn_error_format(
				DRGN_ERROR_VAR_VALUE_UNAVAILABLE,
				"value is unavailable at this location");
#endif
		/* If you suspect an unhandled case, trigger this. */
		return drgn_error_format(DRGN_ERROR_OTHER, "libdwfl error: %s",
					 dwfl_errmsg(dwflerr));
	}

	/* This is naive but dwfl_frame_eval_expr doesn't give us much */
	*is_reference = dwarf_expr_result_is_memory(ops, len);

	return NULL;
}

struct drgn_error *
drgn_stack_frame_object_evaluate(struct drgn_object *obj)
{
	Dwarf_Addr address;
	struct drgn_object_stack_info *stack;
	struct drgn_error *err;
	struct drgn_stack_object *stack_obj = obj->stack;
	enum drgn_object_kind kind = DRGN_OBJECT_NONE;
	struct drgn_qualified_type qualified_type = {
		.type = obj->type,
		.qualifiers = obj->qualifiers,
	};
	bool is_reference;

	if (!obj->needs_stack_evaluation)
		return NULL;

	err = stack_frame_symbol_location(stack_obj->frame,
					  &stack_obj->die, &address,
					  &is_reference);
	if (err)
		return err;

	if (is_reference)
		return drgn_object_set_reference(obj, qualified_type,
					 address, 0, 0,
					 dwarf_die_byte_order(&stack_obj->die));

	switch (obj->kind) {
	case DRGN_OBJECT_SIGNED:
		err = drgn_object_set_signed(obj, qualified_type,
					     address, 0);
		break;
	case DRGN_OBJECT_UNSIGNED:
		err = drgn_object_set_unsigned(obj, qualified_type,
					       address, 0);
		break;
	case DRGN_OBJECT_FLOAT:
		err = drgn_object_set_float(obj, qualified_type,
					    (double)address);
		break;
	}
	return err;
}

struct drgn_error *
drgn_type_from_dwarf_child(struct drgn_dwarf_info_cache *dicache,
			   Dwarf_Die *parent_die,
			   const struct drgn_language *parent_lang,
			   const char *tag_name,
			   bool can_be_void, bool can_be_incomplete_array,
			   bool *is_incomplete_array_ret,
			   struct drgn_qualified_type *ret);

static struct drgn_error *
drgn_stack_frame_object(struct drgn_stack_frame frame,
			struct drgn_frame_symbol *symbol,
			struct drgn_object *ret_obj)
{
	struct drgn_program *prog = frame.trace->prog;
	struct drgn_qualified_type qualified_type;
	struct drgn_error *err;
	Dwarf_Addr address;
	struct drgn_object_type type;
	enum drgn_object_kind kind;
	const char *tag = "DW_TAG_variable";
	struct drgn_stack_object *stack_obj;
	uint64_t bit_size;
	const char *tname;

	if (symbol->parameter)
		tag = "DW_TAG_formal_parameter";

	err = drgn_type_from_dwarf_child(prog->_dicache, &symbol->var_die,
					 prog->lang, tag, false, false,
					 NULL, &qualified_type);
	if (err)
		return err;

	stack_obj = malloc(sizeof(*stack_obj));
	if (!stack_obj)
		return &drgn_enomem;

	stack_obj->frame = frame;
	stack_obj->die = symbol->var_die;

	err = drgn_object_set_common(qualified_type, 0, &type,
				     &kind, &bit_size);
	if (err) {
		free(stack_obj);
		return err;
	}

	drgn_object_reinit(ret_obj, &type, kind, bit_size, false);
	ret_obj->needs_stack_evaluation = true;
	ret_obj->stack = stack_obj;
	return NULL;
}

static struct drgn_error *
parse_scope_symbols(Dwarf_Die *die, unsigned tag,
		    struct drgn_frame_symbol_vector *vector)
{
	struct drgn_error *err = NULL;
	Dwarf_Die var_die;

	if (dwarf_child(die, &var_die) != 0)
		return NULL;

	do {
		struct drgn_frame_symbol *var;

		if (dwarf_tag(&var_die) != tag)
			continue;

		var = drgn_frame_symbol_vector_append_entry(vector);
		if (!var) {
			err = &drgn_enomem;
			break;
		}

		var->var_die = var_die;

		var->name = dwarf_diename(&var_die);
		if (!var->name) {
			err = drgn_error_libdw();
			break;
		}
	} while (dwarf_siblingof(&var_die, &var_die) == 0);

	return err;
}

static struct drgn_error *
drgn_stack_frame_parse_parameters(struct drgn_stack_frame frame)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_stack_trace *trace = frame.trace;
	struct drgn_program *prog = frame.trace->prog;
	struct drgn_error *err;
	uint64_t pc = drgn_stack_frame_pc(frame);
	Dwarf_Die func_die, result, var_die, *scopes = NULL;
	Dwarf_Attribute attr_mem, *location;
	Dwarf_Attribute base_mem, *base;
	int nscopes, i;
	uint64_t bias;

	if (state->flags & DRGN_FRAME_STATE_PARAMS_EVALUATED)
		return NULL;

	err = drgn_program_block_find(prog, pc, &func_die, &bias);
	if (err)
		return err;

	drgn_frame_symbol_vector_init(&state->parameters);
	err = parse_scope_symbols(&func_die, DW_TAG_formal_parameter,
				  &state->parameters);
	if (err) {
		drgn_frame_symbol_vector_deinit(&state->parameters);
		return err;
	}

	drgn_frame_symbol_vector_shrink_to_fit(&state->parameters);
	state->flags |= DRGN_FRAME_STATE_PARAMS_EVALUATED;
	return NULL;
}

static struct drgn_error *
find_frame_symbol_in_vector(struct drgn_frame_symbol_vector *vector,
			    const char *name, struct drgn_frame_symbol **ret)
{
	int i;

	for (i = 0; i < vector->size; i++) {
		if (!strcmp(name, vector->data[i].name)) {
			*ret = &vector->data[i];
			return NULL;
		}
	}

	return drgn_error_format(DRGN_ERROR_LOOKUP,
				 "no symbol named `%s' found in stack frame",
				 name);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_num_parameters(struct drgn_stack_frame frame, size_t *count)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_error *err;

	err = drgn_stack_frame_parse_parameters(frame);
	if (err)
		return err;

	*count = state->parameters.size;
	return NULL;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_parameter_by_name(struct drgn_stack_frame frame,
				   const char *name,
				   struct drgn_object *ret_obj)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_frame_symbol *sym;
	struct drgn_error *err;

	err = drgn_stack_frame_parse_parameters(frame);
	if (err)
		return err;

	err = find_frame_symbol_in_vector(&state->parameters, name, &sym);
	if (err)
		return err;

	return drgn_stack_frame_object(frame, sym, ret_obj);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_parameter_by_index(struct drgn_stack_frame frame,
				    size_t index, const char **name,
				    struct drgn_object *ret_obj)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_frame_symbol *symbols;
	struct drgn_error *err;
	size_t max;

	err = drgn_stack_frame_parse_parameters(frame);
	if (err)
		return err;

	symbols = drgn_frame_state_parameters(&frame, &max);

	if (index > max)
		return drgn_error_format(DRGN_ERROR_OUT_OF_BOUNDS,
					 "index %lu is out of range", index);

	err = drgn_stack_frame_object(frame, &symbols[index], ret_obj);
	if (err)
		return err;

	*name = symbols[index].name;
	return NULL;
}

static struct drgn_error *
drgn_stack_frame_parse_variables(struct drgn_stack_frame frame)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_program *prog = frame.trace->prog;
	struct drgn_error *err;
	uint64_t pc = drgn_stack_frame_pc(frame);
	struct drgn_dwarf_index *dindex;
	Dwarf_Die func_die, result, var_die, *cudie, *scopes = NULL;
	Dwarf_Attribute attr_mem, *location;
	Dwarf_Attribute base_mem, *base;
	Dwfl_Module *module;
	int nscopes, i;
	uint64_t bias;

	if (state->flags & DRGN_FRAME_STATE_SYMBOLS_EVALUATED)
		return NULL;

	err = drgn_program_block_find(prog, pc, &func_die, &bias);
	if (err)
		return err;

	module = dwfl_frame_module(frame.trace->frames[frame.i]);
	cudie = dwfl_module_addrdie (module, pc, &bias);

	nscopes = dwarf_getscopes (cudie, pc - bias, &scopes);
	if (nscopes < 0)
		return drgn_error_libdw();

	drgn_frame_symbol_vector_init(&state->variables);
	for (i = 0; i < nscopes - 1; i++) {
		err = parse_scope_symbols(&scopes[i], DW_TAG_variable,
					  &state->variables);
		if (err) {
			free(scopes);
			drgn_frame_symbol_vector_deinit(&state->variables);
			return err;
		}
	}
	drgn_frame_symbol_vector_shrink_to_fit(&state->variables);

	state->flags |= DRGN_FRAME_STATE_SYMBOLS_EVALUATED;
	return NULL;

out_error:
	free(scopes);
	drgn_frame_symbol_vector_deinit(&state->variables);
	return err;
}

static struct drgn_error *
drgn_stack_frame_get_symbol(struct drgn_stack_frame frame, const char *name,
			    struct drgn_frame_symbol **ret)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);

	return find_frame_symbol_in_vector(&state->variables, name, ret);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_num_variables(struct drgn_stack_frame frame, size_t *count)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_error *err;

	err = drgn_stack_frame_parse_variables(frame);
	if (err)
		return err;

	*count = state->variables.size;
	return NULL;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_variable_by_name(struct drgn_stack_frame frame,
				   const char *name,
				   struct drgn_object *ret_obj)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_frame_symbol *sym;
	struct drgn_error *err;

	err = drgn_stack_frame_parse_variables(frame);
	if (err)
		return err;

	err = find_frame_symbol_in_vector(&state->variables, name, &sym);
	if (err)
		return err;

	return drgn_stack_frame_object(frame, sym, ret_obj);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_stack_frame_variable_by_index(struct drgn_stack_frame frame,
				    size_t index, const char **name,
				    struct drgn_object *ret_obj)
{
	struct drgn_frame_state *state = stack_frame_state(&frame);
	struct drgn_frame_symbol *symbols;
	struct drgn_error *err;
	size_t max;

	err = drgn_stack_frame_parse_variables(frame);
	if (err)
		return err;

	symbols = drgn_frame_state_variables(&frame, &max);

	if (index > max)
		return drgn_error_format(DRGN_ERROR_OUT_OF_BOUNDS,
					 "index %lu is out of range", index);

	err = drgn_stack_frame_object(frame, &symbols[index], ret_obj);
	if (err)
		return err;

	*name = symbols[index].name;
	return NULL;
}

static bool drgn_thread_memory_read(Dwfl *dwfl, Dwarf_Addr addr,
				    Dwarf_Word *result, void *dwfl_arg)
{
	struct drgn_error *err;
	struct drgn_program *prog = dwfl_arg;
	uint64_t word;

	err = drgn_program_read_word(prog, addr, false, &word);
	if (err) {
		if (err->code == DRGN_ERROR_FAULT) {
			/*
			 * This could be the end of the stack trace, so it shouldn't be
			 * fatal.
			 */
			drgn_error_destroy(err);
		} else {
			drgn_error_destroy(prog->stack_trace_err);
			prog->stack_trace_err = err;
		}
		return false;
	}
	*result = word;
	return true;
}

/*
 * We only care about the specific thread that we're unwinding, so we return it
 * with an arbitrary TID.
 */
#define STACK_TRACE_OBJ_TID 1
static pid_t drgn_object_stack_trace_next_thread(Dwfl *dwfl, void *dwfl_arg,
						 void **thread_argp)
{
	struct drgn_program *prog = dwfl_arg;

	if (*thread_argp)
		return 0;
	*thread_argp = prog;
	return STACK_TRACE_OBJ_TID;
}

static struct drgn_error *
drgn_get_stack_trace_obj(struct drgn_object *res, struct drgn_program *prog,
			 bool *is_pt_regs_ret)
{
	struct drgn_error *err;
	struct drgn_type *type;

	type = drgn_underlying_type(prog->stack_trace_obj->type);
	if (drgn_type_kind(type) == DRGN_TYPE_STRUCT &&
	    strcmp(drgn_type_tag(type), "pt_regs") == 0) {
		*is_pt_regs_ret = true;
		return drgn_object_read(res, prog->stack_trace_obj);
	}

	if (drgn_type_kind(type) != DRGN_TYPE_POINTER)
		goto type_error;
	type = drgn_underlying_type(drgn_type_type(type).type);
	if (drgn_type_kind(type) != DRGN_TYPE_STRUCT)
		goto type_error;

	if ((prog->flags & DRGN_PROGRAM_IS_LINUX_KERNEL) &&
	    strcmp(drgn_type_tag(type), "task_struct") == 0) {
		*is_pt_regs_ret = false;
		return drgn_object_read(res, prog->stack_trace_obj);
	} else if (strcmp(drgn_type_tag(type), "pt_regs") == 0) {
		*is_pt_regs_ret = true;
		/*
		 * If the drgn_object_read() call fails, we're breaking
		 * the rule of not modifying the result on error, but we
		 * don't care in this context.
		 */
		err = drgn_object_dereference(res, prog->stack_trace_obj);
		if (err)
			return err;
		return drgn_object_read(res, res);
	}

type_error:
	return drgn_error_format(DRGN_ERROR_TYPE,
				 "expected struct pt_regs, struct pt_regs *%s, or int",
				 (prog->flags & DRGN_PROGRAM_IS_LINUX_KERNEL) ?
				 ", struct task_struct *" : "");
}

static bool drgn_thread_set_initial_registers(Dwfl_Thread *thread,
					      void *thread_arg)
{
	struct drgn_error *err;
	struct drgn_program *prog = thread_arg;
	struct drgn_object obj;
	struct drgn_object tmp;
	struct string prstatus;

	drgn_object_init(&obj, prog);
	drgn_object_init(&tmp, prog);

	/* First, try pt_regs. */
	if (prog->stack_trace_obj) {
		bool is_pt_regs;

		err = drgn_get_stack_trace_obj(&obj, prog, &is_pt_regs);
		if (err)
			goto out;

		if (is_pt_regs) {
			assert(obj.kind == DRGN_OBJECT_BUFFER &&
			       !obj.is_reference);
			if (!prog->platform.arch->pt_regs_set_initial_registers) {
				err = drgn_error_format(DRGN_ERROR_INVALID_ARGUMENT,
							"pt_regs stack unwinding is not supported for %s architecture",
							prog->platform.arch->name);
				goto out;
			}
			err = prog->platform.arch->pt_regs_set_initial_registers(thread,
										 &obj);
			goto out;
		}
	} else if (prog->flags & DRGN_PROGRAM_IS_LINUX_KERNEL) {
		bool found;

		err = drgn_program_find_object(prog, "init_pid_ns", NULL,
					       DRGN_FIND_OBJECT_ANY, &tmp);
		if (err)
			goto out;
		err = drgn_object_address_of(&tmp, &tmp);
		if (err)
			goto out;
		err = linux_helper_find_task(&obj, &tmp, prog->stack_trace_tid);
		if (err)
			goto out;
		err = drgn_object_bool(&obj, &found);
		if (err)
			goto out;
		if (!found) {
			err = drgn_error_create(DRGN_ERROR_LOOKUP, "task not found");
			goto out;
		}
	}

	if (prog->flags & DRGN_PROGRAM_IS_LINUX_KERNEL) {
		if (prog->flags & DRGN_PROGRAM_IS_LIVE) {
			err = drgn_object_member_dereference(&tmp, &obj, "on_cpu");
			if (!err) {
				bool on_cpu;
				err = drgn_object_bool(&tmp, &on_cpu);
				if (err)
					goto out;
				if (on_cpu) {
					err = drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
								"cannot unwind stack of running task");
					goto out;
				}
			} else if (err->code == DRGN_ERROR_LOOKUP) {
				/*
				 * The running kernel is !SMP. Assume that the
				 * task isn't running (which can only be wrong
				 * for this thread itself).
				 */
				drgn_error_destroy(err);
			} else {
				goto out;
			}
			prstatus.str = NULL;
			prstatus.len = 0;
		} else {
			union drgn_value value;
			uint32_t cpu;

			err = drgn_object_member_dereference(&tmp, &obj, "cpu");
			if (!err) {
				err = drgn_object_read_integer(&tmp, &value);
				if (err)
					goto out;
				cpu = value.uvalue;
			} else if (err->code == DRGN_ERROR_LOOKUP) {
				/* !SMP. Must be CPU 0. */
				drgn_error_destroy(err);
				cpu = 0;
			} else {
				goto out;
			}
			err = drgn_program_find_prstatus_by_cpu(prog, cpu,
								&prstatus);
			if (err)
				goto out;
		}
		if (!prog->platform.arch->linux_kernel_set_initial_registers) {
			err = drgn_error_format(DRGN_ERROR_INVALID_ARGUMENT,
						"Linux kernel stack unwinding is not supported for %s architecture",
						prog->platform.arch->name);
			goto out;
		}
		err = prog->platform.arch->linux_kernel_set_initial_registers(thread,
									      &obj,
									      prstatus.str,
									      prstatus.len);
	} else {
		err = drgn_program_find_prstatus_by_tid(prog,
							prog->stack_trace_tid,
							&prstatus);
		if (err)
			goto out;
		if (!prstatus.str) {
			err = drgn_error_create(DRGN_ERROR_LOOKUP, "thread not found");
			goto out;
		}
		if (!prog->platform.arch->prstatus_set_initial_registers) {
			err = drgn_error_format(DRGN_ERROR_INVALID_ARGUMENT,
						"core dump stack unwinding is not supported for %s architecture",
						prog->platform.arch->name);
			goto out;
		}
		err = prog->platform.arch->prstatus_set_initial_registers(prog,
									  thread,
									  prstatus.str,
									  prstatus.len);
	}

out:
	drgn_object_deinit(&tmp);
	drgn_object_deinit(&obj);
	if (err) {
		drgn_error_destroy(prog->stack_trace_err);
		prog->stack_trace_err = err;
		return false;
	}
	return true;
}

static int drgn_append_stack_frame(Dwfl_Frame *state, void *arg)
{
	struct drgn_stack_trace **tracep = arg;
	struct drgn_stack_trace *trace = *tracep;

	if (trace->num_frames >= trace->capacity) {
		struct drgn_stack_trace *tmp;
		size_t new_capacity, bytes;

		if (__builtin_mul_overflow(2U, trace->capacity,
					   &new_capacity) ||
		    __builtin_mul_overflow(new_capacity,
					   sizeof(trace->frames[0]), &bytes) ||
		    __builtin_add_overflow(bytes, sizeof(*trace), &bytes) ||
		    !(tmp = realloc(trace, bytes))) {
			drgn_error_destroy(trace->prog->stack_trace_err);
			trace->prog->stack_trace_err = &drgn_enomem;
			return DWARF_CB_ABORT;
		}
		*tracep = trace = tmp;
		trace->capacity = new_capacity;
	}
	trace->frames[trace->num_frames++] = state;
	return DWARF_CB_OK;
}

static const Dwfl_Thread_Callbacks drgn_linux_kernel_thread_callbacks = {
	.next_thread = drgn_object_stack_trace_next_thread,
	.memory_read = drgn_thread_memory_read,
	.set_initial_registers = drgn_thread_set_initial_registers,
};

static struct drgn_error *drgn_get_stack_trace(struct drgn_program *prog,
					       uint32_t tid,
					       const struct drgn_object *obj,
					       struct drgn_stack_trace **ret)
{
	struct drgn_error *err;
	Dwfl *dwfl;
	Dwfl_Thread *thread;
	struct drgn_stack_trace *trace;

	if (!prog->has_platform) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "cannot unwind stack without platform");
	}
	if ((prog->flags & (DRGN_PROGRAM_IS_LINUX_KERNEL |
			    DRGN_PROGRAM_IS_LIVE)) == DRGN_PROGRAM_IS_LIVE) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "stack unwinding is not yet supported for live processes");
	}

	err = drgn_program_get_dwfl(prog, &dwfl);
	if (err)
		return err;
	if (!prog->attached_dwfl_state) {
		if (!dwfl_attach_state(dwfl, NULL, 0,
				       &drgn_linux_kernel_thread_callbacks,
				       prog))
			return drgn_error_libdwfl();
		prog->attached_dwfl_state = true;
	}

	prog->stack_trace_tid = tid;
	prog->stack_trace_obj = obj;
	thread = dwfl_attach_thread(dwfl, STACK_TRACE_OBJ_TID);
	prog->stack_trace_obj = NULL;
	prog->stack_trace_tid = 0;
	if (prog->stack_trace_err)
		goto stack_trace_err;
	if (!thread) {
		err = drgn_error_libdwfl();
		goto err;
	}

	trace = malloc(sizeof(*trace) + sizeof(trace->frames[0]));
	if (!trace) {
		err = &drgn_enomem;
		goto err;
	}
	trace->prog = prog;
	trace->capacity = 1;
	trace->num_frames = 0;

	dwfl_thread_getframes(thread, drgn_append_stack_frame, &trace);
	if (prog->stack_trace_err) {
		free(trace);
		goto stack_trace_err;
	}

	/* Shrink the trace to fit if we can, but don't fail if we can't. */
	if (trace->capacity > trace->num_frames) {
		struct drgn_stack_trace *tmp;

		tmp = realloc(trace,
			      sizeof(*trace) +
			      trace->num_frames * sizeof(trace->frames[0]));
		if (tmp)
			trace = tmp;
	}

	trace->states = calloc(trace->num_frames, sizeof(*trace->states));
	if (!trace->states) {
		free(trace);
		goto stack_trace_err;
	}

	trace->thread = thread;
	*ret = trace;
	return NULL;

stack_trace_err:
	/*
	 * The error reporting for dwfl_getthread_frames() is not great. The
	 * documentation says that some of its unwinder implementations always
	 * return an error. So, we do our own error reporting for fatal errors
	 * through prog->stack_trace_err.
	 */
	err = prog->stack_trace_err;
	prog->stack_trace_err = NULL;
err:
	dwfl_detach_thread(thread);
	return err;
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_stack_trace(struct drgn_program *prog, uint32_t tid,
			 struct drgn_stack_trace **ret)
{
	return drgn_get_stack_trace(prog, tid, NULL, ret);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_object_stack_trace(const struct drgn_object *obj,
			struct drgn_stack_trace **ret)
{
	struct drgn_error *err;

	if (drgn_type_kind(drgn_underlying_type(obj->type)) == DRGN_TYPE_INT) {
		union drgn_value value;

		err = drgn_object_read_integer(obj, &value);
		if (err)
			return err;
		return drgn_get_stack_trace(obj->prog, value.uvalue, NULL, ret);
	} else {
		return drgn_get_stack_trace(obj->prog, 0, obj, ret);
	}
}
