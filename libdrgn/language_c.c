// Copyright 2018-2019 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "internal.h"
#include "hash_table.h"
#include "language.h"
#include "lexer.h"
#include "memory_reader.h"
#include "object.h"
#include "program.h"
#include "string_builder.h"
#include "type.h"
#include "type_index.h"

static struct drgn_error *
c_declare_variable(struct drgn_qualified_type qualified_type,
		   struct string_callback *name, size_t indent,
		   struct string_builder *sb);

static struct drgn_error *
c_define_type(struct drgn_qualified_type qualified_type, size_t indent,
	      struct string_builder *sb);

static struct drgn_error *append_tabs(int n, struct string_builder *sb)
{
	while (n-- > 0) {
		struct drgn_error *err;

		err = string_builder_appendc(sb, '\t');
		if (err)
			return err;
	}
	return NULL;
}

static struct drgn_error *c_variable_name(struct string_callback *name,
					  void *arg, struct string_builder *sb)
{
	return string_builder_append(sb, arg);
}

static struct drgn_error *c_append_qualifiers(enum drgn_qualifiers qualifiers,
					      struct string_builder *sb)
{
	static const char *qualifier_names[] = {
		"const", "volatile", "restrict", "_Atomic",
	};
	struct drgn_error *err;
	bool first = true;
	unsigned int i;

	static_assert((1 << ARRAY_SIZE(qualifier_names)) - 1 ==
		      DRGN_ALL_QUALIFIERS, "missing C qualifier name");

	for (i = 0; (1U << i) & DRGN_ALL_QUALIFIERS; i++) {
		if (!(qualifiers & (1U << i)))
			continue;
		if (!first) {
			err = string_builder_appendc(sb, ' ');
			if (err)
				return err;
		}
		err = string_builder_append(sb, qualifier_names[i]);
		if (err)
			return err;
		first = false;
	}
	return NULL;
}

static struct drgn_error *
c_declare_basic(struct drgn_qualified_type qualified_type,
		struct string_callback *name, size_t indent,
		struct string_builder *sb)
{
	struct drgn_error *err;

	err = append_tabs(indent, sb);
	if (err)
		return err;
	if (qualified_type.qualifiers) {
		err = c_append_qualifiers(qualified_type.qualifiers, sb);
		if (err)
			return err;
		err = string_builder_appendc(sb, ' ');
		if (err)
			return err;
	}
	err = string_builder_append(sb,
				    drgn_type_kind(qualified_type.type) == DRGN_TYPE_VOID ?
				    "void" : drgn_type_name(qualified_type.type));
	if (err)
		return err;
	if (name) {
		err = string_builder_appendc(sb, ' ');
		if (err)
			return err;
		err = string_callback_call(name, sb);
		if (err)
			return err;
	}
	return NULL;
}

static struct drgn_error *
c_append_tagged_name(struct drgn_qualified_type qualified_type, size_t indent,
		     struct string_builder *sb)
{
	struct drgn_error *err;
	const char *keyword, *tag;

	switch (drgn_type_kind(qualified_type.type)) {
	case DRGN_TYPE_STRUCT:
		keyword = "struct";
		break;
	case DRGN_TYPE_UNION:
		keyword = "union";
		break;
	case DRGN_TYPE_ENUM:
		keyword = "enum";
		break;
	default:
		DRGN_UNREACHABLE();
	}

	err = append_tabs(indent, sb);
	if (err)
		return err;
	if (qualified_type.qualifiers) {
		err = c_append_qualifiers(qualified_type.qualifiers, sb);
		if (err)
			return err;
		err = string_builder_appendc(sb, ' ');
		if (err)
			return err;
	}
	err = string_builder_append(sb, keyword);
	if (err)
		return err;

	tag = drgn_type_tag(qualified_type.type);
	if (tag) {
		err = string_builder_appendc(sb, ' ');
		if (err)
			return err;
		err = string_builder_append(sb, tag);
		if (err)
			return err;
	}

	return NULL;
}

static struct drgn_error *
c_declare_tagged(struct drgn_qualified_type qualified_type,
		 struct string_callback *name, size_t indent,
		 struct string_builder *sb)
{
	struct drgn_error *err;

	if (drgn_type_is_anonymous(qualified_type.type))
		err = c_define_type(qualified_type, indent, sb);
	else
		err = c_append_tagged_name(qualified_type, indent, sb);
	if (err)
		return err;

	if (name) {
		err = string_builder_appendc(sb, ' ');
		if (err)
			return err;
		err = string_callback_call(name, sb);
		if (err)
			return err;
	}
	return NULL;
}

static struct drgn_error *c_pointer_name(struct string_callback *name,
					 void *arg, struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_qualified_type *qualified_type = arg;
	struct drgn_qualified_type referenced_type;
	enum drgn_type_kind referenced_kind;
	bool parenthesize;

	referenced_type = drgn_type_type(qualified_type->type);
	referenced_kind = drgn_type_kind(referenced_type.type);
	parenthesize = (referenced_kind == DRGN_TYPE_ARRAY ||
			referenced_kind == DRGN_TYPE_FUNCTION);
	if (parenthesize) {
		err = string_builder_appendc(sb, '(');
		if (err)
			return err;
	}

	err = string_builder_appendc(sb, '*');
	if (err)
		return err;
	if (qualified_type->qualifiers) {
		err = string_builder_appendc(sb, ' ');
		if (err)
			return err;
		err = c_append_qualifiers(qualified_type->qualifiers, sb);
		if (err)
			return err;
		if (name) {
			err = string_builder_appendc(sb, ' ');
			if (err)
				return err;
		}
	}

	err = string_callback_call(name, sb);
	if (err)
		return err;

	if (parenthesize) {
		err = string_builder_appendc(sb, ')');
		if (err)
			return err;
	}

	return NULL;
}

static struct drgn_error *
c_declare_pointer(struct drgn_qualified_type qualified_type,
		  struct string_callback *name, size_t indent,
		  struct string_builder *sb)
{
	struct string_callback pointer_name = {
		.fn = c_pointer_name,
		.str = name,
		.arg = &qualified_type,
	};
	struct drgn_qualified_type referenced_type;

	referenced_type = drgn_type_type(qualified_type.type);
	return c_declare_variable(referenced_type, &pointer_name, indent, sb);
}

static struct drgn_error *c_array_name(struct string_callback *name, void *arg,
				       struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_qualified_type *qualified_type = arg;

	err = string_callback_call(name, sb);
	if (err)
		return err;

	if (drgn_type_is_complete(qualified_type->type)) {
		uint64_t length = drgn_type_length(qualified_type->type);

		return string_builder_appendf(sb, "[%" PRIu64 "]", length);
	} else {
		return string_builder_append(sb, "[]");
	}
}

static struct drgn_error *
c_declare_array(struct drgn_qualified_type qualified_type,
		struct string_callback *name, size_t indent,
		struct string_builder *sb)
{
	struct string_callback array_name = {
		.fn = c_array_name,
		.str = name,
		.arg = &qualified_type,
	};
	struct drgn_qualified_type element_type;

	element_type = drgn_type_type(qualified_type.type);
	return c_declare_variable(element_type, &array_name, indent, sb);
}

static struct drgn_error *
c_declare_function(struct drgn_qualified_type qualified_type,
		   struct string_callback *name, size_t indent,
		   struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_type_parameter *parameters;
	size_t num_parameters, i;
	struct drgn_qualified_type return_type;

	if (!name) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "function must have name");
	}

	parameters = drgn_type_parameters(qualified_type.type);
	num_parameters = drgn_type_num_parameters(qualified_type.type);

	return_type = drgn_type_type(qualified_type.type);
	err = c_declare_variable(return_type, name, indent, sb);
	if (err)
		return err;

	err = string_builder_appendc(sb, '(');
	if (err)
		return err;

	for (i = 0; i < num_parameters; i++) {
		const char *parameter_name = parameters[i].name;
		struct drgn_qualified_type parameter_type;
		struct string_callback name_cb = {
			.fn = c_variable_name,
			.arg = (void *)parameter_name,
		};

		err = drgn_parameter_type(&parameters[i], &parameter_type);
		if (err)
			return err;

		if (i > 0)  {
			err = string_builder_append(sb, ", ");
			if (err)
				return err;
		}
		err = c_declare_variable(parameter_type,
					 parameter_name && parameter_name[0] ?
					 &name_cb : NULL, 0, sb);
		if (err)
			return err;
	}
	if (num_parameters && drgn_type_is_variadic(qualified_type.type)) {
		err = string_builder_append(sb, ", ...");
		if (err)
			return err;
	} else if (!num_parameters &&
		   !drgn_type_is_variadic(qualified_type.type)) {
		err = string_builder_append(sb, "void");
		if (err)
			return err;
	}

	err = string_builder_appendc(sb, ')');
	if (err)
		return err;
	return NULL;
}

static struct drgn_error *
c_declare_variable(struct drgn_qualified_type qualified_type,
		   struct string_callback *name, size_t indent,
		   struct string_builder *sb)
{
	switch (drgn_type_kind(qualified_type.type)) {
	case DRGN_TYPE_VOID:
	case DRGN_TYPE_INT:
	case DRGN_TYPE_BOOL:
	case DRGN_TYPE_FLOAT:
	case DRGN_TYPE_COMPLEX:
	case DRGN_TYPE_TYPEDEF:
		return c_declare_basic(qualified_type, name, indent, sb);
	case DRGN_TYPE_STRUCT:
	case DRGN_TYPE_UNION:
	case DRGN_TYPE_ENUM:
		return c_declare_tagged(qualified_type, name, indent, sb);
	case DRGN_TYPE_POINTER:
		return c_declare_pointer(qualified_type, name, indent, sb);
	case DRGN_TYPE_ARRAY:
		return c_declare_array(qualified_type, name, indent, sb);
	case DRGN_TYPE_FUNCTION:
		return c_declare_function(qualified_type, name, indent, sb);
	}
	DRGN_UNREACHABLE();
}

static struct drgn_error *
c_define_compound(struct drgn_qualified_type qualified_type, size_t indent,
		  struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_type_member *members;
	size_t num_members, i;

	if (!drgn_type_is_complete(qualified_type.type)) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "cannot get definition of incomplete compound type");
	}

	members = drgn_type_members(qualified_type.type);
	num_members = drgn_type_num_members(qualified_type.type);

	err = c_append_tagged_name(qualified_type, indent, sb);
	if (err)
		return err;
	err = string_builder_append(sb, " {\n");
	if (err)
		return err;

	for (i = 0; i < num_members; i++) {
		const char *member_name = members[i].name;
		struct drgn_qualified_type member_type;
		struct string_callback name_cb = {
			.fn = c_variable_name,
			.arg = (void *)member_name,
		};

		err = drgn_member_type(&members[i], &member_type);
		if (err)
			return err;

		err = c_declare_variable(member_type,
					 member_name && member_name[0] ?
					 &name_cb : NULL, indent + 1, sb);
		if (err)
			return err;
		if (members[i].bit_field_size) {
			err = string_builder_appendf(sb, " : %" PRIu64,
						     members[i].bit_field_size);
			if (err)
				return err;
		}
		err = string_builder_append(sb, ";\n");
		if (err)
			return err;
	}

	err = append_tabs(indent, sb);
	if (err)
		return err;
	err = string_builder_appendc(sb, '}');
	if (err)
		return err;

	return NULL;
}

static struct drgn_error *
c_define_enum(struct drgn_qualified_type qualified_type, size_t indent,
	      struct string_builder *sb)
{
	struct drgn_error *err;
	const struct drgn_type_enumerator *enumerators;
	size_t num_enumerators, i;
	bool is_signed;

	if (!drgn_type_is_complete(qualified_type.type)) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "cannot get definition of incomplete enum type");
	}

	enumerators = drgn_type_enumerators(qualified_type.type);
	num_enumerators = drgn_type_num_enumerators(qualified_type.type);

	err = c_append_tagged_name(qualified_type, indent, sb);
	if (err)
		return err;
	err = string_builder_append(sb, " {\n");
	if (err)
		return err;

	is_signed = drgn_enum_type_is_signed(qualified_type.type);
	for (i = 0; i < num_enumerators; i++) {
		err = append_tabs(indent + 1, sb);
		if (err)
			return err;
		err = string_builder_append(sb, enumerators[i].name);
		if (err)
			return err;
		err = string_builder_append(sb, " = ");
		if (err)
			return err;
		if (is_signed) {
			err = string_builder_appendf(sb, "%" PRId64 ",\n",
						     enumerators[i].svalue);
			if (err)
				return err;
		} else {
			err = string_builder_appendf(sb, "%" PRIu64 ",\n",
						     enumerators[i].uvalue);
			if (err)
				return err;
		}
	}

	err = append_tabs(indent, sb);
	if (err)
		return err;
	err = string_builder_appendc(sb, '}');
	if (err)
		return err;

	return NULL;
}

