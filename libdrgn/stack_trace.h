// Copyright 2019-2020 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

#ifndef DRGN_STACK_TRACE_H
#define DRGN_STACK_TRACE_H

#include <elfutils/libdwfl.h>

struct drgn_frame_symbol {
	const char *name;
	Dwarf_Die var_die;
};

DEFINE_VECTOR(drgn_frame_symbol_vector, struct drgn_frame_symbol);

struct drgn_stack_frame {
	Dwfl_Frame *state;
	Dwarf_Die *scopes;
	int num_scopes;
	int subprogram;
	struct drgn_frame_symbol_vector variables;
	struct drgn_frame_symbol_vector parameters;
	uint64_t bias;
	struct {
		bool variables : 1;
		bool parameters : 1;
	} valid;
};

struct drgn_stack_trace {
	struct drgn_program *prog;
	union {
		Dwfl_Thread *thread;
		/* Used during creation. */
		int capacity;
	};
	int num_frames;
	struct drgn_stack_frame frames[];
};

#endif /* DRGN_STACK_TRACE_H */
