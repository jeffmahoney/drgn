// Copyright 2018-2019 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

/**
 * @file
 *
 * libdrgn public interface.
 */

#ifndef DRGN_H
#define DRGN_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/**
 * @mainpage
 *
 * libdrgn implements the core of <a
 * href="https://github.com/osandov/drgn">drgn</a>, a debugger-as-a-library. It
 * implements the main drgn abstractions: @ref Programs, @ref Types, and @ref
 * Objects. See <a href="modules.html">Modules</a> for detailed documentation.
 */

/** Major version of drgn. */
#define DRGN_VERSION_MAJOR 0
/** Minor version of drgn. */
#define DRGN_VERSION_MINOR 0
/** Patch level of drgn. */
#define DRGN_VERSION_PATCH 1

/**
 * @defgroup ErrorHandling Error handling
 *
 * Error handling in libdrgn.
 *
 * Operations in libdrgn can fail for various reasons. libdrgn returns errors as
 * @ref drgn_error.
 *
 * @{
 */

/** Error code for a @ref drgn_error. */
enum drgn_error_code {
	/** Cannot allocate memory. */
	DRGN_ERROR_NO_MEMORY,
	/** Stop iteration. */
	DRGN_ERROR_STOP,
	/** Miscellaneous error. */
	DRGN_ERROR_OTHER,
	/** Invalid argument. */
	DRGN_ERROR_INVALID_ARGUMENT,
	/** Integer overflow. */
	DRGN_ERROR_OVERFLOW,
	/** Maximum recursion depth exceeded. */
	DRGN_ERROR_RECURSION,
	/** System call error. */
	DRGN_ERROR_OS,
	/** Invalid ELF file. */
	DRGN_ERROR_ELF_FORMAT,
	/** Invalid DWARF file. */
	DRGN_ERROR_DWARF_FORMAT,
	/** File does not have debug information. */
	DRGN_ERROR_MISSING_DEBUG,
	/** Syntax error while parsing. */
	DRGN_ERROR_SYNTAX,
	/** Entry not found. */
	DRGN_ERROR_LOOKUP,
	/** Bad memory access. */
	DRGN_ERROR_FAULT,
	/** Type error in expression. */
	DRGN_ERROR_TYPE,
	/** Division by zero. */
	DRGN_ERROR_ZERO_DIVISION,
	/** Number of defined error codes. */
	DRGN_NUM_ERROR_CODES,
} __attribute__((packed));

/**
 * libdrgn error.
 *
 * All functions in libdrgn that can return an error return this type.
 */
struct drgn_error {
	/** Error code. */
	enum drgn_error_code code;
	/**
	 * @private
	 *
	 * Whether this error needs to be passed to @ref drgn_error_destroy().
	 *
	 * This is @c true for the error codes returned from @ref
	 * drgn_error_create() and its related functions. Certain errors are
	 * statically allocated and do not need to be passed to @ref
	 * drgn_error_destroy() (but they can be).
	 */
	bool needs_destroy;
	/**
	 * If @c code is @c DRGN_ERROR_OS, then the error number returned from
	 * the system call.
	 */
	int errnum;
	/*
	 * If @c code is @c DRGN_ERROR_OS, then the path of the file which
	 * encountered the error if applicable. Otherwise, @c NULL.
	 */
	char *path;
	/** Human-readable error message. */
	char *message;
};

/**
 * Out of memory @ref drgn_error.
 *
 * This has a code of @ref DRGN_ERROR_NO_MEMORY. It can be returned if a memory
 * allocation fails in order to avoid doing another memory allocation. It does
 * not not need to be passed to @ref drgn_error_destroy() (but it can be).
 */
extern struct drgn_error drgn_enomem;

/**
 * Create a @ref drgn_error.
 *
 * @param[in] code Error code.
 * @param[in] message Human-readable error message. This string is copied.
 * @return A new error with the given code and message. If there is a failure to
 * allocate memory for the error or the message, @ref drgn_enomem is returned
 * instead.
 */
struct drgn_error *drgn_error_create(int code, const char *message)
	__attribute__((returns_nonnull));

/**
 * Create a @ref drgn_error from a printf-style format.
 *
 * @param[in] code Error code.
 * @param[in] format printf-style format string.
 * @param[in] ... Arguments for the format string.
 * @return A new error with the given code and formatted message. If there is a
 * failure to allocate memory for the error or the message, @ref drgn_enomem is
 * returned instead.
 */
struct drgn_error *drgn_error_format(int code, const char *format, ...)
	__attribute__((returns_nonnull,format(printf, 2, 3)));

/**
 * Create a @ref DRGN_ERROR_OS @ref drgn_error.
 *
 * @sa drgn_error_create().
 *
 * @param[in] errnum Error number (i.e., @c errno).
 * @param[in] path If not @c NULL, the path of the file which encountered the
 * error. This string is copied.
 */
struct drgn_error *drgn_error_create_os(int errnum, const char *path,
					const char *message)
	__attribute__((returns_nonnull));

/**
 * Write a @ref drgn_error to a @c stdio stream.
 *
 * For @ref DRGN_ERROR_OS errors, this concatenates @ref drgn_error::message,
 * @ref drgn_error::path, and @c strerror() of @ref drgn_error::errnum.
 * Otherwise, this just writes @c message.
 *
 * @param[in] file File to write to (usually @c stderr).
 * @param[in] err Error to write.
 */
void drgn_error_fwrite(FILE *file, struct drgn_error *err);

/**
 * Free a @ref drgn_error.
 *
 * This must be called on any error returned from libdrgn unless otherwise
 * noted.
 *
 * @param[in] err Error to destroy. If @c NULL, this is a no-op.
 */
void drgn_error_destroy(struct drgn_error *err);

/** @} */

struct drgn_type;
struct drgn_type_thunk;

/**
 * @ingroup Types
 *
 * Type qualifiers.
 *
 * Some languages, like C, have the notion of qualifiers which add properties to
 * a type. Qualifiers are represented as a bitmask; each qualifier is a bit.
 */
enum drgn_qualifiers {
	/** Constant type. */
	DRGN_QUALIFIER_CONST = (1 << 0),
	/** Volatile type. */
	DRGN_QUALIFIER_VOLATILE = (1 << 1),
	/** Restrict type. */
	DRGN_QUALIFIER_RESTRICT = (1 << 2),
	/** Atomic type. */
	DRGN_QUALIFIER_ATOMIC = (1 << 3),
	/** Bitmask of all valid qualifiers. */
	DRGN_ALL_QUALIFIERS = (1 << 4) - 1,
} __attribute__((packed));

/**
 * @ingroup LazyTypes
 *
 * Lazily-evaluated type.
 *
 * A lazy type may be in two states: unevaluated, in which case an arbitrary
 * callback must be called to evaluate the type, or evaluated, in which case the
 * type is cached. To evaluate a type, the thunk callback is called, the thunk
 * is freed, and the result is cached.
 *
 * This is for internal use only.
 */
struct drgn_lazy_type {
	union {
		/** Type if it has already been evaluated. */
		struct drgn_type *type;
		/** Thunk if the type has not been evaluated yet. */
		struct drgn_type_thunk *thunk;
	};
	/** Qualifiers, or -1 if the type has not been evaluated yet. */
	enum drgn_qualifiers qualifiers;
};

/**
 * @defgroup Types Types
 *
 * Type descriptors.
 *
 * Types in a program are represented by @ref drgn_type.
 *
 * Type descriptors have various fields depending on the kind of type. For each
 * field @c foo, there is a @c drgn_type_kind_has_foo() helper which returns
 * whether the given kind of type has the field @c foo; a @c drgn_type_has_foo()
 * helper which does the same but takes a type; and a @c drgn_type_foo() helper
 * which returns the field. For members, enumerators, and parameters, there is
 * also a @c drgn_type_num_foo() helper.
 *
 * @{
 */