static struct drgn_error *
c_define_typedef(struct drgn_qualified_type qualified_type, size_t indent,
		 struct string_builder *sb)
{
	struct string_callback typedef_name = {
		.fn = c_variable_name,
		.arg = (char *)drgn_type_name(qualified_type.type),
	};
	struct drgn_qualified_type aliased_type;
	struct drgn_error *err;

	err = append_tabs(indent, sb);
	if (err)
		return err;
	if (qualified_type.qualifiers) {
		err = c_append_qualifiers(qualified_type.qualifiers, sb);
		if (err)
			return err;
		err = string_builder_appendc(sb, ' ');
		if (err)
			return err;
	}
	err = string_builder_append(sb, "typedef ");
	if (err)
		return err;

	aliased_type = drgn_type_type(qualified_type.type);
	return c_declare_variable(aliased_type, &typedef_name, 0, sb);
}

static struct drgn_error *
c_define_type(struct drgn_qualified_type qualified_type, size_t indent,
	      struct string_builder *sb)
{
	switch (drgn_type_kind(qualified_type.type)) {
	case DRGN_TYPE_VOID:
	case DRGN_TYPE_INT:
	case DRGN_TYPE_BOOL:
	case DRGN_TYPE_FLOAT:
	case DRGN_TYPE_COMPLEX:
		return c_declare_basic(qualified_type, NULL, indent, sb);
	case DRGN_TYPE_STRUCT:
	case DRGN_TYPE_UNION:
		return c_define_compound(qualified_type, indent, sb);
	case DRGN_TYPE_ENUM:
		return c_define_enum(qualified_type, indent, sb);
	case DRGN_TYPE_TYPEDEF:
		return c_define_typedef(qualified_type, indent, sb);
	case DRGN_TYPE_POINTER:
		return c_declare_pointer(qualified_type, NULL, indent, sb);
	case DRGN_TYPE_ARRAY:
		return c_declare_array(qualified_type, NULL, indent, sb);
	case DRGN_TYPE_FUNCTION:
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "function type cannot be pretty-printed");
	}
	DRGN_UNREACHABLE();
}

static struct drgn_error *
c_anonymous_type_name(struct drgn_qualified_type qualified_type,
		      struct string_builder *sb)
{
	struct drgn_error *err;

	err = c_append_tagged_name(qualified_type, 0, sb);
	if (err)
		return err;
	err = string_builder_append(sb, " <anonymous>");
	if (err)
		return err;

	return NULL;
}

static struct drgn_error *
c_pretty_print_type_name_impl(struct drgn_qualified_type qualified_type,
			      struct string_builder *sb)
{
	if (drgn_type_is_anonymous(qualified_type.type)) {
		return c_anonymous_type_name(qualified_type, sb);
	} else if (drgn_type_kind(qualified_type.type) == DRGN_TYPE_FUNCTION) {
		struct string_callback name_cb = {
			.fn = c_variable_name,
			.arg = (void *)"",
		};

		return c_declare_function(qualified_type, &name_cb, 0, sb);
	} else {
		return c_declare_variable(qualified_type, NULL, 0, sb);
	}
}

static struct drgn_error *
c_pretty_print_type_name(struct drgn_qualified_type qualified_type, char **ret)
{
	struct drgn_error *err;
	struct string_builder sb;

	err = string_builder_init(&sb);
	if (err)
		return err;

	err = c_pretty_print_type_name_impl(qualified_type, &sb);
	if (err) {
		free(sb.str);
		return err;
	}
	*ret = sb.str;
	return NULL;
}

static struct drgn_error *
c_pretty_print_type(struct drgn_qualified_type qualified_type, char **ret)
{
	struct drgn_error *err;
	struct string_builder sb;

	err = string_builder_init(&sb);
	if (err)
		return err;

	if (drgn_type_is_complete(qualified_type.type))
		err = c_define_type(qualified_type, 0, &sb);
	else
		err = c_pretty_print_type_name_impl(qualified_type, &sb);
	if (err) {
		free(sb.str);
		return err;
	}
	*ret = sb.str;
	return NULL;
}

static struct drgn_error *
c_pretty_print_object_impl(const struct drgn_object *obj, bool cast,
			   bool dereference, size_t indent,
			   size_t one_line_columns, size_t multi_line_columns,
			   struct string_builder *sb);

static struct drgn_error *
c_pretty_print_int_object(const struct drgn_object *obj,
			  struct string_builder *sb)
{
	struct drgn_error *err;

	switch (obj->kind) {
	case DRGN_OBJECT_SIGNED: {
		int64_t svalue;

		err = drgn_object_read_signed(obj, &svalue);
		if (err)
			return err;
		return string_builder_appendf(sb, "%" PRId64, svalue);
	}
	case DRGN_OBJECT_UNSIGNED: {
		uint64_t uvalue;

		err = drgn_object_read_unsigned(obj, &uvalue);
		if (err)
			return err;
		return string_builder_appendf(sb, "%" PRIu64, uvalue);
	}
	default:
		DRGN_UNREACHABLE();
	}
}

static struct drgn_error *
c_pretty_print_float_object(const struct drgn_object *obj,
			    struct string_builder *sb)
{
	struct drgn_error *err;
	double fvalue;

	err = drgn_object_read_float(obj, &fvalue);
	if (err)
		return err;
	if (rint(fvalue) == fvalue) {
		return string_builder_appendf(sb, "%.1f", fvalue);
	} else {
		return string_builder_appendf(sb, "%.*g", DBL_DECIMAL_DIG,
					      fvalue);
	}
}

static struct drgn_error *c_pretty_print_members(const struct drgn_object *obj,
						 struct drgn_object *member,
						 struct drgn_type *type,
						 uint64_t bit_offset,
						 size_t indent,
						 size_t multi_line_columns,
						 struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_type_member *members;
	size_t num_members, i;

	if (!drgn_type_has_members(type))
		return NULL;

	members = drgn_type_members(type);
	num_members = drgn_type_num_members(type);
	for (i = 0; i < num_members; i++) {
		struct drgn_qualified_type member_type;

		err = drgn_member_type(&members[i], &member_type);
		if (err)
			return err;

		if (members[i].name) {
			size_t member_start, remaining_columns;

			if (multi_line_columns == 0)
				return &drgn_stop;

			err = string_builder_appendc(sb, '\n');
			if (err)
				return err;
			err = append_tabs(indent + 1, sb);
			if (err)
				return err;

			member_start = sb->len;
			err = string_builder_appendf(sb, ".%s = ",
						     members[i].name);
			if (err)
				return err;

			if (__builtin_sub_overflow(multi_line_columns,
						   8 * (indent + 1) +
						   sb->len - member_start + 1,
						   &remaining_columns))
				remaining_columns = 0;

			err = drgn_object_slice(member, obj, member_type,
						bit_offset + members[i].bit_offset,
						members[i].bit_field_size);
			if (err)
				return err;

			err = c_pretty_print_object_impl(member, true, false,
							 indent + 1,
							 remaining_columns,
							 multi_line_columns,
							 sb);
			if (err)
				return err;
			err = string_builder_appendc(sb, ',');
			if (err)
				return err;
		} else {
			err = c_pretty_print_members(obj, member,
						     member_type.type,
						     bit_offset + members[i].bit_offset,
						     indent, multi_line_columns,
						     sb);
			if (err)
				return err;
		}
	}
	return NULL;
}

static struct drgn_error *
c_pretty_print_compound_object(const struct drgn_object *obj,
			       struct drgn_type *underlying_type,
			       size_t indent, size_t multi_line_columns,
			       struct string_builder *sb)
{
	struct drgn_error *err;
	size_t old_len;
	struct drgn_object member;

	if (!drgn_type_is_complete(underlying_type)) {
		return drgn_error_format(DRGN_ERROR_TYPE,
					 "cannot format incomplete %s object",
					 drgn_type_kind(underlying_type) ==
					 DRGN_TYPE_STRUCT ? "struct" : "union");
	}

	err = string_builder_appendc(sb, '{');
	if (err)
		return err;
	old_len = sb->len;

	drgn_object_init(&member, obj->prog);
	err = c_pretty_print_members(obj, &member, underlying_type, 0, indent,
				     multi_line_columns, sb);
	drgn_object_deinit(&member);
	if (err)
		return err;
	if (sb->len != old_len) {
		err = string_builder_appendc(sb, '\n');
		if (err)
			return err;
		err = append_tabs(indent, sb);
		if (err)
			return err;
	}
	return string_builder_appendc(sb, '}');
}

static struct drgn_error *
c_pretty_print_enum_object(const struct drgn_object *obj,
			   struct drgn_type *underlying_type,
			   struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_type_enumerator *enumerators;
	size_t num_enumerators, i;

	if (!drgn_type_is_complete(underlying_type)) {
		return drgn_error_create(DRGN_ERROR_TYPE,
					 "cannot format incomplete enum object");
	}

	enumerators = drgn_type_enumerators(underlying_type);
	num_enumerators = drgn_type_num_enumerators(underlying_type);
	if (drgn_enum_type_is_signed(underlying_type)) {
		int64_t svalue;

		err = drgn_object_read_signed(obj, &svalue);
		if (err)
			return err;
		for (i = 0; i < num_enumerators; i++) {
			if (enumerators[i].svalue == svalue) {
				return string_builder_append(sb,
							     enumerators[i].name);
			}
		}
		return string_builder_appendf(sb, "%" PRId64, svalue);
	} else {
		uint64_t uvalue;

		err = drgn_object_read_unsigned(obj, &uvalue);
		if (err)
			return err;
		for (i = 0; i < num_enumerators; i++) {
			if (enumerators[i].uvalue == uvalue) {
				return string_builder_append(sb,
							     enumerators[i].name);
			}
		}
		return string_builder_appendf(sb, "%" PRIu64, uvalue);
	}
}

static bool is_character_type(struct drgn_type *type)
{
	switch (drgn_type_primitive(type)) {
	case DRGN_C_TYPE_CHAR:
	case DRGN_C_TYPE_SIGNED_CHAR:
	case DRGN_C_TYPE_UNSIGNED_CHAR:
		return true;
	default:
		return false;
	}
}

static struct drgn_error *
c_pretty_print_character(unsigned char c, struct string_builder *sb)
{
	switch (c) {
	case '\a':
		return string_builder_append(sb, "\\a");
	case '\b':
		return string_builder_append(sb, "\\b");
	case '\t':
		return string_builder_append(sb, "\\t");
	case '\n':
		return string_builder_append(sb, "\\n");
	case '\v':
		return string_builder_append(sb, "\\v");
	case '\f':
		return string_builder_append(sb, "\\f");
	case '\r':
		return string_builder_append(sb, "\\r");
	case '"':
		return string_builder_append(sb, "\\\"");
	case '\\':
		return string_builder_append(sb, "\\\\");
	default:
		if (c <= '\x1f' || c >= '\x7f')
			return string_builder_appendf(sb, "\\x%02x", c);
		else
			return string_builder_appendc(sb, c);
	}
}

static struct drgn_error *
c_pretty_print_string(struct drgn_memory_reader *reader, uint64_t address,
		      uint64_t length, struct string_builder *sb)
{
	struct drgn_error *err;

	err = string_builder_appendc(sb, '"');
	if (err)
		return err;
	while (length) {
		unsigned char c;

		err = drgn_memory_reader_read(reader, &c, address++, 1, false);
		if (err)
			return err;

		if (c == '\0') {
			break;
		} else {
			err = c_pretty_print_character(c, sb);
			if (err)
				return err;
		}
		length--;
	}
	return string_builder_appendc(sb, '"');
}

static struct drgn_error *
c_pretty_print_pointer_object(const struct drgn_object *obj,
			      struct drgn_type *underlying_type, bool cast,
			      bool dereference, size_t indent,
			      size_t one_line_columns,
			      size_t multi_line_columns,
			      struct string_builder *sb)
{
	struct drgn_error *err;
	bool is_c_string;
	uint64_t uvalue;
	size_t old_len, address_end;

	is_c_string = is_character_type(drgn_type_type(underlying_type).type);
	/* Always dereference strings. */
	if (is_c_string)
		dereference = true;

