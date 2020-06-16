// Copyright (c) Facebook, Inc. and its affiliates.
// SPDX-License-Identifier: GPL-3.0+

#include "drgnpy.h"

static void StackTrace_dealloc(StackTrace *self)
{
	drgn_stack_trace_destroy(self->trace);
	Py_XDECREF(self->prog);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *StackTrace_str(StackTrace *self)
{
	struct drgn_error *err;
	PyObject *ret;
	char *str;

	err = drgn_format_stack_trace(self->trace, &str);
	if (err)
		return set_drgn_error(err);

	ret = PyUnicode_FromString(str);
	free(str);
	return ret;
}

static Py_ssize_t StackTrace_length(StackTrace *self)
{
	return drgn_stack_trace_num_frames(self->trace);
}

static StackFrame *StackTrace_item(StackTrace *self, Py_ssize_t i)
{
	StackFrame *ret;

	if (i < 0 || i >= drgn_stack_trace_num_frames(self->trace)) {
		PyErr_SetString(PyExc_IndexError,
				"stack frame index out of range");
		return NULL;
	}
	ret = (StackFrame *)StackFrame_type.tp_alloc(&StackFrame_type, 0);
	if (!ret)
		return NULL;
	ret->frame.trace = self->trace;
	ret->frame.i = i;
	ret->trace = self;
	Py_INCREF(self);
	return ret;
}

static PySequenceMethods StackTrace_as_sequence = {
	.sq_length = (lenfunc)StackTrace_length,
	.sq_item = (ssizeargfunc)StackTrace_item,
};

PyTypeObject StackTrace_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "_drgn.StackTrace",
	.tp_basicsize = sizeof(StackTrace),
	.tp_dealloc = (destructor)StackTrace_dealloc,
	.tp_as_sequence = &StackTrace_as_sequence,
	.tp_str = (reprfunc)StackTrace_str,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = drgn_StackTrace_DOC,
};