/**
 * Kinds of types.
 *
 * Every type in a program supported by libdrgn falls into one of these
 * categories.
 */
enum drgn_type_kind {
	/** Void type. */
	DRGN_TYPE_VOID = 1,
	/** Integer type. */
	DRGN_TYPE_INT,
	/** Boolean type. */
	DRGN_TYPE_BOOL,
	/** Floating-point type. */
	DRGN_TYPE_FLOAT,
	/** Complex type. */
	DRGN_TYPE_COMPLEX,
	/** Structure type. */
	DRGN_TYPE_STRUCT,
	/** Union type. */
	DRGN_TYPE_UNION,
	/** Enumerated type. */
	DRGN_TYPE_ENUM,
	/** Type definition (a.k.a.\ alias) type. */
	DRGN_TYPE_TYPEDEF,
	/** Pointer type. */
	DRGN_TYPE_POINTER,
	/** Array type. */
	DRGN_TYPE_ARRAY,
	/** Function type. */
	DRGN_TYPE_FUNCTION,
} __attribute__((packed));

/** Primitive types known to drgn. */
enum drgn_primitive_type {
	/* Primitive C types. */
	DRGN_C_TYPE_VOID,
	DRGN_C_TYPE_CHAR,
	DRGN_C_TYPE_SIGNED_CHAR,
	DRGN_C_TYPE_UNSIGNED_CHAR,
	DRGN_C_TYPE_SHORT,
	DRGN_C_TYPE_UNSIGNED_SHORT,
	DRGN_C_TYPE_INT,
	DRGN_C_TYPE_UNSIGNED_INT,
	DRGN_C_TYPE_LONG,
	DRGN_C_TYPE_UNSIGNED_LONG,
	DRGN_C_TYPE_LONG_LONG,
	DRGN_C_TYPE_UNSIGNED_LONG_LONG,
	DRGN_C_TYPE_BOOL,
	DRGN_C_TYPE_FLOAT,
	DRGN_C_TYPE_DOUBLE,
	DRGN_C_TYPE_LONG_DOUBLE,
	DRGN_C_TYPE_SIZE_T,
	DRGN_C_TYPE_PTRDIFF_T,
	DRGN_PRIMITIVE_TYPE_NUM,
	DRGN_NOT_PRIMITIVE_TYPE = DRGN_PRIMITIVE_TYPE_NUM,
	/*
	 * Make sure to update api_reference.rst and type.c when adding anything
	 * here.
	 */
} __attribute__((packed));

/** Member of a structure or union type. */
struct drgn_type_member {
	/**
	 * Type of the member.
	 *
	 * Access this with @ref drgn_member_type().
	 */
	struct drgn_lazy_type type;
	/** Member name or @c NULL if it is unnamed. */
	const char *name;
	/**
	 * Offset in bits from the beginning of the type to the beginning of
	 * this member (i.e., for little-endian machines, the least significant
	 * bit, and for big-endian machines, the most significant bit). Members
	 * are usually aligned to at least a byte, so this is usually a multiple
	 * of 8 (but that may not be the case for bit fields).
	 */
	uint64_t bit_offset;
	/**
	 * If this member is a bit field, the size of the field in bits. If this
	 * member is not a bit field, 0.
	 */
	uint64_t bit_field_size;
};

/** Value of an enumerated type. */
struct drgn_type_enumerator {
	/** Enumerator name. */
	const char *name;
	union {
		/** Enumerator value if the type is signed. */
		int64_t svalue;
		/** Enumerator value if the type is unsigned. */
		uint64_t uvalue;
	};
};

/** Parameter of a function type. */
struct drgn_type_parameter {
	/**
	 * Type of the parameter.
	 *
	 * Access this with @ref drgn_parameter_type().
	 */
	struct drgn_lazy_type type;
	/** Parameter name or @c NULL if it is unnamed. */
	const char *name;
};

/**
 * Language-agnostic type descriptor.
 *
 * This structure should not be accessed directly; see @ref Types.
 */
struct drgn_type {
	/** @privatesection */
	struct {
		enum drgn_type_kind kind;
		bool is_complete;
		enum drgn_primitive_type primitive;
		/* These are the qualifiers for the wrapped type, not this type. */
		enum drgn_qualifiers qualifiers;
		/*
		 * This mess of unions is used to make this as compact as possible. Use
		 * the provided helpers and don't think about it.
		 */
		union {
			const char *name;
			const char *tag;
			size_t num_parameters;
		};
		union {
			uint64_t size;
			uint64_t length;
			size_t num_enumerators;
			bool is_variadic;
		};
		union {
			bool is_signed;
			size_t num_members;
			struct drgn_type *type;
		};
	} _private;
	/*
	 * An array of struct drgn_type_member, struct drgn_type_enumerator, or
	 * struct drgn_type_parameter may follow. We can't use flexible array
	 * members for these because they are not allowed in a union or nested
	 * structure; we can't use GCC's zero length array extension because
	 * that triggers false positives in Clang's AddressSanitizer. Instead,
	 * these are accessed internally with drgn_type_payload().
	 */
};

/**
 * Qualified type.
 *
 * A type with qualifiers.
 *
 * @sa drgn_qualifiers
 */
struct drgn_qualified_type {
	/** Unqualified type. */
	struct drgn_type *type;
	/** Bitmask of qualifiers on this type. */
	enum drgn_qualifiers qualifiers;
};

/** Get the kind of a type. */
static inline enum drgn_type_kind drgn_type_kind(struct drgn_type *type)
{
	return type->_private.kind;
}

/** Get the primitive type corresponding to a @ref drgn_type. */
static inline enum drgn_primitive_type
drgn_type_primitive(struct drgn_type *type)
{
	return type->_private.primitive;
}

/**
 * Get whether a type is complete (i.e., the type definition is known).
 *
 * This is always @c false for the void type. It may be @c false for structure,
 * union, enumerated, and array types, as well as typedef types where the
 * underlying type is one of those. Otherwise, it is always @c true.
 */
static inline bool drgn_type_is_complete(struct drgn_type *type)
{
	return type->_private.is_complete;
}

/**
 * Get whether a kind of type has a name. This is true for integer, boolean,
 * floating-point, complex, and typedef types.
 */
static inline bool drgn_type_kind_has_name(enum drgn_type_kind kind)
{
	return (kind == DRGN_TYPE_INT ||
		kind == DRGN_TYPE_BOOL ||
		kind == DRGN_TYPE_FLOAT ||
		kind == DRGN_TYPE_COMPLEX ||
		kind == DRGN_TYPE_TYPEDEF);
}
/** Get whether a type has a name. @sa drgn_type_kind_has_name() */
static inline bool drgn_type_has_name(struct drgn_type *type)
{
	return drgn_type_kind_has_name(drgn_type_kind(type));
}
/**
 * Get the name of a type. @ref drgn_type_has_name() must be true for this type.
 */
static inline const char *drgn_type_name(struct drgn_type *type)
{
	assert(drgn_type_has_name(type));
	return type->_private.name;
}

/**
 * Get whether a kind of type has a size. This is true for integer, boolean,
 * floating-point, complex, structure, union, and pointer types.
 */