	old_len = sb->len;
	if (dereference && !is_c_string) {
		err = string_builder_appendc(sb, '*');
		if (err)
			return err;
	}
	if (cast) {
		err = string_builder_appendc(sb, '(');
		if (err)
			return err;
		err = c_pretty_print_type_name_impl(drgn_object_qualified_type(obj),
						    sb);
		if (err)
			return err;
		err = string_builder_appendc(sb, ')');
		if (err)
			return err;

	}

	err = drgn_object_read_unsigned(obj, &uvalue);
	if (err)
		return err;

	err = string_builder_appendf(sb, "0x%" PRIx64, uvalue);
	if (err || !dereference)
		return err;
	address_end = sb->len;

	err = string_builder_append(sb, " = ");
	if (err)
		return err;

	if (__builtin_sub_overflow(one_line_columns, sb->len - old_len,
				   &one_line_columns))
	    one_line_columns = 0;

	if (is_c_string) {
		err = c_pretty_print_string(obj->prog->reader, uvalue,
					    UINT64_MAX, sb);
	} else {
		struct drgn_object dereferenced;

		drgn_object_init(&dereferenced, obj->prog);
		err = drgn_object_dereference(&dereferenced, obj);
		if (err) {
			drgn_object_deinit(&dereferenced);
			if (err->code == DRGN_ERROR_TYPE)
				goto no_dereference;
			return err;
		}
		err = c_pretty_print_object_impl(&dereferenced, false, false,
						 indent, one_line_columns,
						 multi_line_columns, sb);
		drgn_object_deinit(&dereferenced);
	}
	if (!err || err->code != DRGN_ERROR_FAULT) {
		/* We either succeeded or hit a fatal error. */
		return err;
	}

no_dereference:
	/*
	 * We hit a non-fatal error. Delete the asterisk and truncate everything
	 * after the address.
	 */
	drgn_error_destroy(err);
	sb->len = address_end;
	if (!is_c_string) {
		sb->len--;
		memmove(&sb->str[old_len], &sb->str[old_len + 1],
			sb->len - old_len);
	}
	sb->str[sb->len] = '\0';
	return NULL;
}

static struct drgn_error *
c_pretty_print_array_object(const struct drgn_object *obj,
			    struct drgn_type *underlying_type, size_t indent,
			    size_t one_line_columns, size_t multi_line_columns,
			    struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_qualified_type element_type;
	uint64_t element_bit_size;
	struct drgn_object element;
	uint64_t length, i;
	size_t old_len, remaining_columns, start_columns;

	length = drgn_type_length(underlying_type);
	element_type = drgn_type_type(underlying_type);

	if (length && is_character_type(element_type.type)) {
		if (obj->is_reference) {
			return c_pretty_print_string(obj->prog->reader,
						     obj->reference.address,
						     length, sb);
		} else {
			const unsigned char *buf;
			uint64_t size;

			err = string_builder_appendc(sb, '"');
			if (err)
				return err;
			buf = (const unsigned char *)drgn_object_buffer(obj);
			size = drgn_value_size(obj->bit_size,
					       obj->value.bit_offset);
			for (i = 0; i < size; i++) {
				if (buf[i] == '\0')
					break;
				err = c_pretty_print_character(buf[i], sb);
				if (err)
					return err;
			}
			return string_builder_appendc(sb, '"');
		}
	}

	err = drgn_type_bit_size(element_type.type, &element_bit_size);
	if (err)
		return err;

	drgn_object_init(&element, obj->prog);
	while (length) {
		bool truthy;

		err = drgn_object_slice(&element, obj, element_type,
					(length - 1) * element_bit_size, 0);
		if (err)
			goto out;

		err = drgn_object_truthiness(&element, &truthy);
		if (err)
			goto out;
		if (!truthy)
			length--;
		else
			break;
	}
	if (!length) {
		err = string_builder_append(sb, "{}");
		goto out;
	}

	/* First, try to fit everything on one line. */
	err = string_builder_append(sb, "{ ");
	if (err)
		goto out;
	old_len = sb->len - 1; /* Minus one for the space. */
	if (__builtin_sub_overflow(one_line_columns, 2, &remaining_columns))
		remaining_columns = 0;
	/* Stop if we can't fit the comma, space, and closing brace. */
	for (i = 0; i < length && remaining_columns >= 3; i++) {
		size_t element_start;

		err = drgn_object_slice(&element, obj, element_type,
					i * element_bit_size, 0);
		if (err)
			goto out;

		element_start = sb->len;
		err = c_pretty_print_object_impl(&element, false, false,
						 indent + 1,
						 remaining_columns - 3, 0, sb);
		if (err && err->code == DRGN_ERROR_STOP)
			break;
		else if (err)
			goto out;

		err = string_builder_append(sb, ", ");
		if (err)
			goto out;

		if (__builtin_sub_overflow(remaining_columns,
					   sb->len - element_start,
					   &remaining_columns))
			break;
	}
	if (i >= length && remaining_columns >= 1) {
		/* Everything fit. */
		err = string_builder_appendc(sb, '}');
		goto out;
	}

	if (multi_line_columns == 0) {
		/* We were asked to fit on one line and we couldn't. */
		return &drgn_stop;
	}

	/* Start over (truncate the string) and use multiple lines. */
	sb->len = old_len;
	if (__builtin_sub_overflow(multi_line_columns, 8 * (indent + 1),
				   &start_columns))
		start_columns = 0;
	remaining_columns = 0;
	for (i = 0; i < length; i++) {
		size_t newline;

		err = drgn_object_slice(&element, obj, element_type,
					i * element_bit_size, 0);
		if (err)
			goto out;

		newline = sb->len;
		err = string_builder_appendc(sb, '\n');
		if (err)
			goto out;
		err = append_tabs(indent + 1, sb);
		if (err)
			goto out;

		if (start_columns > 1) {
			size_t element_start = sb->len;

			err = c_pretty_print_object_impl(&element, false, false,
							 0, start_columns - 1,
							 0, sb);
			if (!err) {
				size_t element_len = sb->len - element_start;

				if (element_len +
				    (remaining_columns == start_columns ? 1 : 2)
				    <= remaining_columns) {
					/*
					 * It would've fit on the previous line.
					 * Move it over.
					 */
					if (remaining_columns != start_columns) {
						sb->str[newline++] = ' ';
						remaining_columns--;
					}
					memmove(&sb->str[newline],
						&sb->str[element_start],
						element_len);
					sb->len = newline + element_len;
					err = string_builder_appendc(sb, ',');
					if (err)
						goto out;
					remaining_columns -= element_len + 1;
					continue;
				}
				if (element_len < start_columns) {
					/* It fit on the new line. */
					err = string_builder_appendc(sb, ',');
					if (err)
						goto out;
					remaining_columns = (start_columns -
							     element_len - 1);
					continue;
				}
			} else if (err->code != DRGN_ERROR_STOP) {
				goto out;
			}
			/* It didn't fit. */
			sb->len = element_start;
		}

		err = c_pretty_print_object_impl(&element, false, false,
						 indent + 1, 0,
						 multi_line_columns, sb);
		if (err)
			goto out;
		err = string_builder_appendc(sb, ',');
		if (err)
			goto out;
		remaining_columns = 0;
	}

	err = string_builder_appendc(sb, '\n');
	if (err)
		goto out;
	err = append_tabs(indent, sb);
	if (err)
		goto out;
	err = string_builder_appendc(sb, '}');
out:
	drgn_object_deinit(&element);
	return err;
}

static struct drgn_error *
c_pretty_print_function_object(const struct drgn_object *obj,
			       struct string_builder *sb)
{
	/* Function values currently aren't possible anyways. */
	if (!obj->is_reference) {
		return drgn_error_create(DRGN_ERROR_TYPE,
					 "cannot format function value");
	}
	return string_builder_appendf(sb, "0x%" PRIx64, obj->reference.address);
}

static struct drgn_error *
c_pretty_print_object_impl(const struct drgn_object *obj, bool cast,
			   bool dereference, size_t indent,
			   size_t one_line_columns, size_t multi_line_columns,
			   struct string_builder *sb)
{
	struct drgn_error *err;
	struct drgn_type *underlying_type = drgn_underlying_type(obj->type);

	/*
	 * Pointers are special because they can have an asterisk prefix if
	 * we're dereferencing them.
	 */
	if (drgn_type_kind(underlying_type) == DRGN_TYPE_POINTER) {
		return c_pretty_print_pointer_object(obj, underlying_type, cast,
						     dereference, indent,
						     one_line_columns,
						     multi_line_columns, sb);
	}

	if (cast) {
		size_t old_len = sb->len;

		err = string_builder_appendc(sb, '(');
		if (err)
			return err;
		err = c_pretty_print_type_name_impl(drgn_object_qualified_type(obj),
						    sb);
		if (err)
			return err;
		err = string_builder_appendc(sb, ')');
		if (err)
			return err;

		if (__builtin_sub_overflow(one_line_columns, sb->len - old_len,
					   &one_line_columns))
		    one_line_columns = 0;
	}

	switch (drgn_type_kind(underlying_type)) {
	case DRGN_TYPE_VOID:
		return drgn_error_create(DRGN_ERROR_TYPE,
					 "cannot format void object");
	case DRGN_TYPE_INT:
	case DRGN_TYPE_BOOL:
		return c_pretty_print_int_object(obj, sb);
	case DRGN_TYPE_FLOAT:
		return c_pretty_print_float_object(obj, sb);
	case DRGN_TYPE_COMPLEX:
		return drgn_error_format(DRGN_ERROR_TYPE,
					 "complex object formatting is not implemented");
	case DRGN_TYPE_STRUCT:
	case DRGN_TYPE_UNION:
		return c_pretty_print_compound_object(obj, underlying_type,
						      indent,
						      multi_line_columns, sb);
	case DRGN_TYPE_ENUM:
		return c_pretty_print_enum_object(obj, underlying_type, sb);
	case DRGN_TYPE_ARRAY:
		return c_pretty_print_array_object(obj, underlying_type, indent,
						   one_line_columns,
						   multi_line_columns, sb);
	case DRGN_TYPE_FUNCTION:
		return c_pretty_print_function_object(obj, sb);
	default:
		DRGN_UNREACHABLE();
	}
}

static struct drgn_error *c_pretty_print_object(const struct drgn_object *obj,
						size_t columns, char **ret)
{
	struct drgn_error *err;
	struct string_builder sb;

	err = string_builder_init(&sb);
	if (err)
		return err;

	err = c_pretty_print_object_impl(obj, true, true, 0, columns,
					 max(columns, (size_t)1), &sb);
	if (err) {
		free(sb.str);
		return err;
	}
	*ret = sb.str;
	return NULL;
}

/* This obviously incomplete since we only handle the tokens we care about. */
enum {
	C_TOKEN_EOF = -1,
	MIN_KEYWORD_TOKEN,
	MIN_SPECIFIER_TOKEN = MIN_KEYWORD_TOKEN,
	C_TOKEN_VOID = MIN_SPECIFIER_TOKEN,
	C_TOKEN_CHAR,
	C_TOKEN_SHORT,
	C_TOKEN_INT,
	C_TOKEN_LONG,
	C_TOKEN_SIGNED,
	C_TOKEN_UNSIGNED,
	C_TOKEN_BOOL,
	C_TOKEN_FLOAT,
	C_TOKEN_DOUBLE,
	C_TOKEN_COMPLEX,
	MAX_SPECIFIER_TOKEN = C_TOKEN_COMPLEX,
	MIN_QUALIFIER_TOKEN,
	C_TOKEN_CONST = MIN_QUALIFIER_TOKEN,
	C_TOKEN_RESTRICT,
	C_TOKEN_VOLATILE,
	C_TOKEN_ATOMIC,
	MAX_QUALIFIER_TOKEN = C_TOKEN_ATOMIC,
	C_TOKEN_STRUCT,
	C_TOKEN_UNION,
	C_TOKEN_ENUM,
	MAX_KEYWORD_TOKEN = C_TOKEN_ENUM,
	C_TOKEN_LPAREN,
	C_TOKEN_RPAREN,
	C_TOKEN_LBRACKET,
	C_TOKEN_RBRACKET,
	C_TOKEN_ASTERISK,
	C_TOKEN_DOT,
	C_TOKEN_NUMBER,
	C_TOKEN_IDENTIFIER,
};