static void StackFrame_dealloc(StackFrame *self)
{
	Py_XDECREF(self->trace);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *StackFrame_symbol(StackFrame *self)
{
	struct drgn_error *err;
	struct drgn_symbol *sym;
	PyObject *ret;

	err = drgn_stack_frame_symbol(self->frame, &sym);
	if (err)
		return set_drgn_error(err);
	ret = Symbol_wrap(sym, self->trace->prog);
	if (!ret) {
		drgn_symbol_destroy(sym);
		return NULL;
	}
	return ret;
}

static PyObject *StackFrame_register(StackFrame *self, PyObject *arg)
{
	struct drgn_error *err;
	uint64_t value;

	if (PyUnicode_Check(arg)) {
		err = drgn_stack_frame_register_by_name(self->frame,
							PyUnicode_AsUTF8(arg),
							&value);
	} else {
		struct index_arg number = {};

		if (PyObject_TypeCheck(arg, &Register_type))
			arg = PyStructSequence_GET_ITEM(arg, 1);
		if (!index_converter(arg, &number))
			return NULL;
		err = drgn_stack_frame_register(self->frame, number.uvalue,
						&value);
	}
	if (err)
		return set_drgn_error(err);
	return PyLong_FromUnsignedLongLong(value);
}

static PyObject *StackFrame_registers(StackFrame *self)
{
	struct drgn_error *err;
	PyObject *dict;
	const struct drgn_platform *platform;
	size_t num_registers, i;

	dict = PyDict_New();
	if (!dict)
		return NULL;
	platform = drgn_program_platform(&self->trace->prog->prog);
	num_registers = drgn_platform_num_registers(platform);
	for (i = 0; i < num_registers; i++) {
		const struct drgn_register *reg;
		uint64_t value;
		PyObject *value_obj;
		int ret;

		reg = drgn_platform_register(platform, i);
		err = drgn_stack_frame_register(self->frame,
						drgn_register_number(reg),
						&value);
		if (err) {
			drgn_error_destroy(err);
			continue;
		}
		value_obj = PyLong_FromUnsignedLongLong(value);
		if (!value_obj) {
			Py_DECREF(dict);
			return NULL;
		}
		ret = PyDict_SetItemString(dict, drgn_register_name(reg),
					   value_obj);
		Py_DECREF(value_obj);
		if (ret == -1) {
			Py_DECREF(dict);
			return NULL;
		}
	}
	return dict;
}

static PyObject *StackFrame_parameters(StackFrame *self, void *arg)
{
	DrgnObject *ret;
	struct drgn_error *err;
	PyObject *parameters_obj;
	size_t num_parameters, i;

	parameters_obj = PyODict_New();
	if (!parameters_obj)
		return NULL;

	err = drgn_stack_frame_num_parameters(self->frame, &num_parameters);
	if (err) {
		set_drgn_error(err);
		goto err;
	}

	for (i = 0; i < num_parameters; i++) {
		const char *pname;
		PyObject *name;

		ret = DrgnObject_alloc(self->trace->prog);
		if (!ret)
			goto err;

		err = drgn_stack_frame_parameter_by_index(self->frame, i,
							  &pname, &ret->obj);
		if (err) {
			set_drgn_error(err);
			Py_DECREF(ret);
			goto err;
		}

		name = PyUnicode_FromString(pname);
		if (!name) {
			Py_DECREF(ret);
			goto err;
		}

		PyODict_SetItem(parameters_obj, name, (PyObject *)ret);
	}

	return parameters_obj;
err:
	Py_DECREF(parameters_obj);
	return NULL;
}

static PyObject *StackFrame_variables(StackFrame *self, void *arg)
{
	struct drgn_error *err;
	size_t num_variables;
	PyObject *dict;
	int i;

	dict = PyDict_New();
	if (!dict)
		return NULL;

	err = drgn_stack_frame_num_variables(self->frame, &num_variables);
	if (err) {
		if (err->code == DRGN_ERROR_LOOKUP)
			return dict;
		Py_DECREF(dict);
		set_drgn_error(err);
		return NULL;
	}

	/*
	 * Iterate the variables in reverse order so that shadowed variables
	 * in outer scopes are properly obscured.
	 */
	for (i = num_variables - 1; i >= 0; i--) {
		const char *name;
		DrgnObject *ret;

		ret = DrgnObject_alloc(self->trace->prog);
		if (!ret)
			goto error;

		err = drgn_stack_frame_variable_by_index(self->frame, i, &name,
							 &ret->obj);
		if (err) {
			Py_DECREF((PyObject *)ret);
			set_drgn_error(err);
			goto error;
		}

		if (PyDict_SetItemString(dict, name, (PyObject *)ret)) {
			Py_DECREF(Py_None);
			goto error;
		}
	}

	return dict;

error:
	Py_DECREF(dict);
	return NULL;
}

static DrgnObject *StackFrame_subscript(StackFrame *self, PyObject *key)
{
	struct drgn_error *err;
	DrgnObject *ret;
	const char *name;

	if (!PyUnicode_Check(key)) {
		PyErr_SetObject(PyExc_KeyError, key);
		return NULL;
	}

	name = PyUnicode_AsUTF8(key);
	if (!name)
		return NULL;


	ret = DrgnObject_alloc(self->trace->prog);
	if (!ret)
		return NULL;

	err = drgn_stack_frame_variable_by_name(self->frame, name, &ret->obj);
	if (err && err->code != DRGN_ERROR_LOOKUP) {
		set_drgn_error(err);
		return NULL;
	}

	err = drgn_stack_frame_parameter_by_name(self->frame, name, &ret->obj);
	if (err) {
		set_drgn_error(err);
		return NULL;
	}

	return ret;
}

static PyObject *StackFrame_get_pc(StackFrame *self, void *arg)
{
	return PyLong_FromUnsignedLongLong(drgn_stack_frame_pc(self->frame));
}

static PyMethodDef StackFrame_methods[] = {
	{"symbol", (PyCFunction)StackFrame_symbol, METH_NOARGS,
	 drgn_StackFrame_symbol_DOC},
	{"register", (PyCFunction)StackFrame_register,
	 METH_O, drgn_StackFrame_register_DOC},
	{"registers", (PyCFunction)StackFrame_registers,
	 METH_NOARGS, drgn_StackFrame_registers_DOC},
	{},
};

static PyGetSetDef StackFrame_getset[] = {
	{"pc", (getter)StackFrame_get_pc, NULL, drgn_StackFrame_pc_DOC},
	{"variables", (getter)StackFrame_variables, NULL,
	 drgn_StackFrame_variables_DOC },
	{"parameters", (getter)StackFrame_parameters, NULL,
	 drgn_StackFrame_parameters_DOC },
	{},
};

static PyMappingMethods StackFrame_as_mapping = {
	.mp_subscript = (binaryfunc)StackFrame_subscript,
};

PyTypeObject StackFrame_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "_drgn.StackFrame",
	.tp_basicsize = sizeof(StackFrame),
	.tp_dealloc = (destructor)StackFrame_dealloc,
	.tp_as_mapping = &StackFrame_as_mapping,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = drgn_StackFrame_DOC,
	.tp_methods = StackFrame_methods,
	.tp_getset = StackFrame_getset,
};