static inline bool drgn_type_kind_has_size(enum drgn_type_kind kind)
{
	return (kind == DRGN_TYPE_INT ||
		kind == DRGN_TYPE_BOOL ||
		kind == DRGN_TYPE_FLOAT ||
		kind == DRGN_TYPE_COMPLEX ||
		kind == DRGN_TYPE_STRUCT ||
		kind == DRGN_TYPE_UNION ||
		kind == DRGN_TYPE_POINTER);
}
/** Get whether a type has a size. @sa drgn_type_kind_has_size() */
static inline bool drgn_type_has_size(struct drgn_type *type)
{
	return drgn_type_kind_has_size(drgn_type_kind(type));
}
/**
 * Get the size of a type in bytes. @ref drgn_type_has_size() must be true for
 * this type.
 */
static inline uint64_t drgn_type_size(struct drgn_type *type)
{
	assert(drgn_type_has_size(type));
	return type->_private.size;
}

/**
 * Get whether a kind of type has a signedness. This is true for integer types.
 */
static inline bool drgn_type_kind_has_is_signed(enum drgn_type_kind kind)
{
	return kind == DRGN_TYPE_INT;
}
/** Get whether a type has a signedness. @sa drgn_type_kind_has_is_signed() */
static inline bool drgn_type_has_is_signed(struct drgn_type *type)
{
	return drgn_type_kind_has_is_signed(drgn_type_kind(type));
}
/**
 * Get the signedness of a type. @ref drgn_type_has_is_signed() must be true for
 * this type.
 */
static inline bool drgn_type_is_signed(struct drgn_type *type)
{
	assert(drgn_type_has_is_signed(type));
	return type->_private.is_signed;
}

/**
 * Get whether a kind of type has a tag. This is true for structure, union, and
 * enumerated types.
 */
static inline bool drgn_type_kind_has_tag(enum drgn_type_kind kind)
{
	return (kind == DRGN_TYPE_STRUCT ||
		kind == DRGN_TYPE_UNION ||
		kind == DRGN_TYPE_ENUM);
}
/** Get whether a type has a tag. @sa drgn_type_kind_has_tag() */
static inline bool drgn_type_has_tag(struct drgn_type *type)
{
	return drgn_type_kind_has_tag(drgn_type_kind(type));
}
/**
 * Get the tag of a type. @ref drgn_type_has_tag() must be true for this type.
 */
static inline const char *drgn_type_tag(struct drgn_type *type)
{
	assert(drgn_type_has_tag(type));
	return type->_private.tag;
}

static inline void *drgn_type_payload(struct drgn_type *type)
{
	return (char *)type + sizeof(*type);
}

/**
 * Get whether a kind of type has members. This is true for structure and union
 * types.
 */
static inline bool drgn_type_kind_has_members(enum drgn_type_kind kind)
{
	return kind == DRGN_TYPE_STRUCT || kind == DRGN_TYPE_UNION;
}
/** Get whether a type has members. @sa drgn_type_kind_has_members() */
static inline bool drgn_type_has_members(struct drgn_type *type)
{
	return drgn_type_kind_has_members(drgn_type_kind(type));
}
/**
 * Get the members of a type. @ref drgn_type_has_members() must be true for this
 * type.
 */
static inline struct drgn_type_member *drgn_type_members(struct drgn_type *type)
{
	assert(drgn_type_has_members(type));
	return drgn_type_payload(type);
}
/**
 * Get the number of members of a type. @ref drgn_type_has_members() must be
 * true for this type. If the type is incomplete, this is always zero.
 */
static inline size_t drgn_type_num_members(struct drgn_type *type)
{
	assert(drgn_type_has_members(type));
	return type->_private.num_members;
}

/**
 * Get whether a kind of type has a wrapped type. This is true for complex,
 * enumerated, typedef, pointer, array, and function types.
 */
static inline bool drgn_type_kind_has_type(enum drgn_type_kind kind)
{
	return (kind == DRGN_TYPE_COMPLEX ||
		kind == DRGN_TYPE_ENUM ||
		kind == DRGN_TYPE_TYPEDEF ||
		kind == DRGN_TYPE_POINTER ||
		kind == DRGN_TYPE_ARRAY ||
		kind == DRGN_TYPE_FUNCTION);
}
/** Get whether a type has a wrapped type. @sa drgn_type_kind_has_type() */
static inline bool drgn_type_has_type(struct drgn_type *type)
{
	return drgn_type_kind_has_type(drgn_type_kind(type));
}
/**
 * Get the type wrapped by this type.
 *
 * For a complex type, this is the corresponding real type.
 *
 * For an enumerated type, this is the compatible integer type. It is @c NULL if
 * the enumerated type is incomplete.
 *
 * For a typedef type, this is the aliased type.
 *
 * For a pointer type, this is the referenced type.
 *
 * For an array type, this is the element type.
 *
 * For a function type, this is the return type.
 */
static inline struct drgn_qualified_type
drgn_type_type(struct drgn_type *type)
{
	assert(drgn_type_has_type(type));
	return (struct drgn_qualified_type){
		.type = type->_private.type,
		.qualifiers = type->_private.qualifiers,
	};
}

/**
 * Get whether a kind of type has enumerators. This is true for enumerated
 * types.
 */
static inline bool drgn_type_kind_has_enumerators(enum drgn_type_kind kind)
{
	return kind == DRGN_TYPE_ENUM;
}
/** Get whether a type has enumerators. @sa drgn_type_kind_has_enumerators() */
static inline bool drgn_type_has_enumerators(struct drgn_type *type)
{
	return drgn_type_kind_has_enumerators(drgn_type_kind(type));
}
/**
 * Get the enumerators of a type. @ref drgn_type_has_enumerators() must be true
 * for this type.
 */
static inline struct drgn_type_enumerator *
drgn_type_enumerators(struct drgn_type *type)
{
	assert(drgn_type_has_enumerators(type));
	return drgn_type_payload(type);
}
/**
 * Get the number of enumerators of a type. @ref drgn_type_has_enumerators()
 * must be true for this type. If the type is incomplete, this is always zero.
 */
static inline size_t drgn_type_num_enumerators(struct drgn_type *type)
{
	assert(drgn_type_has_enumerators(type));
	return type->_private.num_enumerators;
}

/** Get whether a kind of type has a length. This is true for array types. */
static inline bool drgn_type_kind_has_length(enum drgn_type_kind kind)
{
	return kind == DRGN_TYPE_ARRAY;
}
/** Get whether a type has a length. @sa drgn_type_kind_has_length() */
static inline bool drgn_type_has_length(struct drgn_type *type)
{
	return drgn_type_kind_has_length(drgn_type_kind(type));
}
/**
 * Get the length of a type. @ref drgn_type_has_length() must be true for this
 * type. If the type is incomplete, this is always zero.
 */
static inline uint64_t drgn_type_length(struct drgn_type *type)
{
	assert(drgn_type_has_length(type));
	return type->_private.length;
}

/**
 * Get whether a kind of type has parameters. This is true for function types.
 */
static inline bool drgn_type_kind_has_parameters(enum drgn_type_kind kind)
{
	return kind == DRGN_TYPE_FUNCTION;
}
/** Get whether a type has parameters. @sa drgn_type_kind_has_parameters() */
static inline bool drgn_type_has_parameters(struct drgn_type *type)
{
	return drgn_type_kind_has_parameters(drgn_type_kind(type));
}
/**
 * Get the parameters of a type. @ref drgn_type_has_parameters() must be true
 * for this type.
 */
static inline struct drgn_type_parameter *drgn_type_parameters(struct drgn_type *type)
{
	assert(drgn_type_has_parameters(type));
	return drgn_type_payload(type);
}
/**
 * Get the number of parameters of a type. @ref drgn_type_has_parameters() must
 * be true for this type.
 */