static const char *token_spelling[] = {
	[C_TOKEN_VOID] = "void",
	[C_TOKEN_CHAR] = "char",
	[C_TOKEN_SHORT] = "short",
	[C_TOKEN_INT] = "int",
	[C_TOKEN_LONG] = "long",
	[C_TOKEN_SIGNED] = "signed",
	[C_TOKEN_UNSIGNED] = "unsigned",
	[C_TOKEN_BOOL] = "_Bool",
	[C_TOKEN_FLOAT] = "float",
	[C_TOKEN_DOUBLE] = "double",
	[C_TOKEN_COMPLEX] = "_Complex",
	[C_TOKEN_CONST] = "const",
	[C_TOKEN_RESTRICT] = "restrict",
	[C_TOKEN_VOLATILE] = "volatile",
	[C_TOKEN_ATOMIC] = "_Atomic",
	[C_TOKEN_STRUCT] = "struct",
	[C_TOKEN_UNION] = "union",
	[C_TOKEN_ENUM] = "enum",
};

DEFINE_HASH_MAP(c_keyword_map, struct string, int, string_hash, string_eq);

static struct c_keyword_map c_keywords;

__attribute__((constructor(101)))
static void c_keywords_init(void)
{
	int i;

	c_keyword_map_init(&c_keywords);
	for (i = MIN_KEYWORD_TOKEN; i <= MAX_KEYWORD_TOKEN; i++) {
		struct string key = {
			.str = token_spelling[i],
			.len = strlen(token_spelling[i]),
		};

		if (!c_keyword_map_insert(&c_keywords, &key, &i))
			abort();
	}
}

__attribute__((destructor(101)))
static void c_keywords_deinit(void)
{
	c_keyword_map_deinit(&c_keywords);
}

struct drgn_error *drgn_lexer_c(struct drgn_lexer *lexer,
				struct drgn_token *token) {
	const char *p = lexer->p;

	while (isspace(*p))
		p++;

	token->value = p;
	switch (*p) {
	case '\0':
		token->kind = C_TOKEN_EOF;
		break;
	case '(':
		token->kind = C_TOKEN_LPAREN;
		p++;
		break;
	case ')':
		token->kind = C_TOKEN_RPAREN;
		p++;
		break;
	case '[':
		token->kind = C_TOKEN_LBRACKET;
		p++;
		break;
	case ']':
		token->kind = C_TOKEN_RBRACKET;
		p++;
		break;
	case '*':
		token->kind = C_TOKEN_ASTERISK;
		p++;
		break;
	case '.':
		token->kind = C_TOKEN_DOT;
		p++;
		break;
	default:
		if (isalpha(*p) || *p == '_') {
			struct string key;
			int *kind;

			do {
				p++;
			} while (isalnum(*p) || *p == '_');

			key.str = token->value;
			key.len = p - token->value;
			kind = c_keyword_map_search(&c_keywords, &key);
			token->kind = kind ? *kind : C_TOKEN_IDENTIFIER;
		} else if ('0' <= *p && *p <= '9') {
			token->kind = C_TOKEN_NUMBER;
			if (*p++ == '0' && *p == 'x') {
				p++;
				while (('0' <= *p && *p <= '9') ||
				       ('a' <= *p && *p <= 'f') ||
				       ('A' <= *p && *p <= 'F')) {
					p++;
				}
				if (p - token->value <= 2) {
					return drgn_error_create(DRGN_ERROR_SYNTAX,
								 "invalid number");
				}
			} else {
				while ('0' <= *p && *p <= '9')
					p++;
			}
			if (isalpha(*p) || *p == '_') {
				return drgn_error_create(DRGN_ERROR_SYNTAX,
							 "invalid number");
			}
		} else {
			return drgn_error_format(DRGN_ERROR_SYNTAX,
						 "invalid character \\x%02x", (unsigned char)*p);
		}
		break;
	}

	token->len = p - token->value;
	lexer->p = p;
	return NULL;
}

static struct drgn_error *c_token_to_u64(const struct drgn_token *token,
					 uint64_t *ret)
{
	uint64_t x = 0;
	size_t i;

	assert(token->kind == C_TOKEN_NUMBER);
	if (token->len > 2 && token->value[0] == '0' &&
	    token->value[1] == 'x') {
		for (i = 2; i < token->len; i++) {
			char c = token->value[i];
			int digit;

			if ('0' <= c && c <= '9')
				digit = c - '0';
			else if ('a' <= c && c <= 'f')
				digit = c - 'a';
			else /* ('A' <= c && c <= 'F') */
				digit = c - 'A';
			if (x > UINT64_MAX / 16)
				goto overflow;
			x *= 16;
			if (x > UINT64_MAX - digit)
				goto overflow;
			x += digit;
		}
	} else if (token->value[0] == '0') {
		for (i = 1; i < token->len; i++) {
			int digit;

			digit = token->value[i] - '0';
			if (x > UINT64_MAX / 8)
				goto overflow;
			x *= 8;
			if (x > UINT64_MAX - digit)
				goto overflow;
			x += digit;
		}
	} else {
		for (i = 0; i < token->len; i++) {
			int digit;

			digit = token->value[i] - '0';
			if (x > UINT64_MAX / 10)
				goto overflow;
			x *= 10;
			if (x > UINT64_MAX - digit)
				goto overflow;
			x += digit;
		}
	}
	*ret = x;
	return NULL;

overflow:
	return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
				 "number is too large");
}

enum c_type_specifier {
	SPECIFIER_ERROR,
	SPECIFIER_VOID,
	SPECIFIER_CHAR,
	SPECIFIER_SIGNED_CHAR,
	SPECIFIER_UNSIGNED_CHAR,
	SPECIFIER_SHORT,
	SPECIFIER_SHORT_INT,
	SPECIFIER_SIGNED_SHORT_INT,
	SPECIFIER_UNSIGNED_SHORT_INT,
	SPECIFIER_SIGNED_SHORT,
	SPECIFIER_UNSIGNED_SHORT,
	SPECIFIER_INT,
	SPECIFIER_SIGNED_INT,
	SPECIFIER_UNSIGNED_INT,
	SPECIFIER_LONG,
	SPECIFIER_LONG_INT,
	SPECIFIER_SIGNED_LONG,
	SPECIFIER_UNSIGNED_LONG,
	SPECIFIER_SIGNED_LONG_INT,
	SPECIFIER_UNSIGNED_LONG_INT,
	SPECIFIER_LONG_LONG,
	SPECIFIER_LONG_LONG_INT,
	SPECIFIER_SIGNED_LONG_LONG_INT,
	SPECIFIER_UNSIGNED_LONG_LONG_INT,
	SPECIFIER_SIGNED_LONG_LONG,
	SPECIFIER_UNSIGNED_LONG_LONG,
	SPECIFIER_SIGNED,
	SPECIFIER_UNSIGNED,
	SPECIFIER_BOOL,
	SPECIFIER_FLOAT,
	SPECIFIER_DOUBLE,
	SPECIFIER_LONG_DOUBLE,
	SPECIFIER_NONE,
	NUM_SPECIFIER_STATES,
};

static const char *specifier_spelling[NUM_SPECIFIER_STATES] = {
	[SPECIFIER_VOID] = "void",
	[SPECIFIER_CHAR] = "char",
	[SPECIFIER_SIGNED_CHAR] = "signed char",
	[SPECIFIER_UNSIGNED_CHAR] = "unsigned char",
	[SPECIFIER_SHORT] = "short",
	[SPECIFIER_SHORT_INT] = "short int",
	[SPECIFIER_SIGNED_SHORT_INT] = "signed short int",
	[SPECIFIER_UNSIGNED_SHORT_INT] = "unsigned short int",
	[SPECIFIER_SIGNED_SHORT] = "signed short",
	[SPECIFIER_UNSIGNED_SHORT] = "unsigned short",
	[SPECIFIER_INT] = "int",
	[SPECIFIER_SIGNED_INT] = "signed int",
	[SPECIFIER_UNSIGNED_INT] = "unsigned int",
	[SPECIFIER_LONG] = "long",
	[SPECIFIER_LONG_INT] = "long int",
	[SPECIFIER_SIGNED_LONG] = "signed long",
	[SPECIFIER_UNSIGNED_LONG] = "unsigned long",
	[SPECIFIER_SIGNED_LONG_INT] = "signed long int",
	[SPECIFIER_UNSIGNED_LONG_INT] = "unsigned long int",
	[SPECIFIER_LONG_LONG] = "long long",
	[SPECIFIER_LONG_LONG_INT] = "long long int",
	[SPECIFIER_SIGNED_LONG_LONG_INT] = "signed long long int",
	[SPECIFIER_UNSIGNED_LONG_LONG_INT] = "unsigned long long int",
	[SPECIFIER_SIGNED_LONG_LONG] = "signed long long",
	[SPECIFIER_UNSIGNED_LONG_LONG] = "unsigned long long",
	[SPECIFIER_SIGNED] = "signed",
	[SPECIFIER_UNSIGNED] = "unsigned",
	[SPECIFIER_BOOL] = "_Bool",
	[SPECIFIER_FLOAT] = "float",
	[SPECIFIER_DOUBLE] = "double",
	[SPECIFIER_LONG_DOUBLE] = "long double",
};

static const enum drgn_qualifiers qualifier_from_token[MAX_QUALIFIER_TOKEN + 1] = {
	[C_TOKEN_CONST] = DRGN_QUALIFIER_CONST,
	[C_TOKEN_RESTRICT] = DRGN_QUALIFIER_RESTRICT,
	[C_TOKEN_VOLATILE] = DRGN_QUALIFIER_VOLATILE,
	[C_TOKEN_ATOMIC] = DRGN_QUALIFIER_ATOMIC,
};

static const enum c_type_specifier
specifier_transition[NUM_SPECIFIER_STATES][MAX_SPECIFIER_TOKEN + 1] = {
	[SPECIFIER_NONE] = {
		[C_TOKEN_VOID] = SPECIFIER_VOID,
		[C_TOKEN_CHAR] = SPECIFIER_CHAR,
		[C_TOKEN_SHORT] = SPECIFIER_SHORT,
		[C_TOKEN_INT] = SPECIFIER_INT,
		[C_TOKEN_LONG] = SPECIFIER_LONG,
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED,
		[C_TOKEN_BOOL] = SPECIFIER_BOOL,
		[C_TOKEN_FLOAT] = SPECIFIER_FLOAT,
		[C_TOKEN_DOUBLE] = SPECIFIER_DOUBLE,
	},
	[SPECIFIER_VOID] = {},
	[SPECIFIER_CHAR] = {
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_CHAR,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_CHAR,
	},
	[SPECIFIER_SIGNED_CHAR] = {},
	[SPECIFIER_UNSIGNED_CHAR] = {},
	[SPECIFIER_SHORT] = {
		[C_TOKEN_INT] = SPECIFIER_SHORT_INT,
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_SHORT,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_SHORT,
	},
	[SPECIFIER_SHORT_INT] = {
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_SHORT_INT,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_SHORT_INT,
	},
	[SPECIFIER_SIGNED_SHORT_INT] = {},
	[SPECIFIER_UNSIGNED_SHORT_INT] = {},
	[SPECIFIER_SIGNED_SHORT] = {
		[C_TOKEN_INT] = SPECIFIER_SIGNED_SHORT_INT,
	},
	[SPECIFIER_UNSIGNED_SHORT] = {
		[C_TOKEN_INT] = SPECIFIER_UNSIGNED_SHORT_INT,
	},
	[SPECIFIER_INT] = {
		[C_TOKEN_SHORT] = SPECIFIER_SHORT_INT,
		[C_TOKEN_LONG] = SPECIFIER_LONG_INT,
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_INT,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_INT,
	},
	[SPECIFIER_SIGNED_INT] = {
		[C_TOKEN_SHORT] = SPECIFIER_SIGNED_SHORT_INT,
		[C_TOKEN_LONG] = SPECIFIER_SIGNED_LONG_INT,
	},
	[SPECIFIER_UNSIGNED_INT] = {
		[C_TOKEN_SHORT] = SPECIFIER_UNSIGNED_SHORT_INT,
		[C_TOKEN_LONG] = SPECIFIER_UNSIGNED_LONG_INT,
	},
	[SPECIFIER_LONG] = {
		[C_TOKEN_INT] = SPECIFIER_LONG_INT,
		[C_TOKEN_LONG] = SPECIFIER_LONG_LONG,
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_LONG,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_LONG,
		[C_TOKEN_DOUBLE] = SPECIFIER_LONG_DOUBLE,
	},
	[SPECIFIER_LONG_INT] = {
		[C_TOKEN_LONG] = SPECIFIER_LONG_LONG_INT,
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_LONG_INT,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_LONG_INT,
	},
	[SPECIFIER_SIGNED_LONG] = {
		[C_TOKEN_LONG] = SPECIFIER_SIGNED_LONG_LONG,
		[C_TOKEN_INT] = SPECIFIER_SIGNED_LONG_INT,
	},
	[SPECIFIER_UNSIGNED_LONG] = {
		[C_TOKEN_LONG] = SPECIFIER_UNSIGNED_LONG_LONG,
		[C_TOKEN_INT] = SPECIFIER_UNSIGNED_LONG_INT,
	},
	[SPECIFIER_SIGNED_LONG_INT] = {
		[C_TOKEN_LONG] = SPECIFIER_SIGNED_LONG_LONG_INT,
	},
	[SPECIFIER_UNSIGNED_LONG_INT] = {
		[C_TOKEN_LONG] = SPECIFIER_UNSIGNED_LONG_LONG_INT,
	},
	[SPECIFIER_LONG_LONG] = {
		[C_TOKEN_INT] = SPECIFIER_LONG_LONG_INT,
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_LONG_LONG,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_LONG_LONG,
	},
	[SPECIFIER_LONG_LONG_INT] = {
		[C_TOKEN_SIGNED] = SPECIFIER_SIGNED_LONG_LONG_INT,
		[C_TOKEN_UNSIGNED] = SPECIFIER_UNSIGNED_LONG_LONG_INT,
	},
	[SPECIFIER_SIGNED_LONG_LONG_INT] = {},
	[SPECIFIER_UNSIGNED_LONG_LONG_INT] = {},
	[SPECIFIER_SIGNED_LONG_LONG] = {
		[C_TOKEN_INT] = SPECIFIER_SIGNED_LONG_LONG_INT,
	},
	[SPECIFIER_UNSIGNED_LONG_LONG] = {
		[C_TOKEN_INT] = SPECIFIER_UNSIGNED_LONG_LONG_INT,
	},
	[SPECIFIER_SIGNED] = {
		[C_TOKEN_CHAR] = SPECIFIER_SIGNED_CHAR,
		[C_TOKEN_SHORT] = SPECIFIER_SIGNED_SHORT,
		[C_TOKEN_INT] = SPECIFIER_SIGNED_INT,
		[C_TOKEN_LONG] = SPECIFIER_SIGNED_LONG,
	},
	[SPECIFIER_UNSIGNED] = {
		[C_TOKEN_CHAR] = SPECIFIER_UNSIGNED_CHAR,
		[C_TOKEN_SHORT] = SPECIFIER_UNSIGNED_SHORT,
		[C_TOKEN_INT] = SPECIFIER_UNSIGNED_INT,
		[C_TOKEN_LONG] = SPECIFIER_UNSIGNED_LONG,
	},
	[SPECIFIER_BOOL] = {},
	[SPECIFIER_FLOAT] = {},
	[SPECIFIER_DOUBLE] = {
		[C_TOKEN_LONG] = SPECIFIER_LONG_DOUBLE,
	},
	[SPECIFIER_LONG_DOUBLE] = {},
};

static const enum drgn_primitive_type specifier_kind[NUM_SPECIFIER_STATES] = {
	[SPECIFIER_VOID] = DRGN_C_TYPE_VOID,
	[SPECIFIER_CHAR] = DRGN_C_TYPE_CHAR,
	[SPECIFIER_SIGNED_CHAR] = DRGN_C_TYPE_SIGNED_CHAR,
	[SPECIFIER_UNSIGNED_CHAR] = DRGN_C_TYPE_UNSIGNED_CHAR,
	[SPECIFIER_SHORT] = DRGN_C_TYPE_SHORT,
	[SPECIFIER_SHORT_INT] = DRGN_C_TYPE_SHORT,
	[SPECIFIER_SIGNED_SHORT_INT] = DRGN_C_TYPE_SHORT,
	[SPECIFIER_UNSIGNED_SHORT_INT] = DRGN_C_TYPE_UNSIGNED_SHORT,
	[SPECIFIER_SIGNED_SHORT] = DRGN_C_TYPE_SHORT,
	[SPECIFIER_UNSIGNED_SHORT] = DRGN_C_TYPE_UNSIGNED_SHORT,
	[SPECIFIER_INT] = DRGN_C_TYPE_INT,
	[SPECIFIER_SIGNED_INT] = DRGN_C_TYPE_INT,
	[SPECIFIER_UNSIGNED_INT] = DRGN_C_TYPE_UNSIGNED_INT,
	[SPECIFIER_LONG] = DRGN_C_TYPE_LONG,
	[SPECIFIER_LONG_INT] = DRGN_C_TYPE_LONG,
	[SPECIFIER_SIGNED_LONG] = DRGN_C_TYPE_LONG,
	[SPECIFIER_UNSIGNED_LONG] = DRGN_C_TYPE_UNSIGNED_LONG,
	[SPECIFIER_SIGNED_LONG_INT] = DRGN_C_TYPE_LONG,
	[SPECIFIER_UNSIGNED_LONG_INT] = DRGN_C_TYPE_UNSIGNED_LONG,
	[SPECIFIER_LONG_LONG] = DRGN_C_TYPE_LONG_LONG,
	[SPECIFIER_LONG_LONG_INT] = DRGN_C_TYPE_LONG_LONG,
	[SPECIFIER_SIGNED_LONG_LONG_INT] = DRGN_C_TYPE_LONG_LONG,
	[SPECIFIER_UNSIGNED_LONG_LONG_INT] = DRGN_C_TYPE_UNSIGNED_LONG_LONG,
	[SPECIFIER_SIGNED_LONG_LONG] = DRGN_C_TYPE_LONG_LONG,
	[SPECIFIER_UNSIGNED_LONG_LONG] = DRGN_C_TYPE_UNSIGNED_LONG_LONG,
	[SPECIFIER_SIGNED] = DRGN_C_TYPE_INT,
	[SPECIFIER_UNSIGNED] = DRGN_C_TYPE_UNSIGNED_INT,
	[SPECIFIER_BOOL] = DRGN_C_TYPE_BOOL,
	[SPECIFIER_FLOAT] = DRGN_C_TYPE_FLOAT,
	[SPECIFIER_DOUBLE] = DRGN_C_TYPE_DOUBLE,
	[SPECIFIER_LONG_DOUBLE] = DRGN_C_TYPE_LONG_DOUBLE,
};

enum drgn_primitive_type c_parse_specifier_list(const char *s)
{
	struct drgn_error *err;
	struct drgn_lexer lexer;
	enum c_type_specifier specifier = SPECIFIER_NONE;
	enum drgn_primitive_type primitive = DRGN_NOT_PRIMITIVE_TYPE;

	drgn_lexer_init(&lexer, drgn_lexer_c, s);

	for (;;) {
		struct drgn_token token;

		err = drgn_lexer_pop(&lexer, &token);
		if (err) {
			drgn_error_destroy(err);
			goto out;
		}

		if (MIN_SPECIFIER_TOKEN <= token.kind &&
		    token.kind <= MAX_SPECIFIER_TOKEN)
			specifier = specifier_transition[specifier][token.kind];
		else if (token.kind == C_TOKEN_EOF)
			break;
		else
			specifier = SPECIFIER_ERROR;
		if (specifier == SPECIFIER_ERROR)
			goto out;
	}

	primitive = specifier_kind[specifier];
out:
	drgn_lexer_deinit(&lexer);
	return primitive;
}

static struct drgn_error *
c_parse_specifier_qualifier_list(struct drgn_type_index *tindex,
				 struct drgn_lexer *lexer, const char *filename,
				 struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	enum c_type_specifier specifier = SPECIFIER_NONE;
	enum drgn_qualifiers qualifiers = 0;
	const char *identifier = NULL;
	size_t identifier_len = 0;
	int tag_token = C_TOKEN_EOF;

	for (;;) {
		struct drgn_token token;

		err = drgn_lexer_pop(lexer, &token);
		if (err)
			return err;

		/* type-qualifier */
		if (MIN_QUALIFIER_TOKEN <= token.kind &&
		    token.kind <= MAX_QUALIFIER_TOKEN) {
			qualifiers |= qualifier_from_token[token.kind];
		/* type-specifier */
		} else if (MIN_SPECIFIER_TOKEN <= token.kind &&
			   token.kind <= MAX_SPECIFIER_TOKEN) {
			enum c_type_specifier prev_specifier;

			if (tag_token != C_TOKEN_EOF) {
				return drgn_error_format(DRGN_ERROR_SYNTAX,
							 "cannot combine '%s' with '%s'",
							 token_spelling[token.kind],
							 token_spelling[tag_token]);
			}
			if (identifier) {
				return drgn_error_format(DRGN_ERROR_SYNTAX,
							 "cannot combine '%s' with identifier",
							 token_spelling[token.kind]);
			}
			prev_specifier = specifier;
			specifier = specifier_transition[specifier][token.kind];
			if (specifier == SPECIFIER_ERROR) {
				return drgn_error_format(DRGN_ERROR_SYNTAX,
							 "cannot combine '%s' with '%s'",
							 token_spelling[token.kind],
							 specifier_spelling[prev_specifier]);
			}
		} else if (token.kind == C_TOKEN_IDENTIFIER &&
			   specifier == SPECIFIER_NONE && !identifier) {
			identifier = token.value;
			identifier_len = token.len;
		} else if (token.kind == C_TOKEN_STRUCT ||
			   token.kind == C_TOKEN_UNION ||
			   token.kind == C_TOKEN_ENUM) {
			if (identifier) {
				return drgn_error_format(DRGN_ERROR_SYNTAX,
							 "cannot combine '%s' with identifier",
							 token_spelling[token.kind]);
			}
			if (specifier != SPECIFIER_NONE) {
				return drgn_error_format(DRGN_ERROR_SYNTAX,
							 "cannot combine '%s' with '%s'",
							 token_spelling[token.kind],
							 specifier_spelling[specifier]);
			}
			tag_token = token.kind;
			err = drgn_lexer_pop(lexer, &token);
			if (err)
				return err;
			if (token.kind != C_TOKEN_IDENTIFIER) {
				return drgn_error_format(DRGN_ERROR_SYNTAX,
							 "expected identifier after '%s'",
							 token_spelling[token.kind]);

			}
			identifier = token.value;
			identifier_len = token.len;
		} else {
			err = drgn_lexer_push(lexer, &token);
			if (err)
				return err;
			break;
		}
	}

	if (specifier == SPECIFIER_NONE) {
		enum drgn_type_kind kind;

		if (tag_token == C_TOKEN_STRUCT) {
			kind = DRGN_TYPE_STRUCT;
		} else if (tag_token == C_TOKEN_UNION) {
			kind = DRGN_TYPE_UNION;
		} else if (tag_token == C_TOKEN_ENUM) {
			kind = DRGN_TYPE_ENUM;
		} else if (identifier) {
			if (strncmp(identifier, "size_t",
				    strlen("size_t")) == 0) {
				ret->type = tindex->primitive_types[DRGN_C_TYPE_SIZE_T];
				ret->qualifiers = 0;
				goto out;
			} else if (strncmp(identifier, "ptrdiff_t",
				    strlen("ptrdiff_t")) == 0) {
				ret->type = tindex->primitive_types[DRGN_C_TYPE_PTRDIFF_T];
				ret->qualifiers = 0;
				goto out;
			} else {
				kind = DRGN_TYPE_TYPEDEF;
			}
		} else {
			return drgn_error_create(DRGN_ERROR_SYNTAX,
						 "expected type specifier");
		}

		err = drgn_type_index_find_internal(tindex, kind, identifier,
						    identifier_len, filename,
						    ret);
		if (err)
			return err;
	} else {
		ret->type = tindex->primitive_types[specifier_kind[specifier]];
		ret->qualifiers = 0;
	}
out:
	ret->qualifiers |= qualifiers;
	return NULL;
}

struct c_declarator {
	/* C_TOKEN_ASTERISK or C_TOKEN_LBRACKET. */
	int kind;
	enum drgn_qualifiers qualifiers;
	/* Only for C_TOKEN_LBRACKET. */
	bool is_complete;
	uint64_t length;
	struct c_declarator *next;
};

/* These functions don't free the declarator list on error. */
static struct drgn_error *
c_parse_abstract_declarator(struct drgn_type_index *tindex,
			    struct drgn_lexer *lexer,
			    struct c_declarator **outer,
			    struct c_declarator **inner);

static struct drgn_error *
c_parse_optional_type_qualifier_list(struct drgn_lexer *lexer,
				     enum drgn_qualifiers *qualifiers)
{
	struct drgn_error *err;
	struct drgn_token token;

	*qualifiers = 0;
	for (;;) {
		err = drgn_lexer_pop(lexer, &token);
		if (err)
			return err;

		if (token.kind < MIN_QUALIFIER_TOKEN ||
		    token.kind > MAX_QUALIFIER_TOKEN) {
			err = drgn_lexer_push(lexer, &token);
			if (err)
				return err;
			return NULL;
		}
		*qualifiers |= qualifier_from_token[token.kind];
	}
}