static inline size_t drgn_type_num_parameters(struct drgn_type *type)
{
	assert(drgn_type_has_parameters(type));
	return type->_private.num_parameters;
}

/**
 * Get whether a kind of type can be variadic. This is true for function types.
 */
static inline bool drgn_type_kind_has_is_variadic(enum drgn_type_kind kind)
{
	return kind == DRGN_TYPE_FUNCTION;
}
/** Get whether a type can be variadic. @sa drgn_type_kind_has_is_variadic() */
static inline bool drgn_type_has_is_variadic(struct drgn_type *type)
{
	return drgn_type_kind_has_is_variadic(drgn_type_kind(type));
}
/**
 * Get whether a type is variadic. @ref drgn_type_has_is_variadic() must be true
 * for this type.
 */
static inline bool drgn_type_is_variadic(struct drgn_type *type)
{
	assert(drgn_type_has_is_variadic(type));
	return type->_private.is_variadic;
}

/**
 * Evaluate the type of a @ref drgn_type_member.
 *
 * @param[in] member Member.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_member_type(struct drgn_type_member *member,
				    struct drgn_qualified_type *ret);

/**
 * Evaluate the type of a @ref drgn_type_parameter.
 *
 * @param[in] parameter Parameter.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_parameter_type(struct drgn_type_parameter *parameter,
				       struct drgn_qualified_type *ret);

/**
 * Get the size of a type in bytes.
 *
 * Unlike @ref drgn_type_size(), this is applicable to any type which has a
 * meaningful size, including typedefs and arrays. Void, function, and
 * incomplete types do not have a size; an error is returned for those types.
 *
 * @param[in] type Type.
 * @param[out] ret Returned size.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_type_sizeof(struct drgn_type *type,
				    uint64_t *ret);

/**
 * Compare two @ref drgn_type%s for equality.
 *
 * Two types are equal if all of their fields are equal, recursively.
 *
 * @param[in] a First type.
 * @param[in] b First type.
 * @param[out] ret @c true if the types are equal, @c false if they are not.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_type_eq(struct drgn_type *a, struct drgn_type *b,
				bool *ret);

/**
 * Compare two @ref drgn_qualified_type%s for equality.
 *
 * Two qualified types are equal if their unqualified types are equal and their
 * qualifiers are equal.
 *
 * @param[in] a First qualified type.
 * @param[in] b First qualified type.
 * @param[out] ret @c true if the qualified types are equal, @c false if they
 * are not.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_qualified_type_eq(struct drgn_qualified_type a,
					  struct drgn_qualified_type b,
					  bool *ret);

/**
 * Pretty-print the name of a type.
 *
 * This will format the name of the type as it would be referred to in its
 * programming language.
 *
 * @param[in] qualified_type Type to format.
 * @param[out] ret Returned string. On success, it must be freed with @c free().
 * On error, its contents are undefined.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_pretty_print_type_name(struct drgn_qualified_type qualified_type,
			    char **ret);

/**
 * Pretty-print the definition of a type.
 *
 * This will format the type as it would be defined in its programming language.
 *
 * @param[in] qualified_type Type to format.
 * @param[out] ret Returned string. On success, it must be freed with @c free().
 * On error, its contents are undefined.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_pretty_print_type(struct drgn_qualified_type qualified_type, char **ret);

/** @} */

struct drgn_object;

/**
 * @defgroup Programs Programs
 *
 * Debugging programs.
 *
 * A program being debugged is represented by a @ref drgn_program.
 *
 * @{
 */

/**
 * @struct drgn_program
 *
 * Program being debugged.
 *
 * A @ref drgn_program represents a crashed or running program. It supports
 * looking up objects (@ref drgn_program_find_object()) and types (@ref
 * drgn_program_find_type()) by name and reading arbitrary memory from the
 * program (@ref drgn_program_read_memory()).
 *
 * A @ref drgn_program is created with @ref drgn_program_from_core_dump(), @ref
 * drgn_program_from_kernel(), or @ref drgn_program_from_pid(). It must be freed
 * with @ref drgn_program_destroy().
 */
struct drgn_program;

/** Flags which apply tho a @ref drgn_program. */
enum drgn_program_flags {
	/** The program is the Linux kernel. */
	DRGN_PROGRAM_IS_LINUX_KERNEL = (1 << 0),
};

/**
 * Create a @ref drgn_program from a core dump file.
 *
 * The type of program (e.g., userspace or kernel) is determined automatically.
 *
 * @param[in] path Core dump file path.
 * @param[in] verbose Whether to print non-fatal errors to stderr.
 * @param[out] ret Returned program.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_from_core_dump(const char *path, bool verbose,
					       struct drgn_program **ret);

/**
 * Create a @ref drgn_program from the running operating system kernel.
 *
 * This requires root privileges.
 *
 * @param[in] verbose Whether to print non-fatal errors to stderr.
 * @param[out] ret Returned program.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_from_kernel(bool verbose,
					    struct drgn_program **ret);

/**
 * Create a @ref drgn_program from the a running program.
 *
 * On Linux, this requires @c PTRACE_MODE_ATTACH_FSCREDS permissions (see
 * <tt>ptrace(2)</tt>).
 *
 * @param[in] pid Process ID of the program to debug.
 * @param[out] ret Returned program.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_from_pid(pid_t pid, struct drgn_program **ret);

/**
 * Free a @ref drgn_program.
 *
 * @param[in] prog Program to free.
 */
void drgn_program_destroy(struct drgn_program *prog);

/** Get the set of @ref drgn_program_flags applying to a @ref drgn_program. */
enum drgn_program_flags drgn_program_flags(struct drgn_program *prog);

/**
 * Get the size of a word in the given program.
 *
 * @return The word size in bytes (either 4 or 8).
 */
uint8_t drgn_program_word_size(struct drgn_program *prog);

/**
 * Get whether a program is little-endian.
 *
 * @return @c true if the program is little-endian, @c false if it is
 * big-endian.
 */
bool drgn_program_is_little_endian(struct drgn_program *prog);

/**
 * Read from a program's memory.
 *
 * @param[in] prog Program to read from.
 * @param[out] buf Buffer to read into.
 * @param[in] address Starting address in memory to read.
 * @param[in] count Number of bytes to read.
 * @param[in] physical Whether @c address is physical. A program may support
 * only virtual or physical addresses or both.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_read_memory(struct drgn_program *prog,
					    void *buf, uint64_t address,
					    size_t count, bool physical);

/**
 * Read a C string from a program's memory.
 *
 * This reads up to and including the terminating null byte.
 *
 * @param[in] prog Program to read from.
 * @param[in] address Starting address in memory to read.
 * @param[in] physical Whether @c address is physical. See @ref
 * drgn_program_read_memory().
 * @param[in] max_size Stop after this many bytes are read, not including the
 * null byte. A null byte is appended to @p ret in this case.
 * @param[out] ret Returned string. On success, it must be freed with @c free().
 * On error, its contents are undefined.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_read_c_string(struct drgn_program *prog,
					      uint64_t address, bool physical,
					      size_t max_size, char **ret);

/**
 * Find a type in a program by name.
 *
 * The returned type is valid for the lifetime of the @ref drgn_program.
 *
 * @param[in] prog Program.
 * @param[in] name Name of the type.
 * @param[in] filename Filename containing the type definition. This is matched
 * from right to left, so a type defined in <tt>/usr/include/stdio.h</tt> will
 * match <tt>stdio.h</tt>, <tt>include/stdio.h</tt>,
 * <tt>usr/include/stdio.h</tt>, and <tt>/usr/include/stdio.h</tt>. An empty or
 * @c NULL @p filename matches any definition. If multiple definitions match,
 * one is returned arbitrarily.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_find_type(struct drgn_program *prog,
					  const char *name,
					  const char *filename,
					  struct drgn_qualified_type *ret);

/** Flags for @ref drgn_program_find_object(). */
enum drgn_find_object_flags {
	/** Find a constant (e.g., enumeration constant or macro). */
	DRGN_FIND_OBJECT_CONSTANT = 1 << 0,
	/** Find a function. */
	DRGN_FIND_OBJECT_FUNCTION = 1 << 1,
	/** Find a variable. */
	DRGN_FIND_OBJECT_VARIABLE = 1 << 2,
	/** Find any kind of object. */
	DRGN_FIND_OBJECT_ANY = (1 << 3) - 1,
};