static struct drgn_error *
c_parse_pointer(struct drgn_type_index *tindex, struct drgn_lexer *lexer,
		struct c_declarator **outer, struct c_declarator **inner)
{
	struct drgn_error *err;
	struct drgn_token token;

	err = drgn_lexer_pop(lexer, &token);
	if (err)
		return err;
	if (token.kind != C_TOKEN_ASTERISK)
		return drgn_error_create(DRGN_ERROR_SYNTAX, "expected '*'");

	*inner = NULL;
	for (;;) {
		struct c_declarator *tmp;

		tmp = malloc(sizeof(*tmp));
		if (!tmp)
			return &drgn_enomem;

		tmp->kind = C_TOKEN_ASTERISK;
		tmp->next = *outer;
		*outer = tmp;

		err = c_parse_optional_type_qualifier_list(lexer,
							   &(*outer)->qualifiers);
		if (err)
			return err;
		if (!*inner)
			*inner = *outer;

		err = drgn_lexer_pop(lexer, &token);
		if (err)
			return err;
		if (token.kind != C_TOKEN_ASTERISK)
			return drgn_lexer_push(lexer, &token);
	}
}

static struct drgn_error *
c_parse_direct_abstract_declarator(struct drgn_type_index *tindex,
				   struct drgn_lexer *lexer,
				   struct c_declarator **outer,
				   struct c_declarator **inner)
{
	struct drgn_error *err;
	struct drgn_token token;

	*inner = NULL;

	err = drgn_lexer_pop(lexer, &token);
	if (err)
		return err;
	if (token.kind == C_TOKEN_LPAREN) {
		struct drgn_token token2;

		err = drgn_lexer_peek(lexer, &token2);
		if (err)
			return err;
		if (token2.kind == C_TOKEN_ASTERISK ||
		    token2.kind == C_TOKEN_LPAREN ||
		    token2.kind == C_TOKEN_LBRACKET) {
			err = c_parse_abstract_declarator(tindex, lexer, outer,
							  inner);
			if (err)
				return err;
			err = drgn_lexer_pop(lexer, &token2);
			if (err)
				return err;
			if (token2.kind != C_TOKEN_RPAREN) {
				return drgn_error_create(DRGN_ERROR_SYNTAX,
							 "expected ')'");
			}
			err = drgn_lexer_pop(lexer, &token);
			if (err)
				return err;
		}
	}

	for (;;) {
		if (token.kind == C_TOKEN_LBRACKET) {
			struct c_declarator *tmp;

			err = drgn_lexer_pop(lexer, &token);
			if (err)
				return err;

			tmp = malloc(sizeof(*tmp));
			if (!tmp)
				return &drgn_enomem;

			tmp->kind = C_TOKEN_LBRACKET;
			tmp->qualifiers = 0;
			if (token.kind == C_TOKEN_NUMBER) {
				tmp->is_complete = true;
				err = c_token_to_u64(&token, &tmp->length);
				if (err) {
					free(tmp);
					return err;
				}
				err = drgn_lexer_pop(lexer, &token);
				if (err) {
					free(tmp);
					return err;
				}
			} else {
				tmp->is_complete = false;
			}

			if (*inner) {
				tmp->next = (*inner)->next;
				*inner = (*inner)->next = tmp;
			} else {
				tmp->next = *outer;
				*outer = *inner = tmp;
			}
			if (token.kind != C_TOKEN_RBRACKET) {
				return drgn_error_create(DRGN_ERROR_SYNTAX,
							 "expected ']'");
			}
		} else if (token.kind == C_TOKEN_LPAREN) {
			return drgn_error_create(DRGN_ERROR_SYNTAX,
						 "function pointer types are not implemented");
		} else {
			err = drgn_lexer_push(lexer, &token);
			if (err)
				return err;

			if (!*inner) {
				return drgn_error_create(DRGN_ERROR_SYNTAX,
							 "expected abstract declarator");
			}
			return NULL;
		}

		err = drgn_lexer_pop(lexer, &token);
		if (err)
			return err;
	}
}

static struct drgn_error *
c_parse_abstract_declarator(struct drgn_type_index *tindex,
			    struct drgn_lexer *lexer,
			    struct c_declarator **outer,
			    struct c_declarator **inner)
{
	struct drgn_error *err;
	struct drgn_token token;

	err = drgn_lexer_peek(lexer, &token);
	if (err)
		return err;
	if (token.kind == C_TOKEN_ASTERISK) {
		err = c_parse_pointer(tindex, lexer, outer, inner);
		if (err)
			return err;

		err = drgn_lexer_peek(lexer, &token);
		if (token.kind == C_TOKEN_LPAREN ||
		    token.kind == C_TOKEN_LBRACKET) {
			struct c_declarator *tmp;

			err = c_parse_direct_abstract_declarator(tindex, lexer,
								 outer, &tmp);
			if (err)
				return err;
		}
		return NULL;
	} else {
		return c_parse_direct_abstract_declarator(tindex, lexer, outer,
							  inner);
	}
}

/* This always frees the declarator list regardless of success or failure. */
static struct drgn_error *
c_type_from_declarator(struct drgn_type_index *tindex,
		       struct c_declarator *declarator,
		       struct drgn_qualified_type *ret)
{
	struct drgn_error *err;

	if (!declarator)
		return NULL;

	err = c_type_from_declarator(tindex, declarator->next, ret);
	if (err) {
		free(declarator);
		return err;
	}

	if (declarator->kind == C_TOKEN_ASTERISK) {
		err = drgn_type_index_pointer_type(tindex, tindex->word_size,
						   *ret, &ret->type);
	} else if (declarator->is_complete) {
		err = drgn_type_index_array_type(tindex, declarator->length,
						 *ret, &ret->type);
	} else {
		err = drgn_type_index_incomplete_array_type(tindex, *ret,
							    &ret->type);
	}

	if (!err)
		ret->qualifiers = declarator->qualifiers;
	free(declarator);
	return err;
}

static struct drgn_error *c_find_type(struct drgn_type_index *tindex,
				      const char *name, const char *filename,
				      struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	struct drgn_lexer lexer;
	struct drgn_token token;

	drgn_lexer_init(&lexer, drgn_lexer_c, name);

	err = c_parse_specifier_qualifier_list(tindex, &lexer, filename, ret);
	if (err)
		goto out;

	err = drgn_lexer_pop(&lexer, &token);
	if (err)
		goto out;
	if (token.kind != C_TOKEN_EOF) {
		struct c_declarator *outer = NULL, *inner;

		err = drgn_lexer_push(&lexer, &token);
		if (err)
			return err;

		err = c_parse_abstract_declarator(tindex, &lexer, &outer,
						  &inner);
		if (err) {
			while (outer) {
				struct c_declarator *next;

				next = outer->next;
				free(outer);
				outer = next;
			}
			goto out;
		}

		err = c_type_from_declarator(tindex, outer, ret);
		if (err)
			goto out;

		err = drgn_lexer_pop(&lexer, &token);
		if (err)
			goto out;
		if (token.kind != C_TOKEN_EOF) {
			err = drgn_error_create(DRGN_ERROR_SYNTAX,
						"extra tokens after type name");
			goto out;
		}
	}

	err = NULL;
out:
	drgn_lexer_deinit(&lexer);
	return err;
}

static struct drgn_error *c_bit_offset(struct drgn_program *prog,
				       struct drgn_type *type,
				       const char *member_designator,
				       uint64_t *ret)
{
	struct drgn_error *err;
	struct drgn_lexer lexer;
	int state = INT_MIN;
	uint64_t bit_offset = 0;

	drgn_lexer_init(&lexer, drgn_lexer_c, member_designator);

	for (;;) {
		struct drgn_token token;

		err = drgn_lexer_pop(&lexer, &token);
		if (err)
			goto out;

		switch (state) {
		case INT_MIN:
		case C_TOKEN_DOT:
			if (token.kind == C_TOKEN_IDENTIFIER) {
				struct drgn_member_value *member;
				struct drgn_qualified_type member_type;

				err = drgn_program_find_member(prog, type,
							       token.value,
							       token.len,
							       &member);
				if (err)
					goto out;
				if (__builtin_add_overflow(bit_offset,
							   member->bit_offset,
							   &bit_offset)) {
					err = drgn_error_create(DRGN_ERROR_OVERFLOW,
								"offset is too large");
					goto out;
				}
				err = drgn_lazy_type_evaluate(member->type,
							      &member_type);
				if (err)
					goto out;
				type = member_type.type;
			} else if (state == C_TOKEN_DOT) {
				err = drgn_error_create(DRGN_ERROR_SYNTAX,
							"expected identifier after '.'");
				goto out;
			} else {
				err = drgn_error_create(DRGN_ERROR_SYNTAX,
							"expected identifier");
				goto out;
			}
			break;
		case C_TOKEN_IDENTIFIER:
		case C_TOKEN_RBRACKET:
			switch (token.kind) {
			case C_TOKEN_EOF:
				*ret = bit_offset;
				err = NULL;
				goto out;
			case C_TOKEN_DOT:
			case C_TOKEN_LBRACKET:
				break;
			default:
				if (state == C_TOKEN_IDENTIFIER) {
					err = drgn_error_create(DRGN_ERROR_SYNTAX,
								"expected '.' or '[' after identifier");
					goto out;
				} else {
					err = drgn_error_create(DRGN_ERROR_SYNTAX,
								"expected '.' or '[' after ']'");
					goto out;
				}
			}
			break;
		case C_TOKEN_LBRACKET:
			if (token.kind == C_TOKEN_NUMBER) {
				struct drgn_type *underlying_type;
				struct drgn_type *element_type;
				uint64_t index, bit_size, element_offset;

				err = c_token_to_u64(&token, &index);
				if (err)
					goto out;

				underlying_type = drgn_underlying_type(type);
				if (drgn_type_kind(underlying_type) != DRGN_TYPE_ARRAY) {
					err = drgn_type_error("'%s' is not an array",
							      type);
					goto out;
				}
				element_type =
					drgn_type_type(underlying_type).type;
				err = drgn_type_bit_size(element_type,
							 &bit_size);
				if (err)
					goto out;
				if (__builtin_mul_overflow(index, bit_size,
							   &element_offset) ||
				    __builtin_add_overflow(bit_offset,
							   element_offset,
							   &bit_offset)) {
					err = drgn_error_create(DRGN_ERROR_OVERFLOW,
								"offset is too large");
					goto out;
				}
				type = element_type;
			} else {
				err = drgn_error_create(DRGN_ERROR_SYNTAX,
							"expected number after '['");
				goto out;
			}
			break;
		case C_TOKEN_NUMBER:
			if (token.kind != C_TOKEN_RBRACKET) {
				err = drgn_error_create(DRGN_ERROR_SYNTAX,
							"expected ']' after number");
				goto out;
			}
			break;
		default:
			DRGN_UNREACHABLE();
		}
		state = token.kind;
	}

out:
	drgn_lexer_deinit(&lexer);
	return err;
}

static struct drgn_error *c_integer_literal(struct drgn_object *res,
					    uint64_t uvalue)
{
	struct drgn_type **types = res->prog->tindex->primitive_types;
	struct drgn_qualified_type qualified_type;
	unsigned int bits;

	_Static_assert(sizeof(unsigned long long) == 8,
		       "unsigned long long is not 64 bits");
	bits = uvalue ? 64 - __builtin_clzll(uvalue) : 0;

	qualified_type.qualifiers = 0;
	if (bits < 8 * drgn_type_size(types[DRGN_C_TYPE_INT])) {
		qualified_type.type = types[DRGN_C_TYPE_INT];
	} else if (bits < 8 * drgn_type_size(types[DRGN_C_TYPE_LONG])) {
		qualified_type.type = types[DRGN_C_TYPE_LONG];
	} else if (bits < 8 * drgn_type_size(types[DRGN_C_TYPE_LONG_LONG])) {
		qualified_type.type = types[DRGN_C_TYPE_LONG_LONG];
	} else if (bits <=
		   8 * drgn_type_size(types[DRGN_C_TYPE_UNSIGNED_LONG_LONG])) {
		qualified_type.type = types[DRGN_C_TYPE_UNSIGNED_LONG_LONG];
		return drgn_object_set_unsigned(res, qualified_type, uvalue, 0);
	} else {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "integer literal is too large");
	}
	return drgn_object_set_signed(res, qualified_type, uvalue, 0);
}

static struct drgn_error *c_bool_literal(struct drgn_object *res, bool bvalue)
{
	struct drgn_qualified_type qualified_type = {
		res->prog->tindex->primitive_types[DRGN_C_TYPE_INT],
	};

	return drgn_object_set_signed(res, qualified_type, bvalue, 0);
}

static struct drgn_error *c_float_literal(struct drgn_object *res,
					  double fvalue)
{
	struct drgn_qualified_type qualified_type = {
		res->prog->tindex->primitive_types[DRGN_C_TYPE_DOUBLE],
	};

	return drgn_object_set_float(res, qualified_type, fvalue);
}

static const int c_integer_conversion_rank[] = {
	[DRGN_C_TYPE_BOOL] = 0,
	[DRGN_C_TYPE_CHAR] = 1,
	[DRGN_C_TYPE_SIGNED_CHAR] = 1,
	[DRGN_C_TYPE_UNSIGNED_CHAR] = 1,
	[DRGN_C_TYPE_SHORT] = 2,
	[DRGN_C_TYPE_UNSIGNED_SHORT] = 2,
	[DRGN_C_TYPE_INT] = 3,
	[DRGN_C_TYPE_UNSIGNED_INT] = 3,
	[DRGN_C_TYPE_LONG] = 4,
	[DRGN_C_TYPE_UNSIGNED_LONG] = 4,
	[DRGN_C_TYPE_LONG_LONG] = 5,
	[DRGN_C_TYPE_UNSIGNED_LONG_LONG] = 5,
};

static bool c_can_represent_all_values(struct drgn_type *type1,
				       uint64_t bit_field_size1,
				       struct drgn_type *type2,
				       uint64_t bit_field_size2)
{
	uint64_t width1, width2;
	bool is_signed1, is_signed2;

	if (drgn_type_kind(type1) == DRGN_TYPE_BOOL) {
		width1 = 1;
		is_signed1 = false;
	} else {
		width1 = (bit_field_size1 ? bit_field_size1 :
			  8 * drgn_type_size(type1));
		is_signed1 = drgn_type_is_signed(type1);
	}
	if (drgn_type_kind(type2) == DRGN_TYPE_BOOL) {
		width2 = 1;
		is_signed2 = false;
	} else {
		width2 = (bit_field_size2 ? bit_field_size2 :
			  8 * drgn_type_size(type2));
		is_signed2 = drgn_type_is_signed(type2);
	}

	if (is_signed1 == is_signed2)
		return width1 >= width2;
	else if (is_signed1 && !is_signed2)
		return width1 > width2;
	else
		return false;
}

static struct drgn_error *c_integer_promotions(struct drgn_type_index *tindex,
					       struct drgn_object_type *type)
{
	enum drgn_primitive_type primitive;

	switch (drgn_type_kind(type->underlying_type)) {
	case DRGN_TYPE_ENUM:
		/* Convert the enum to its compatible type. */
		type->type = type->underlying_type =
			drgn_type_type(type->underlying_type).type;
		if (!type->type) {
			return drgn_error_format(DRGN_ERROR_INVALID_ARGUMENT,
						 "operand cannot have incomplete enum type");
		}
		break;
	case DRGN_TYPE_INT:
	case DRGN_TYPE_BOOL:
		break;
	default:
		return NULL;
	}

	primitive = drgn_type_primitive(type->underlying_type);
	/*
	 * Integer promotions are performed on types whose integer conversion
	 * rank is less than or equal to the rank of int and unsigned int.
	 *
	 * If this isn't a standard integer type, then we don't know the rank,
	 * so we may need to promote it. According to the C standard, "the rank
	 * of a signed integer type shall be greater than the rank of any signed
	 * integer type with less precision", and "the rank of any standard
	 * integer type shall be greater than the rank of any extended integer
	 * type with the same width". If an extended signed integer type has
	 * less precision than int, or the same width as int, then all of its
	 * values can be represented by int (and likewise for an extended
	 * unsigned integer type and unsigned int). Therefore, an extended
	 * integer type should be promoted iff all of its values can be
	 * represented by int or unsigned int.
	 *
	 * Integer promotions are also performed on bit fields. The C standard
	 * only requires that bit fields of type _Bool, int, or unsigned int are
	 * supported, so it does not define how integer promotions should affect
	 * a bit field which cannot be represented by int or unsigned int. Clang
	 * promotes it to the full width, but GCC does not. We implement the GCC
	 * behavior of preserving the width.
	 */
	if (primitive == DRGN_NOT_PRIMITIVE_TYPE || type->bit_field_size) {
		if (c_can_represent_all_values(tindex->primitive_types[DRGN_C_TYPE_INT],
					       0, type->underlying_type,
					       type->bit_field_size)) {
			type->type = type->underlying_type =
				tindex->primitive_types[DRGN_C_TYPE_INT];
			type->bit_field_size = 0;
		} else if (c_can_represent_all_values(tindex->primitive_types[DRGN_C_TYPE_UNSIGNED_INT],
						      0, type->underlying_type,
						      type->bit_field_size)) {
			type->type = type->underlying_type =
				tindex->primitive_types[DRGN_C_TYPE_UNSIGNED_INT];
			type->bit_field_size = 0;
		}
		return NULL;
	}

	if (primitive == DRGN_C_TYPE_INT ||
	    primitive == DRGN_C_TYPE_UNSIGNED_INT ||
	    c_integer_conversion_rank[primitive] >
	    c_integer_conversion_rank[DRGN_C_TYPE_INT])
		return NULL;

	/*
	 * If int can represent all values of the original type, then the result
	 * is int. Otherwise, the result is unsigned int.
	 */
	if (c_can_represent_all_values(tindex->primitive_types[DRGN_C_TYPE_INT],
				       0, type->underlying_type, 0))
		type->type = tindex->primitive_types[DRGN_C_TYPE_INT];
	else
		type->type = tindex->primitive_types[DRGN_C_TYPE_UNSIGNED_INT];
	type->underlying_type = type->type;
	return NULL;
}

static struct drgn_error *c_common_real_type(struct drgn_type_index *tindex,
					     struct drgn_object_type *type1,
					     struct drgn_object_type *type2,
					     struct drgn_object_type *ret)
{
	struct drgn_error *err;
	enum drgn_primitive_type primitive1, primitive2;
	bool is_float1, is_float2;
	bool is_signed1, is_signed2;
	int rank_cmp;

	ret->qualifiers = 0;

	/*
	 * Strictly, the rules are:
	 *
	 * If either operand is long double, then the result is long double.
	 * Otherwise, if either operand is double, then the result is double.
	 * Otherwise, if either operand is float, then the result is float.
	 *
	 * However, we also have to handle other floating types not in the
	 * standard. Thus, the result is always the larger type, with ties
	 * broken in the order unknown > long double > double > float.
	 */
	is_float1 = drgn_type_kind(type1->underlying_type) == DRGN_TYPE_FLOAT;
	is_float2 = drgn_type_kind(type2->underlying_type) == DRGN_TYPE_FLOAT;
	if (is_float1 && is_float2) {
		uint64_t size1, size2;

		size1 = drgn_type_size(type1->underlying_type);
		size2 = drgn_type_size(type2->underlying_type);
		if (size1 > size2)
			goto ret1;
		else if (size2 > size1)
			goto ret2;
		else if (drgn_type_primitive(type1->underlying_type) >
			 drgn_type_primitive(type2->underlying_type))
			goto ret1;
		else
			goto ret2;
	} else if (is_float1) {
		goto ret1;
	} else if (is_float2) {
		goto ret2;
	}

	/*
	 * Otherwise, the integer promotions are performed before applying the
	 * following rules.
	 */
	err = c_integer_promotions(tindex, type1);
	if (err)
		return err;
	err = c_integer_promotions(tindex, type2);
	if (err)
		return err;

	is_signed1 = drgn_type_is_signed(type1->underlying_type);
	is_signed2 = drgn_type_is_signed(type2->underlying_type);

	/*
	 * The C standard only requires that bit fields of type _Bool, int, or
	 * unsigned int are supported, which are always promoted to int or
	 * unsigned int, so it does not define how to find the common real type
	 * when one or both of the operands are bit fields. GCC seems to use the
	 * wider operand, or the unsigned operand if they have equal width. As
	 * usual, we pick type2 if the two types are equivalent.
	 */
	if (type1->bit_field_size || type2->bit_field_size) {
		uint64_t width1, width2;

		width1 = (type1->bit_field_size ? type1->bit_field_size :
			  8 * drgn_type_size(type1->type));
		width2 = (type2->bit_field_size ? type2->bit_field_size :
			  8 * drgn_type_size(type2->type));
		if (width1 < width2 ||
		    (width1 == width2 && (!is_signed2 || is_signed1)))
			goto ret2;
		else
			goto ret1;
	}

	primitive1 = drgn_type_primitive(type1->underlying_type);
	primitive2 = drgn_type_primitive(type2->underlying_type);

	if (primitive1 != DRGN_NOT_PRIMITIVE_TYPE &&
	    primitive2 != DRGN_NOT_PRIMITIVE_TYPE) {
		/*
		 * If both operands have the same type, then no further
		 * conversions are needed.
		 *
		 * We can return either type1 or type2 here; it only makes a
		 * difference for typedefs. Arbitrarily pick type2 because
		 * that's what GCC seems to do (Clang always throws away the
		 * typedef).
		 */
		if (primitive1 == primitive2)
			goto ret2;

		/* Ranks are small, so this won't overflow. */
		rank_cmp = (c_integer_conversion_rank[primitive1] -
			    c_integer_conversion_rank[primitive2]);
	} else {
		/*
		 * We don't know the rank of non-standard integer types.
		 * However, we can usually compare their ranks, because
		 * according to the C standard, "the rank of a signed integer
		 * type shall be greater than the rank of any signed integer
		 * type with less precision", "the rank of any unsigned integer
		 * type shall equal the rank of the corresponding signed integer
		 * type", and "the rank of any standard integer type shall be
		 * greater than the rank of any extended integer type with the
		 * same width". The only case where we can't is if both types
		 * are non-standard and have the same size; we treat them as
		 * having equal rank in this case.
		 */
		uint64_t size1, size2;

		size1 = drgn_type_size(type1->underlying_type);
		size2 = drgn_type_size(type2->underlying_type);
		if (size1 == size2 && primitive1 == DRGN_NOT_PRIMITIVE_TYPE &&
		    primitive2 == DRGN_NOT_PRIMITIVE_TYPE)
			rank_cmp = 0;
		else if ((size1 == size2 && primitive2 != DRGN_NOT_PRIMITIVE_TYPE) ||
			 size1 < size2)
			rank_cmp = -1;
		else
			rank_cmp = 1;
	}

	/*
	 * Otherwise, if both operands have signed integer types or both have
	 * unsigned integer types, then the result is the type of the operand
	 * with the greater rank.
	 */
	if (is_signed1 == is_signed2) {
		if (rank_cmp > 0)
			goto ret1;
		else
			goto ret2;
	}

        /*
	 * Otherwise, if the operand that has unsigned integer type has rank
	 * greater or equal to the rank of the type of the other operand, then
	 * the result is the unsigned integer type.
	 */
	if (!is_signed1 && rank_cmp >= 0)
		goto ret1;
	else if (!is_signed2 && rank_cmp <= 0)
		goto ret2;

	/*
	 * Otherwise, if the type of the operand with signed integer type can
	 * represent all of the values of the type of the operand with unsigned
	 * integer type, then the result is the signed integer type.
	 */
	if (is_signed1 && c_can_represent_all_values(type1->underlying_type, 0,
						     type2->underlying_type, 0))
		goto ret1;
	if (is_signed2 && c_can_represent_all_values(type2->underlying_type, 0,
						     type1->underlying_type, 0))
		goto ret2;

	/*
	 * Otherwise, the result is the unsigned integer type corresponding to
	 * the type of the operand with signed integer type.
	 *
	 * Note that this case is not reached for non-standard types: if the
	 * types have different signs and the signed integer type has greater
	 * rank, then it must have greater size and thus be able to represent
	 * all values of the unsigned integer type.
	 */
	if (is_signed1) {
		assert(primitive1 != DRGN_NOT_PRIMITIVE_TYPE);
		ret->type = tindex->primitive_types[primitive1 + 1];
	} else {
		assert(is_signed2);
		assert(primitive2 != DRGN_NOT_PRIMITIVE_TYPE);
		ret->type = tindex->primitive_types[primitive2 + 1];
	}
	ret->underlying_type = ret->type;
	ret->bit_field_size = 0;
	return NULL;

ret1:
	*ret = *type1;
	return NULL;
ret2:
	*ret = *type2;
	return NULL;
}