/**
 * Find an object in a program by name.
 *
 * The object can be a variable, constant, or function depending on @p flags.
 *
 * @param[in] prog Program.
 * @param[in] name Name of the object.
 * @param[in] filename Filename containing the object definition. This is
 * interpreted the same way as for @ref drgn_program_find_type().
 * @param[in] flags Flags indicating what kind of object to look for.
 * @param[out] ret Returned object. It must have already been initialized with
 * @ref drgn_object_init().
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_find_object(struct drgn_program *prog,
					    const char *name,
					    const char *filename,
					    enum drgn_find_object_flags flags,
					    struct drgn_object *ret);

/** Element type and size. */
struct drgn_element_info {
	/** Type of the element. */
	struct drgn_qualified_type qualified_type;
	/**
	 * Size in bits of one element.
	 *
	 * Element @c i is at bit offset <tt>i * bit_size</tt>.
	 */
	uint64_t bit_size;
};

/**
 * Get the element type and size of an array or pointer @ref drgn_type.
 *
 * @param[in] prog Program.
 * @param[in] type Array or pointer. After this function is called, this type
 * must remain valid until the program is destroyed.
 * @param[out] ret Returned element information.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_element_info(struct drgn_program *prog,
					     struct drgn_type *type,
					     struct drgn_element_info *ret);

/**
 * Type, offset, and bit field size of an object member.
 *
 * @sa drgn_type_member
 */
struct drgn_member_info {
	/** Type of the member. */
	struct drgn_qualified_type qualified_type;
	/**
	 * Offset in bits from the beginning of the type to the beginning of the
	 * member.
	 *
	 * If the member was found inside an unnamed member of the enclosing
	 * type, this is the offset from the beginning of the type passed to
	 * @ref drgn_program_member_info().
	 *
	 * See @ref drgn_type_member::bit_offset.
	 */
	uint64_t bit_offset;
	/** See @ref drgn_type_member::bit_field_size. */
	uint64_t bit_field_size;
};

/**
 * Get the type, offset, and bit field size of a member of a @ref drgn_type by
 * name.
 *
 * If the type has any unnamed members, this also matches members of those
 * unnamed members, recursively.
 *
 * @param[in] prog Program.
 * @param[in] type Structure or union type. After this function is called, this
 * type must remain valid until the program is destroyed.
 * @param[in] member_name Name of member.
 * @param[out] ret Returned member information.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_program_member_info(struct drgn_program *prog,
					    struct drgn_type *type,
					    const char *member_name,
					    struct drgn_member_info *ret);

/** @} */

/**
 * @defgroup Objects Objects
 *
 * Objects in a program.
 *
 * A @ref drgn_object represents an object (e.g., variable, constant, or
 * function) in a program.
 *
 * Various operators and helpers are defined on objects; see @ref
 * ObjectOperators and @ref ObjectHelpers.
 *
 * Many operations are language-specific. C is currently the only supported
 * language.
 *
 * In drgn's emulation of C:
 *
 * - Signed and unsigned integer arithmetic is reduced modulo 2^width.
 * - Integer division truncates towards zero.
 * - Modulo has the sign of the dividend.
 * - Division or modulo by 0 returns an error.
 * - Shifts are reduced modulo 2^width. In particular, a shift by a value
 *   greater than the width returns 0.
 * - Shifts by a negative number return an error.
 * - Bitwise operators on signed integers act on the two's complement
 *   representation.
 * - Pointer arithmetic is supported.
 * - Integer literal have the first type of @c int, @c long, <tt>long long</tt>,
 *   and <tt>unsigned long long</tt> which can represent the value.
 * - Boolean literals have type @c int (@b not @c _Bool).
 * - Floating-point literals have type @c double.
 * @{
 */

/**
 * Kinds of objects.
 *
 * The value of a @ref drgn_object falls into one of a handful of categories.
 * This kind determines which field of a @ref drgn_value is used.
 *
 * The incomplete kinds (@ref drgn_object_kind::DRGN_OBJECT_NONE, @ref
 * drgn_object_kind::DRGN_OBJECT_INCOMPLETE_BUFFER, and @ref
 * drgn_object_kind::DRGN_OBJECT_INCOMPLETE_INTEGER) are only possible for
 * reference objects; values have a complete type.
 */
enum drgn_object_kind {
	/**
	 * Memory buffer.
	 *
	 * This is used for objects with a complex, structure, union, or array
	 * type. The value is a buffer of the contents of that object's memory
	 * in the program.
	 */
	DRGN_OBJECT_BUFFER,
	/**
	 * Signed integer.
	 *
	 * This is used for objects with a signed integer or signed enumerated
	 * type.
	 */
	DRGN_OBJECT_SIGNED,
	/**
	 * Unsigned integer.
	 *
	 * This is used for objects with a unsigned integer, boolean, or pointer
	 * type.
	 */
	DRGN_OBJECT_UNSIGNED,
	/**
	 * Floating-point value.
	 *
	 * This used for objects with a floating-point type.
	 */
	DRGN_OBJECT_FLOAT,
	/**
	 * No value.
	 *
	 * This is used for reference objects with a void or function type.
	 */
	DRGN_OBJECT_NONE = -1,
	/**
	 * Incomplete buffer value.
	 *
	 * This is used for reference objects with an incomplete structure,
	 * union, or array type.
	 */
	DRGN_OBJECT_INCOMPLETE_BUFFER = -2,
	/**
	 * Incomplete integer value.
	 *
	 * This is used for reference objects with an incomplete enumerated
	 * types.
	 */
	DRGN_OBJECT_INCOMPLETE_INTEGER = -3,
} __attribute__((packed));

/**
 * Return whether a type corresponding to a kind of object is complete.
 *
 * @sa drgn_type_is_complete()
 */
static inline bool drgn_object_kind_is_complete(enum drgn_object_kind kind)
{
	return kind >= DRGN_OBJECT_BUFFER;
}

/** Byte-order specification. */
enum drgn_byte_order {
	/** Big-endian. */
	DRGN_BIG_ENDIAN,
	/** Little-endian. */
	DRGN_LITTLE_ENDIAN,
	/**
	 * Endianness of the program.
	 *
	 * @sa drgn_program_is_little_endian()
	 */
	DRGN_PROGRAM_ENDIAN,
};

/** Value of a @ref drgn_object. */
union drgn_value {
	/** @ref drgn_object_kind::DRGN_OBJECT_BUFFER value. */
	struct {
		/** Buffer itself. */
		union {
			/** Pointer to an external buffer. */
			char *bufp;
			/**
			 * Inline buffer.
			 *
			 * Tiny buffers (see @ref drgn_value_is_inline()) are
			 * stored inline here instead of in a separate
			 * allocation.
			 */
			char ibuf[8];
		};
		/**
		 * Offset of the value from the beginning of the buffer.
		 *
		 * This is always less than 8, but usually 0.
		 */
		uint8_t bit_offset;
		/** Whether the values within the buffer are little-endian. */
		bool little_endian;
	};
	/** @ref drgn_object_kind::DRGN_OBJECT_SIGNED value. */
	int64_t svalue;
	/** @ref drgn_object_kind::DRGN_OBJECT_UNSIGNED value. */
	uint64_t uvalue;
	/** @ref drgn_object_kind::DRGN_OBJECT_FLOAT value. */
	double fvalue;
};

/**
 * Return the number of bytes needed to store a given number of bits starting at
 * a given offset.
 *
 * This assumes that <tt>bit_size + bit_offset</tt> does not overflow a 64-bit
 * integer, which is guaranteed to be true for object values.
 *
 * @param[in] bit_size Size in bits of the value.
 * @param[in] bit_offset Offset of the value from the beginning of the buffer.
 */
static inline uint64_t drgn_value_size(uint64_t bit_size, uint64_t bit_offset)
{
	uint64_t bits = bit_size + bit_offset;

	return bits / 8 + (bits % 8 ? 1 : 0);
}

/**
 * Return whether a buffer value uses the inline buffer (@ref drgn_value::ibuf).
 *
 * This assumes that <tt>bit_size + bit_offset</tt> does not overflow a 64-bit
 * integer, which is guaranteed to be true for object values.
 *
 * @param[in] bit_size Size in bits of the value.
 * @param[in] bit_offset Offset of the value from the beginning of the buffer.
 */
static inline bool drgn_value_is_inline(uint64_t bit_size, uint64_t bit_offset)
{
	uint64_t bits = bit_size + bit_offset;

	return bits <= 8 * sizeof(((union drgn_value *)0)->ibuf);
}

/**
 * Object in a program.
 *
 * A @ref drgn_object represents a symbol or value in a program. It can be in
 * the memory of the program (a "reference") or a temporary computed value (a
 * "value").
 *
 * A @ref drgn_object must be initialized with @ref drgn_object_init() before it
 * is used. It can then be set and otherwise changed repeatedly. When the object
 * is no longer needed, it must be deinitialized @ref drgn_object_deinit().
 *
 * It is more effecient to initialize an object once and reuse it rather than
 * creating a new one repeatedly (e.g., in a loop).
 *
 * Members of a @ref drgn_object should not be modified except through the
 * provided functions.
 */
struct drgn_object {
	/** Program that this object belongs to. */
	struct drgn_program *prog;
	/** Type of this object. */
	struct drgn_type *type;
	/**
	 * Size of this object in bits.
	 *
	 * This is usually the size of @ref drgn_object::type, but it may be
	 * smaller if this is a bit field (@ref drgn_object::is_bit_field).
	 */
	uint64_t bit_size;
	/** Qualifiers on @ref drgn_object::type. */
	enum drgn_qualifiers qualifiers;
	/** Kind of this object. */
	enum drgn_object_kind kind;
	/** Whether this object is a reference. */
	bool is_reference;
	/** Whether this object is a bit field. */
	bool is_bit_field;
	/** Reference to this object in @ref drgn_object::prog, or its value. */
	union {
		/** Value. */
		union drgn_value value;
		/** Reference. */
		struct {
			/** Address in the program. */
			uint64_t address;
			/**
			 * Offset in bits from @c reference.
			 *
			 * This is always less than 8, but usually 0.
			 */
			uint8_t bit_offset;
			/** Whether the referenced object is little-endian. */
			bool little_endian;
		} reference;
	};
};

/**
 * Return whether an object's value uses the inline buffer (@ref
 * drgn_value::ibuf).
 */
static inline bool drgn_buffer_object_is_inline(const struct drgn_object *obj)
{
	return drgn_value_is_inline(obj->bit_size, obj->value.bit_offset);
}

/** Get the type of a @ref drgn_object. */
static inline struct drgn_qualified_type
drgn_object_qualified_type(const struct drgn_object *obj)
{
	return (struct drgn_qualified_type){
		.type = obj->type,
		.qualifiers = obj->qualifiers,
	};
}

/**
 * Initialize a @ref drgn_object.
 *
 * The object is initialized to a @c NULL reference with a void type. This must
 * be paired with a call to @ref drgn_object_deinit().
 *
 * @param[in] obj Object to initialize.
 * @param[in] prog Program containing the object.
 */
void drgn_object_init(struct drgn_object *obj, struct drgn_program *prog);

/**
 * Deinitialize a @ref drgn_object.
 *
 * The object cannot be used after this unless it is reinitialized with @ref
 * drgn_object_init().
 *
 * @param[in] obj Object to deinitialize.
 */
void drgn_object_deinit(struct drgn_object *obj);

/**
 * @defgroup ObjectSetters Setters
 *
 * Object setters.
 *
 * Once a @ref drgn_object is initialized with @ref drgn_object_init(), it may
 * be set any number of times.
 *
 * @{
 */

/**
 * Set a @ref drgn_object to a signed value.
 *
 * @param[out] res Object to set.
 * @param[in] qualified_type Type to set to.
 * @param[in] svalue Value to set to.
 * @param[in] bit_field_size If the object should be a bit field, its size in
 * bits. Otherwise, 0.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_object_set_signed(struct drgn_object *res,
		       struct drgn_qualified_type qualified_type,
		       int64_t svalue, uint64_t bit_field_size);

/**
 * Set a @ref drgn_object to an unsigned value.
 *
 * @param[out] res Object to set.
 * @param[in] qualified_type Type to set to.
 * @param[in] uvalue Value to set to.
 * @param[in] bit_field_size If the object should be a bit field, its size in
 * bits. Otherwise, 0.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_object_set_unsigned(struct drgn_object *res,
			 struct drgn_qualified_type qualified_type,
			 uint64_t uvalue, uint64_t bit_field_size);

/**
 * Set a @ref drgn_object to a floating-point value.
 *
 * @param[out] res Object to set.
 * @param[in] qualified_type Type to set to.
 * @param[in] fvalue Value to set to.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_object_set_float(struct drgn_object *res,
		      struct drgn_qualified_type qualified_type, double fvalue);

/**
 * Set a @ref drgn_object to a buffer value.
 *
 * @param[out] res Object to set.
 * @param[in] qualified_type Type to set to.
 * @param[in] buf Buffer to set to. It must be at least
 * <tt>bit_size + bit_offset</tt> bits large, where @c bit_size is @p
 * bit_field_size if non-zero and the size of @p qualified_type otherwise. It is
 * copied, so it need not remain valid after this function returns.
 * @param[in] bit_offset Offset of the value from the beginning of the buffer.
 * This must be less than 8 (and is usually 0).
 * @param[in] bit_field_size If the object should be a bit field, its size in
 * bits. Otherwise, 0.
 * @param[in] byte_order Byte order of the result.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_object_set_buffer(struct drgn_object *res,
		       struct drgn_qualified_type qualified_type,
		       const char *buf, uint8_t bit_offset,
		       uint64_t bit_field_size,
		       enum drgn_byte_order byte_order);

/**
 * Set a @ref drgn_object to a reference.
 *
 * @param[out] res Object to set.
 * @param[in] qualified_type Type to set to.
 * @param[in] address Address of the object.
 * @param[in] bit_offset Offset of the value from @p address. This may be
 * greater than or equal to 8.
 * @param[in] bit_field_size If the object should be a bit field, its size in
 * bits. Otherwise, 0.
 * @param[in] byte_order Byte order of the result.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_object_set_reference(struct drgn_object *res,
			  struct drgn_qualified_type qualified_type,
			  uint64_t address, uint64_t bit_offset,
			  uint64_t bit_field_size,
			  enum drgn_byte_order byte_order);

/**
 * Set a @ref drgn_object to a integer literal.
 *
 * This determines the type based on the programming language of the program
 * that the object belongs to.
 *
 * @param[out] res Object to set.
 * @param[in] uvalue Integer value.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_integer_literal(struct drgn_object *res,
					       uint64_t uvalue);

/**
 * Set a @ref drgn_object to a boolean literal.
 *
 * This determines the type based on the programming language of the program
 * that the object belongs to.
 *
 * @param[out] res Object to set.
 * @param[in] bvalue Boolean value.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_bool_literal(struct drgn_object *res,
					    bool bvalue);

/**
 * Set a @ref drgn_object to a floating-point literal.
 *
 * This determines the type based on the programming language of the program
 * that the object belongs to.
 *
 * @param[out] res Object to set.
 * @param[in] fvalue Floating-point value.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_float_literal(struct drgn_object *res,
					     double fvalue);

/** @} */