static struct drgn_error *c_operand_type(const struct drgn_object *obj,
					 struct drgn_object_type *type_ret,
					 bool *is_pointer_ret,
					 uint64_t *referenced_size_ret)
{
	struct drgn_error *err;

	*type_ret = drgn_object_type(obj);
	switch (drgn_type_kind(type_ret->underlying_type)) {
	case DRGN_TYPE_ARRAY:
		err = drgn_type_index_pointer_type(obj->prog->tindex,
						   drgn_program_word_size(obj->prog),
						   drgn_type_type(type_ret->underlying_type),
						   &type_ret->type);
		if (err)
			return err;
		type_ret->underlying_type = type_ret->type;
		break;
	case DRGN_TYPE_FUNCTION: {
		struct drgn_qualified_type function_type = {
			.type = type_ret->underlying_type,
			.qualifiers = type_ret->qualifiers,
		};

		err = drgn_type_index_pointer_type(obj->prog->tindex,
						   drgn_program_word_size(obj->prog),
						   function_type,
						   &type_ret->type);
		if (err)
			return err;
		type_ret->underlying_type = type_ret->type;
		break;
	}
	default:
		break;
	}
	type_ret->qualifiers = 0;

	if (is_pointer_ret) {
		struct drgn_type *type;

		type = type_ret->underlying_type;
		*is_pointer_ret = drgn_type_kind(type) == DRGN_TYPE_POINTER;
		if (*is_pointer_ret && referenced_size_ret) {
			struct drgn_type *referenced_type;

			referenced_type =
				drgn_underlying_type(drgn_type_type(type).type);
			if (drgn_type_kind(referenced_type) == DRGN_TYPE_VOID) {
				*referenced_size_ret = 1;
			} else {
				err = drgn_type_sizeof(referenced_type,
						       referenced_size_ret);
				if (err)
					return err;
			}
		}
	}
	return NULL;
}

static struct drgn_error *c_op_cast(struct drgn_object *res,
				    struct drgn_qualified_type qualified_type,
				    const struct drgn_object *obj)
{
	struct drgn_error *err;
	struct drgn_object_type type;

	err = c_operand_type(obj, &type, NULL, NULL);
	if (err)
		return err;
	return drgn_op_cast(res, qualified_type, obj, &type);
}

/*
 * It's too expensive to check that two pointer types are compatible, so we just
 * check that they refer to the same kind of type with equal size.
 */
static bool c_pointers_similar(const struct drgn_object_type *lhs_type,
			       const struct drgn_object_type *rhs_type,
			       uint64_t lhs_size, uint64_t rhs_size)
{
	struct drgn_type *lhs_referenced_type, *rhs_referenced_type;

	lhs_referenced_type = drgn_type_type(lhs_type->underlying_type).type;
	rhs_referenced_type = drgn_type_type(rhs_type->underlying_type).type;
	return (drgn_type_kind(lhs_referenced_type) ==
		drgn_type_kind(rhs_referenced_type) && lhs_size == rhs_size);
}

static struct drgn_error *c_op_bool(const struct drgn_object *obj, bool *ret)
{
	struct drgn_type *underlying_type;

	underlying_type = drgn_underlying_type(obj->type);
	if (drgn_type_kind(underlying_type) == DRGN_TYPE_ARRAY) {
		*ret = true;
		return NULL;
	}

	if (!drgn_type_is_scalar(underlying_type)) {
		return drgn_qualified_type_error("cannot convert '%s' to bool",
						 drgn_object_qualified_type(obj));
	}

	return drgn_object_truthiness(obj, ret);
}

static struct drgn_error *c_op_cmp(const struct drgn_object *lhs,
				   const struct drgn_object *rhs, int *ret)
{
	struct drgn_error *err;
	struct drgn_object_type lhs_type, rhs_type;
	bool lhs_pointer, rhs_pointer;

	err = c_operand_type(lhs, &lhs_type, &lhs_pointer, NULL);
	if (err)
		return err;
	err = c_operand_type(rhs, &rhs_type, &rhs_pointer, NULL);
	if (err)
		return err;

	if (lhs_pointer && rhs_pointer) {
		return drgn_op_cmp_pointers(lhs, rhs, ret);
	} else if (lhs_pointer || rhs_pointer) {
		goto type_error;
	} else {
		struct drgn_object_type type;

		if (!drgn_type_is_arithmetic(lhs_type.underlying_type) ||
		    !drgn_type_is_arithmetic(rhs_type.underlying_type))
			goto type_error;
		err = c_common_real_type(lhs->prog->tindex, &lhs_type,
					 &rhs_type, &type);
		if (err)
			return err;

		return drgn_op_cmp_impl(lhs, rhs, &type, ret);
	}

type_error:
	return drgn_error_binary_op("comparison", &lhs_type, &rhs_type);
}

static struct drgn_error *c_op_add(struct drgn_object *res,
				   const struct drgn_object *lhs,
				   const struct drgn_object *rhs)
{
	struct drgn_error *err;
	struct drgn_object_type lhs_type, rhs_type;
	bool lhs_pointer, rhs_pointer;
	uint64_t lhs_size, rhs_size;

	err = c_operand_type(lhs, &lhs_type, &lhs_pointer, &lhs_size);
	if (err)
		return err;
	err = c_operand_type(rhs, &rhs_type, &rhs_pointer, &rhs_size);
	if (err)
		return err;

	if (lhs_pointer) {
		if (!drgn_type_is_integer(rhs_type.underlying_type))
			goto type_error;
		return drgn_op_add_to_pointer(res, &lhs_type, lhs_size, false, lhs, rhs);
	} else if (rhs_pointer) {
		if (!drgn_type_is_integer(lhs_type.underlying_type))
			goto type_error;
		return drgn_op_add_to_pointer(res, &rhs_type, rhs_size, false, rhs, lhs);
	} else {
		struct drgn_object_type type;

		if (!drgn_type_is_arithmetic(lhs_type.underlying_type) ||
		    !drgn_type_is_arithmetic(rhs_type.underlying_type))
			goto type_error;
		err = c_common_real_type(lhs->prog->tindex, &lhs_type,
					 &rhs_type, &type);
		if (err)
			return err;

		return drgn_op_add_impl(res, &type, lhs, rhs);
	}

type_error:
	return drgn_error_binary_op("binary +", &lhs_type, &rhs_type);
}

static struct drgn_error *c_op_sub(struct drgn_object *res,
				   const struct drgn_object *lhs,
				   const struct drgn_object *rhs)
{
	struct drgn_error *err;
	struct drgn_object_type lhs_type, rhs_type;
	bool lhs_pointer, rhs_pointer;
	uint64_t lhs_size, rhs_size;

	err = c_operand_type(lhs, &lhs_type, &lhs_pointer, &lhs_size);
	if (err)
		return err;
	err = c_operand_type(rhs, &rhs_type, &rhs_pointer, &rhs_size);
	if (err)
		return err;

	if (lhs_pointer && rhs_pointer) {
		struct drgn_type *ptrdiff_type =
			lhs->prog->tindex->primitive_types[DRGN_C_TYPE_PTRDIFF_T];
		struct drgn_object_type type = {
			.type = ptrdiff_type,
			.underlying_type = ptrdiff_type,
		};

		if (!c_pointers_similar(&lhs_type, &rhs_type, lhs_size,
					rhs_size))
			goto type_error;
		return drgn_op_sub_pointers(res, &type, lhs_size, lhs, rhs);
	} else if (lhs_pointer) {
		if (!drgn_type_is_integer(rhs_type.underlying_type))
			goto type_error;
		return drgn_op_add_to_pointer(res, &lhs_type, lhs_size, true,
					      lhs, rhs);
	} else {
		struct drgn_object_type type;

		if (!drgn_type_is_arithmetic(lhs_type.underlying_type) ||
		    !drgn_type_is_arithmetic(rhs_type.underlying_type))
			goto type_error;
		err = c_common_real_type(lhs->prog->tindex, &lhs_type, &rhs_type,
					 &type);
		if (err)
			return err;

		return drgn_op_sub_impl(res, &type, lhs, rhs);
	}

type_error:
	return drgn_error_binary_op("binary -", &lhs_type, &rhs_type);
}

#define BINARY_OP(op_name, op, check)						\
static struct drgn_error *c_op_##op_name(struct drgn_object *res,		\
					 const struct drgn_object *lhs,		\
					 const struct drgn_object *rhs)		\
{										\
	struct drgn_error *err;							\
	struct drgn_object_type lhs_type, rhs_type, type;			\
										\
	err = c_operand_type(lhs, &lhs_type, NULL, NULL);			\
	if (err)								\
		return err;							\
	err = c_operand_type(rhs, &rhs_type, NULL, NULL);			\
	if (err)								\
		return err;							\
	if (!drgn_type_is_##check(lhs_type.underlying_type) ||			\
	    !drgn_type_is_##check(rhs_type.underlying_type))			\
		return drgn_error_binary_op("binary "#op, &lhs_type,		\
					    &rhs_type);				\
										\
	err = c_common_real_type(lhs->prog->tindex, &lhs_type, &rhs_type,	\
				 &type);					\
	if (err)								\
		return err;							\
										\
	return drgn_op_##op_name##_impl(res, &type, lhs, rhs);			\
}
BINARY_OP(mul, *, arithmetic)
BINARY_OP(div, /, arithmetic)
BINARY_OP(mod, %, integer)
BINARY_OP(and, &, integer)
BINARY_OP(or, |, integer)
BINARY_OP(xor, ^, integer)
#undef BINARY_OP

#define SHIFT_OP(op_name, op)							\
static struct drgn_error *c_op_##op_name(struct drgn_object *res,		\
					 const struct drgn_object *lhs,		\
					 const struct drgn_object *rhs)		\
{										\
	struct drgn_error *err;							\
	struct drgn_object_type lhs_type, rhs_type;				\
										\
	err = c_operand_type(lhs, &lhs_type, NULL, NULL);			\
	if (err)								\
		return err;							\
	err = c_operand_type(rhs, &rhs_type, NULL, NULL);			\
	if (err)								\
		return err;							\
	if (!drgn_type_is_integer(lhs_type.underlying_type) ||			\
	    !drgn_type_is_integer(rhs_type.underlying_type))			\
		return drgn_error_binary_op("binary " #op, &lhs_type,		\
					    &rhs_type);				\
										\
	err = c_integer_promotions(lhs->prog->tindex, &lhs_type);		\
	if (err)								\
		return err;							\
	err = c_integer_promotions(lhs->prog->tindex, &rhs_type);		\
	if (err)								\
		return err;							\
										\
	return drgn_op_##op_name##_impl(res, lhs, &lhs_type, rhs, &rhs_type);	\
}
SHIFT_OP(lshift, <<)
SHIFT_OP(rshift, >>)
#undef SHIFT_OP

#define UNARY_OP(op_name, op, check)					\
static struct drgn_error *c_op_##op_name(struct drgn_object *res,	\
					 const struct drgn_object *obj)	\
{									\
	struct drgn_error *err;						\
	struct drgn_object_type type;					\
									\
	err = c_operand_type(obj, &type, NULL, NULL);			\
	if (err)							\
		return err;						\
	if (!drgn_type_is_##check(type.underlying_type))		\
		return drgn_error_unary_op("unary " #op, &type);	\
									\
	err = c_integer_promotions(obj->prog->tindex, &type);		\
	if (err)							\
		return err;						\
									\
	return drgn_op_##op_name##_impl(res, &type, obj);		\
}
UNARY_OP(pos, +, arithmetic)
UNARY_OP(neg, -, arithmetic)
UNARY_OP(not, ~, integer)
#undef UNARY_OP

const struct drgn_language drgn_language_c = {
	.name = "C",
	.pretty_print_type = c_pretty_print_type,
	.pretty_print_type_name = c_pretty_print_type_name,
	.pretty_print_object = c_pretty_print_object,
	.find_type = c_find_type,
	.bit_offset = c_bit_offset,
	.integer_literal = c_integer_literal,
	.bool_literal = c_bool_literal,
	.float_literal = c_float_literal,
	.op_cast = c_op_cast,
	.op_bool = c_op_bool,
	.op_cmp = c_op_cmp,
	.op_add = c_op_add,
	.op_sub = c_op_sub,
	.op_mul = c_op_mul,
	.op_div = c_op_div,
	.op_mod = c_op_mod,
	.op_lshift = c_op_lshift,
	.op_rshift = c_op_rshift,
	.op_and = c_op_and,
	.op_or = c_op_or,
	.op_xor = c_op_xor,
	.op_pos = c_op_pos,
	.op_neg = c_op_neg,
	.op_not = c_op_not,
};