/**
 * @defgroup ObjectHelpers Helpers
 *
 * Object helpers.
 *
 * Several helpers are provided for working with @ref drgn_object%s.
 *
 * Helpers which return a @ref drgn_object have the same calling convention: the
 * result object is the first argument, which must be initialized and may be the
 * same as the input object argument; the result is only modified if the helper
 * succeeds.
 *
 * @{
 */

/**
 * Set a @ref drgn_object to another object.
 *
 * This copies @c obj to @c res. If @c obj is a value, then @c res is set to a
 * value with the same type and value, and similarly if @c obj was a reference,
 * @c res is set to the same reference.
 *
 * @param[out] res Destination object.
 * @param[in] obj Source object.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_copy(struct drgn_object *res,
				    const struct drgn_object *obj);

/**
 * Get a @ref drgn_object from a slice of a @ref DRGN_OBJECT_BUFFER object.
 *
 * This is a low-level interface used to implement @ref drgn_object_subscript()
 * and @ref drgn_object_member(). Those functions are usually more convenient.
 *
 * If multiple elements of an array are accessed (e.g., when iterating through
 * it), it can be more efficient to call @ref drgn_program_element_info() once
 * to get the required information and this function with the computed bit
 * offset for each element.
 *
 * If the same member of a type is accessed repeatedly (e.g., in a loop), it can
 * be more efficient to call @ref drgn_program_member_info() once to get the
 * required information and this function to access the member each time.
 *
 * @sa drgn_object_pointer_offset
 *
 * @param[out] res Destination object.
 * @param[in] obj Source object.
 * @param[in] qualified_type Result type.
 * @param[in] bit_offset Offset in bits from the beginning of @p obj.
 * @param[in] bit_field_size If the object should be a bit field, its size in
 * bits. Otherwise, 0.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_slice(struct drgn_object *res,
				     const struct drgn_object *obj,
				     struct drgn_qualified_type qualified_type,
				     uint64_t bit_offset,
				     uint64_t bit_field_size);

/**
 * Get a @ref drgn_object from dereferencing a pointer object with an offset.
 *
 * This is a low-level interface used to implement @ref drgn_object_subscript()
 * and @ref drgn_object_member_dereference(). Those functions are usually more
 * convenient, but this function can be more efficient if accessing multiple
 * elements or the same member multiple times.
 *
 * @sa drgn_object_slice
 *
 * @param[out] res Dereferenced object.
 * @param[in] obj Pointer object.
 * @param[in] qualified_type Result type.
 * @param[in] bit_offset Offset in bits from the address given by the value of
 * @p obj.
 * @param[in] bit_field_size If the object should be a bit field, its size in
 * bits. Otherwise, 0.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_object_dereference_offset(struct drgn_object *res,
			       const struct drgn_object *obj,
			       struct drgn_qualified_type qualified_type,
			       uint64_t bit_offset, uint64_t bit_field_size);

/**
 * Read a @ref drgn_object.
 *
 * If @c obj is already a value, then this is equivalent to @ref
 * drgn_object_copy(). If @c is a reference, then this reads the reference and
 * sets @res to the value.
 *
 * @param[out] res Object to set.
 * @param[in] obj Object to read.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_read(struct drgn_object *res,
				    const struct drgn_object *obj);

/**
 * Read the value of a @ref drgn_object.
 *
 * If @p obj is a value, that value is returned directly. If @p is a reference,
 * the value is read into the provided temporary buffer.
 *
 * This must be paired with @ref drgn_object_deinit_value().
 *
 * @param[in] obj Object to read.
 * @param[in] value Temporary value to use if necessary.
 * @param[out] ret Pointer to the returned value, which is <tt>&obj->value</tt>
 * if @p obj is a value, or @p value if @p obj is a reference.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_read_value(const struct drgn_object *obj,
					  union drgn_value *value,
					  const union drgn_value **ret);

/**
 * Deinitialize a value which was read with @ref drgn_object_read_value().
 *
 * @param[in] obj Object which was read.
 * @param[in] value Value returned from @ref drgn_object_read_value() in @p ret.
 */
void drgn_object_deinit_value(const struct drgn_object *obj,
			      const union drgn_value *value);

/**
 * Get the value of an object with kind @ref
 * drgn_object_kind::DRGN_OBJECT_SIGNED.
 *
 * If the object is not a signed integer, an error is returned.
 *
 * @param[in] obj Object to read.
 * @param[out] ret Returned value.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_read_signed(const struct drgn_object *obj,
					   int64_t *ret);

/**
 * Get the value of an object with kind @ref
 * drgn_object_kind::DRGN_OBJECT_UNSIGNED.
 *
 * If the object is not an unsigned integer, an error is returned.
 *
 * @param[in] obj Object to read.
 * @param[out] ret Returned value.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_read_unsigned(const struct drgn_object *obj,
					     uint64_t *ret);

/**
 * Get the value of an object with kind @ref
 * drgn_object_kind::DRGN_OBJECT_FLOAT.
 *
 * If the object does not have a floating-point type, an error is returned.
 *
 * @param[in] obj Object to read.
 * @param[out] ret Returned value.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_read_float(const struct drgn_object *obj,
					  double *ret);

/**
 * Read the null-terminated string pointed to by a @ref drgn_object.
 *
 * This is only valid for pointers and arrays. The element type is ignored; this
 * operates byte-by-byte.
 *
 * For pointers and flexible arrays, this stops at the first null byte.
 *
 * For complete arrays, this stops at the first null byte or at the end of the
 * array.
 *
 * The returned string is always null-terminated.
 *
 * @param[in] obj Object to read.
 * @param[out] ret Returned string. On success, it must be freed with @c free().
 * On error, its contents are undefined.
 */
struct drgn_error *drgn_object_read_c_string(const struct drgn_object *obj,
					     char **ret);

/**
 * Pretty print a @ref drgn_object.
 *
 * This will format the object similarly to an expression in its programming
 * language.
 *
 * @param[in] obj Object to format.
 * @param[in] columns Number of columns to limit output to when the expression
 * can be reasonably wrapped.
 * @param[out] ret Returned string. On success, it must be freed with @c free().
 * On error, its contents are undefined.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_pretty_print_object(const struct drgn_object *obj,
					    size_t columns, char **ret);

/** @} */

/**
 * @defgroup ObjectOperators Operators
 *
 * Object operators.
 *
 * Various operators are defined on @ref drgn_object%s. These operators obey the
 * rules of the programming language of the given objects.
 *
 * Operators which return a @ref drgn_object have the same calling convention:
 * the result object is the first argument, which must be initialized and may be
 * the same as one or more of the operands; the result is only modified if the
 * operator succeeds.
 *
 * @{
 */

/**
 * Set a @ref drgn_object to the value of an object casted to a another type.
 *
 * Objects with a scalar type can be casted to a different scalar type. Other
 * objects can only be casted to the same type. @p res is always set to a value
 * object.
 *
 * @sa drgn_object_reinterpret()
 *
 * @param[out] res Object to set.
 * @param[in] qualified_type New type.
 * @param[in] obj Object to read.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_cast(struct drgn_object *res,
				    struct drgn_qualified_type qualified_type,
				    const struct drgn_object *obj);

/**
 * Set a @ref drgn_object to the value of an object reinterpreted as another
 * type.
 *
 * This reinterprets the raw memory of the object, so an object can be
 * reinterpreted as any other type. However, value objects with a scalar type
 * cannot be reinterpreted, as their memory layout is not known.
 *
 * If @c obj is a value, then @c res is set to a value; if @c obj was a
 * reference, then @c res is set to a reference.
 *
 * @sa drgn_object_cast()
 *
 * @param[out] res Object to set.
 * @param[in] qualified_type New type.
 * @param[in] obj Object to reinterpret.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_object_reinterpret(struct drgn_object *res,
			struct drgn_qualified_type qualified_type,
			enum drgn_byte_order byte_order,
			const struct drgn_object *obj);

/**
 * @ref drgn_object binary operator.
 *
 * Binary operators apply any language-specific conversions to @p lhs and @p
 * rhs, apply the operator, and store the result in @p res.
 *
 * @param[out] res Operator result. May be the same as @p lhs and/or @p rhs.
 * @param[in] lhs Operator left hand side.
 * @param[in] rhs Operator right hand side.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
typedef struct drgn_error *drgn_binary_op(struct drgn_object *res,
					  const struct drgn_object *lhs,
					  const struct drgn_object *rhs);

/**
 * @ref drgn_object unary operator.
 *
 * Unary operators apply any language-specific conversions to @p obj, apply the
 * operator, and store the result in @p res.
 *
 * @param[out] res Operator result. May be the same as @p obj.
 * @param[in] obj Operand.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
typedef struct drgn_error *drgn_unary_op(struct drgn_object *res,
					 const struct drgn_object *obj);

/**
 * Convert a @ref drgn_object to a boolean value.
 *
 * This gets the "truthiness" of an object according to its programming
 * language.
 *
 * @param[in] obj Object.
 * @param[out] ret Returned boolean value.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_bool(const struct drgn_object *obj, bool *ret);

/**
 * Compare the value of two @ref drgn_object%s.
 *
 * This applies any language-specific conversions to @p lhs and @p rhs and
 * compares the resulting values.
 *
 * @param[in] lhs Comparison left hand side.
 * @param[in] rhs Comparison right hand side.
 * @param[out] ret 0 if the operands are equal, < 0 if @p lhs < @p rhs, and > 0
 * if @p lhs > @p rhs.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_cmp(const struct drgn_object *lhs,
				   const struct drgn_object *rhs, int *ret);

/** Add (@c +) two @ref drgn_object%s. */
drgn_binary_op drgn_object_add;
/** Subtract (@c -) a @ref drgn_object from another. */
drgn_binary_op drgn_object_sub;
/** Multiply (@c *) two @ref drgn_object%s. */
drgn_binary_op drgn_object_mul;
/** Divide (@c /) a @ref drgn_object by another. */
drgn_binary_op drgn_object_div;
/** Calculate the modulus (@c %) of two @ref drgn_object%s. */
drgn_binary_op drgn_object_mod;
/** Left shift (@c <<) a @ref drgn_object by another. */
drgn_binary_op drgn_object_lshift;
/** Right shift (@c >>) a @ref drgn_object by another. */
drgn_binary_op drgn_object_rshift;
/** Calculate the bitwise and (@c &) of two @ref drgn_object%s. */
drgn_binary_op drgn_object_and;
/** Calculate the bitwise or (@c |) of two @ref drgn_object%s. */
drgn_binary_op drgn_object_or;
/** Calculate the bitwise exclusive or (@c ^) of two @ref drgn_object%s. */
drgn_binary_op drgn_object_xor;
/** Apply unary plus (@c +) to a @ref drgn_object. */
drgn_unary_op drgn_object_pos;
/** Calculate the arithmetic negation (@c -) of a @ref drgn_object. */
drgn_unary_op drgn_object_neg;
/** Calculate the bitwise negation (@c ~) of a @ref drgn_object. */
drgn_unary_op drgn_object_not;

/**
 * Get the address of (@c &) a @ref drgn_object as an object.
 *
 * This is only possible for reference objects, as value objects don't have an
 * address in the program.
 *
 * @param[out] res Resulting pointer value. May be the same as @p obj.
 * @param[in] obj Reference object.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
struct drgn_error *drgn_object_address_of(struct drgn_object *res,
					  const struct drgn_object *obj);

/**
 * Subscript (@c []) a @ref drgn_object.
 *
 * This is applicable to pointers and arrays.
 *
 * @param[out] res Resulting element. May be the same as @p obj.
 * @param[in] obj Object to subscript.
 * @param[in] index Element index.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
struct drgn_error *drgn_object_subscript(struct drgn_object *res,
					 const struct drgn_object *obj,
					 uint64_t index);

/**
 * Deference (@c *) a @ref drgn_object.
 *
 * This is equivalent to @ref drgn_object_subscript with an index of 0.
 *
 * @param[out] res Deferenced object. May be the same as @p obj.
 * @param[in] obj Object to dereference.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
static inline struct drgn_error *
drgn_object_dereference(struct drgn_object *res, const struct drgn_object *obj)
{
	return drgn_object_subscript(res, obj, 0);
}

/**
 * Get a member of a structure or union @ref drgn_object (@c .).
 *
 * @param[out] res Returned member. May be the same as @p obj.
 * @param[in] obj Object.
 * @param[in] member_name Name of member.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
struct drgn_error *drgn_object_member(struct drgn_object *res,
				      const struct drgn_object *obj,
				      const char *member_name);

/**
 * Get a member of a pointer @ref drgn_object (@c ->).
 *
 * This is applicable to pointers to structures and pointers to unions.
 *
 * @param[out] res Returned member. May be the same as @p obj.
 * @param[in] obj Object.
 * @param[in] member_name Name of member.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
struct drgn_error *drgn_object_member_dereference(struct drgn_object *res,
						  const struct drgn_object *obj,
						  const char *member_name);


/**
 * Get the containing object of a member @ref drgn_object.
 *
 * This corresponds to the @c container_of() macro commonly used in C.
 *
 * @param[out] res Returned object. May be the same as @p obj.
 * @param[in] obj Pointer to a member.
 * @param[in] type Type which contains the member.
 * @param[in] member_designator Name of the member in @p type. This can include
 * one or more member references and zero or more array subscripts.
 * @return @c NULL on success, non-@c NULL on error. @p res is not modified on
 * error.
 */
struct drgn_error *
drgn_object_container_of(struct drgn_object *res, const struct drgn_object *obj,
			 struct drgn_qualified_type qualified_type,
			 const char *member_designator);

/**
 * Get the size of a @ref drgn_object in bytes.
 *
 * @param[in] obj Object.
 * @param[out] ret Returned size.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_object_sizeof(const struct drgn_object *obj,
				      uint64_t *ret);

/** @} */

/** @} */

#endif /* DRGN_H */
