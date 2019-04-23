// Copyright 2018-2019 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

#include <assert.h>
#include <dwarf.h>
#include <elfutils/libdw.h>
#include <fcntl.h>
#include <gelf.h>
#include <inttypes.h>
#include <libelf.h>
#include <omp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "internal.h"
#include "dwarf_index.h"
#include "hash_table.h"
#include "read.h"
#include "siphash.h"

enum {
	SECTION_SYMTAB,
	SECTION_DEBUG_ABBREV,
	SECTION_DEBUG_INFO,
	SECTION_DEBUG_LINE,
	SECTION_DEBUG_STR,
	NUM_SECTIONS,
};

static const char * const section_name[NUM_SECTIONS] = {
	[SECTION_SYMTAB] = ".symtab",
	[SECTION_DEBUG_ABBREV] = ".debug_abbrev",
	[SECTION_DEBUG_INFO] = ".debug_info",
	[SECTION_DEBUG_LINE] = ".debug_line",
	[SECTION_DEBUG_STR] = ".debug_str",
};

static const bool section_optional[NUM_SECTIONS] = {
	[SECTION_SYMTAB] = true,
	[SECTION_DEBUG_LINE] = true,
};

/*
 * The DWARF abbreviation table gets translated into a series of instructions.
 * An instruction <= INSN_MAX_SKIP indicates a number of bytes to be skipped
 * over. The next few instructions mean that the corresponding attribute can be
 * skipped over. The remaining instructions indicate that the corresponding
 * attribute should be parsed. Finally, every sequence of instructions
 * corresponding to a DIE is terminated by a zero byte followed by a bitmask of
 * TAG_FLAG_* bits combined with the DWARF tag (which may be set to zero if the
 * tag is not of interest).
 */
enum {
	INSN_MAX_SKIP = 229,
	ATTRIB_BLOCK1,
	ATTRIB_BLOCK2,
	ATTRIB_BLOCK4,
	ATTRIB_EXPRLOC,
	ATTRIB_LEB128,
	ATTRIB_STRING,
	ATTRIB_SIBLING_REF1,
	ATTRIB_SIBLING_REF2,
	ATTRIB_SIBLING_REF4,
	ATTRIB_SIBLING_REF8,
	ATTRIB_SIBLING_REF_UDATA,
	ATTRIB_NAME_STRP4,
	ATTRIB_NAME_STRP8,
	ATTRIB_NAME_STRING,
	ATTRIB_STMT_LIST_LINEPTR4,
	ATTRIB_STMT_LIST_LINEPTR8,
	ATTRIB_DECL_FILE_DATA1,
	ATTRIB_DECL_FILE_DATA2,
	ATTRIB_DECL_FILE_DATA4,
	ATTRIB_DECL_FILE_DATA8,
	ATTRIB_DECL_FILE_UDATA,
	ATTRIB_SPECIFICATION_REF1,
	ATTRIB_SPECIFICATION_REF2,
	ATTRIB_SPECIFICATION_REF4,
	ATTRIB_SPECIFICATION_REF8,
	ATTRIB_SPECIFICATION_REF_UDATA,
	ATTRIB_MAX_INSN = ATTRIB_SPECIFICATION_REF_UDATA,
};

enum {
	/* Maximum number of bits used by the tags we care about. */
	TAG_BITS = 6,
	TAG_MASK = (1 << TAG_BITS) - 1,
	/* The remaining bits can be used for other purposes. */
	TAG_FLAG_DECLARATION = 0x40,
	TAG_FLAG_CHILDREN = 0x80,
};

struct abbrev_table {
	/*
	 * This array is indexed on the DWARF abbreviation code minus one. It
	 * maps the abbreviation code to an index in the insns array where the
	 * instruction stream for that code begins.
	 *
	 * Technically, abbreviation codes don't have to be sequential. In
	 * practice, GCC seems to always generate sequential codes starting at
	 * one, so we can get away with a flat array.
	 */
	uint32_t *decls;
	size_t num_decls;
	uint8_t *insns;
};

struct file_name_table {
	uint64_t *file_name_hashes;
	size_t num_files;
};

struct compilation_unit {
	struct debug_file *file;
	const char *ptr;
	uint64_t unit_length;
	uint16_t version;
	uint64_t debug_abbrev_offset;
	uint8_t address_size;
	bool is_64_bit;
};

struct debug_file {
	Elf_Data *sections[NUM_SECTIONS];
	/* Other byte order. */
	bool bswap;
	bool failed;
	int fd;
	/*
	 * If this is NULL, then we didn't open the file and don't own the Elf
	 * handle.
	 */
	const char *path;
	Elf *elf;
	Dwarf *dwarf;
	Elf_Data *rela_sections[NUM_SECTIONS];
	struct debug_file *next;
};

static inline const char *section_ptr(Elf_Data *data, size_t offset)
{
	return &((char *)data->d_buf)[offset];
}

static inline const char *section_end(Elf_Data *data)
{
	return section_ptr(data, data->d_size);
}

/*
 * An indexed DIE.
 *
 * DIEs with the same name but different tags or files are considered distinct.
 * We only compare the hash of the file name, not the string value, because a
 * 64-bit collision is unlikely enough, especially when also considering the
 * name and tag.
 */
struct die_entry {
	uint64_t tag;
	uint64_t file_name_hash;
	/*
	 * The next DIE with the same name (as an index into
	 * dwarf_index_shard::entries), or SIZE_MAX if this is the last DIE.
	 */
	size_t next;
	struct debug_file *file;
	uint64_t offset;
};

/*
 * The key is the DIE name. The value is the first DIE with that name (as an
 * index into dwarf_index_shard::entries).
 */
DEFINE_HASH_MAP(die_map, struct string, size_t, string_hash, string_eq)

struct dwarf_index_shard {
	omp_lock_t lock;
	struct die_map map;
	/*
	 * We store all entries in a shard as a single array, which is more
	 * cache friendly.
	 */
	struct die_entry *entries;
	size_t num_entries, entries_capacity;
};

#define SHARD_BITS 8

static inline size_t hash_pair_to_shard(struct hash_pair hp)
{
	/*
	 * The 8 most significant bits of the hash are used as the F14 tag, so
	 * we don't want to use those for sharding.
	 */
	return ((hp.first >> (8 * sizeof(size_t) - 8 - SHARD_BITS)) &
		(((size_t)1 << SHARD_BITS) - 1));
}

DEFINE_HASH_MAP(debug_file_map, const char *, struct debug_file *,
		c_string_hash, c_string_eq)

struct drgn_dwarf_index {
	/* DRGN_DWARF_INDEX_* flags passed to drgn_dwarf_index_create(). */
	int flags;
	struct debug_file_map files;
	struct debug_file *opened_first, *opened_last;
	struct debug_file *indexed_first, *indexed_last;
	/* The index is sharded to reduce lock contention. */
	struct dwarf_index_shard shards[1 << SHARD_BITS];
};

static inline struct drgn_error *drgn_eof(void)
{
	return drgn_error_create(DRGN_ERROR_DWARF_FORMAT,
				 "debug information is truncated");
}

static inline bool skip_leb128(const char **ptr, const char *end)
{
	for (;;) {
		if (*ptr >= end)
			return false;
		if (!(*(const uint8_t *)(*ptr)++ & 0x80))
			return true;
	}
}

static inline struct drgn_error *read_uleb128(const char **ptr, const char *end,
					      uint64_t *value)
{
	int shift = 0;
	uint8_t byte;

	*value = 0;
	for (;;) {
		if (*ptr >= end)
			return drgn_eof();
		byte = *(const uint8_t *)*ptr;
		(*ptr)++;
		if (shift == 63 && byte > 1) {
			return drgn_error_create(DRGN_ERROR_OVERFLOW,
						 "ULEB128 overflowed unsigned 64-bit integer");
		}
		*value |= (uint64_t)(byte & 0x7f) << shift;
		shift += 7;
		if (!(byte & 0x80))
			break;
	}
	return NULL;
}

static inline struct drgn_error *read_uleb128_into_size_t(const char **ptr,
							  const char *end,
							  size_t *value)
{
	struct drgn_error *err;
	uint64_t tmp;

	if ((err = read_uleb128(ptr, end, &tmp)))
		return err;

	if (tmp > SIZE_MAX)
		return drgn_eof();
	*value = tmp;
	return NULL;
}

static void free_shards(struct drgn_dwarf_index *dindex, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(dindex->shards[i].entries);
		die_map_deinit(&dindex->shards[i].map);
		omp_destroy_lock(&dindex->shards[i].lock);
	}
}

struct drgn_error *
drgn_dwarf_index_create(int flags, struct drgn_dwarf_index **ret)
{
	static const size_t initial_shard_capacity = max(1024 >> SHARD_BITS, 1);
	struct drgn_error *err;
	struct drgn_dwarf_index *dindex;
	size_t i;

	if (flags & ~DRGN_DWARF_INDEX_ALL) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "invalid flags");
	}
	dindex = malloc(sizeof(*dindex));
	if (!dindex)
		return &drgn_enomem;
	dindex->flags = flags;
	debug_file_map_init(&dindex->files);
	dindex->opened_first = dindex->opened_last = NULL;
	dindex->indexed_first = dindex->indexed_last = NULL;
	for (i = 0; i < ARRAY_SIZE(dindex->shards); i++) {
		struct dwarf_index_shard *shard = &dindex->shards[i];

		omp_init_lock(&shard->lock);
		die_map_init(&shard->map);
		shard->num_entries = 0;
		shard->entries_capacity = initial_shard_capacity;
		shard->entries = malloc_array(initial_shard_capacity,
					      sizeof(*shard->entries));
		if (!shard->entries ||
		    !die_map_reserve(&shard->map, initial_shard_capacity)) {
			free_shards(dindex, i + 1);
			err = &drgn_enomem;
			goto err;
		}
	}
	*ret = dindex;
	return NULL;

err:
	debug_file_map_deinit(&dindex->files);
	free(dindex);
	return err;
}

static void free_files(struct drgn_dwarf_index *dindex,
		       struct debug_file *files)
{
	struct debug_file *file, *next;

	file = files;
	while (file) {
		next = file->next;
		dwarf_end(file->dwarf);
		if (file->path) {
			elf_end(file->elf);
			close(file->fd);
			debug_file_map_delete(&dindex->files, &file->path);
			free((char *)file->path);
		}
		free(file);
		file = next;
	}
}

void drgn_dwarf_index_destroy(struct drgn_dwarf_index *dindex)
{
	if (dindex) {
		free_shards(dindex, ARRAY_SIZE(dindex->shards));
		free_files(dindex, dindex->opened_first);
		free_files(dindex, dindex->indexed_first);
		debug_file_map_deinit(&dindex->files);
		free(dindex);
	}
}

static struct drgn_error *read_sections(struct debug_file *file)
{
	struct drgn_error *err;
	const char *e_ident;
	size_t shstrndx;
	Elf_Scn *scn = NULL;
	size_t section_index[NUM_SECTIONS] = {};
	size_t i;

	e_ident = elf_getident(file->elf, NULL);
	if (!e_ident)
		return &drgn_not_elf;

	file->bswap = (e_ident[EI_DATA] !=
		       (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ?
			ELFDATA2LSB : ELFDATA2MSB));

	if (elf_getshdrstrndx(file->elf, &shstrndx))
		return drgn_error_libelf();

	/* First pass: get the symbol table and all debug sections. */
	while ((scn = elf_nextscn(file->elf, scn))) {
		GElf_Shdr *shdr, shdr_mem;
		const char *scnname;

		shdr = gelf_getshdr(scn, &shdr_mem);
		if (!shdr)
			return drgn_error_libelf();

		if (shdr->sh_type == SHT_NOBITS || (shdr->sh_flags & SHF_GROUP))
			continue;

		scnname = elf_strptr(file->elf, shstrndx, shdr->sh_name);
		if (!scnname)
			continue;

		for (i = 0; i < NUM_SECTIONS; i++) {
			if (file->sections[i])
				continue;

			if (strcmp(scnname, section_name[i]) != 0)
				continue;

			err = read_elf_section(scn, &file->sections[i]);
			if (err)
				return err;
			section_index[i] = elf_ndxscn(scn);
		}
	}

	for (i = 0; i < NUM_SECTIONS; i++) {
		if (!file->sections[i] && !section_optional[i]) {
			return drgn_error_format(DRGN_ERROR_MISSING_DEBUG,
						 "ELF file has no %s section",
						 section_name[i]);
		}
	}

	/* Second pass: get the relocation sections. */
	while ((scn = elf_nextscn(file->elf, scn))) {
		GElf_Shdr *shdr, shdr_mem;

		shdr = gelf_getshdr(scn, &shdr_mem);
		if (!shdr)
			return drgn_error_libelf();

		if (shdr->sh_type != SHT_RELA)
			continue;

		for (i = 0; i < NUM_SECTIONS; i++) {
			if (file->rela_sections[i])
				continue;

			if (shdr->sh_info != section_index[i])
				continue;

			if (e_ident[EI_CLASS] != ELFCLASS64) {
				return drgn_error_create(DRGN_ERROR_ELF_FORMAT,
							 "32-bit ELF relocations are not implemented");
			}
			if (!file->sections[SECTION_SYMTAB]) {
				return drgn_error_create(DRGN_ERROR_ELF_FORMAT,
							 "ELF file has no .symtab section");
			}
			if (shdr->sh_link != section_index[SECTION_SYMTAB]) {
				return drgn_error_create(DRGN_ERROR_ELF_FORMAT,
							 "relocation symbol table section is not .symtab");
			}

			err = read_elf_section(scn, &file->rela_sections[i]);
			if (err)
				return err;
		}
	}

	return NULL;
}

struct drgn_error *drgn_dwarf_index_open(struct drgn_dwarf_index *dindex,
					 const char *path, Elf **elf)
{
	struct drgn_error *err;
	const char *key;
	struct hash_pair hp;
	struct debug_file_map_pos pos;
	struct debug_file *file;

	key = realpath(path, NULL);
	if (!key)
		return drgn_error_create_os(errno, path, "realpath");

	hp = debug_file_map_hash(&path);
	pos = debug_file_map_search_pos(&dindex->files, &key, hp);
	if (pos.item) {
		free((char *)key);
		file = pos.item->value;
		goto out;
	}

	file = calloc(1, sizeof(*file));
	if (!file) {
		err = &drgn_enomem;
		goto err_key;
	}

	file->path = key;

	file->fd = open(path, O_RDONLY);
	if (file->fd == -1) {
		err = drgn_error_create_os(errno, path, "open");
		goto err_file;
	}

	elf_version(EV_CURRENT);

	file->elf = elf_begin(file->fd, ELF_C_READ_MMAP_PRIVATE, NULL);
	if (!file->elf) {
		err = drgn_error_libelf();
		goto err_fd;
	}

	pos = debug_file_map_insert_searched_pos(&dindex->files, &key, &file,
						 hp);
	if (!pos.item) {
		err = &drgn_enomem;
		goto err_elf;
	}

	err = read_sections(file);
	if (err)
		goto err_hash;

	if (dindex->opened_last)
		dindex->opened_last->next = file;
	else
		dindex->opened_first = file;
	dindex->opened_last = file;
out:
	if (elf)
		*elf = file->elf;
	return NULL;

err_hash:
	debug_file_map_delete_pos(&dindex->files, pos, hp);
err_elf:
	elf_end(file->elf);
err_fd:
	close(file->fd);
err_file:
	free(file);
err_key:
	free((char *)key);
	return err;
}

struct drgn_error *drgn_dwarf_index_open_elf(struct drgn_dwarf_index *dindex,
					     Elf *elf)
{
	struct drgn_error *err;
	struct debug_file *file;

	file = calloc(1, sizeof(*file));
	if (!file)
		return &drgn_enomem;

	file->elf = elf;

	err = read_sections(file);
	if (err)
		goto err;

	if (dindex->opened_last)
		dindex->opened_last->next = file;
	else
		dindex->opened_first = file;
	dindex->opened_last = file;
	return NULL;

err:
	free(file);
	return err;
}

static struct drgn_error *apply_relocation(Elf_Data *section,
					   Elf_Data *rela_section,
					   Elf_Data *symtab, size_t i)
{
	const Elf64_Rela *reloc;
	const Elf64_Sym *syms;
	size_t num_syms;
	uint32_t r_sym;
	uint32_t r_type;
	char *p;

	reloc = &((Elf64_Rela *)rela_section->d_buf)[i];
	syms = (Elf64_Sym *)symtab->d_buf;
	num_syms = symtab->d_size / sizeof(Elf64_Sym);

	p = (char *)section->d_buf + reloc->r_offset;
	r_sym = reloc->r_info >> 32;
	r_type = reloc->r_info & UINT32_C(0xffffffff);
	switch (r_type) {
	case R_X86_64_NONE:
		break;
	case R_X86_64_32:
		if (r_sym >= num_syms) {
			return drgn_error_create(DRGN_ERROR_ELF_FORMAT,
						 "invalid relocation symbol");
		}
		if (reloc->r_offset > SIZE_MAX - sizeof(uint32_t) ||
		    reloc->r_offset + sizeof(uint32_t) > section->d_size) {
			return drgn_error_create(DRGN_ERROR_ELF_FORMAT,
						 "invalid relocation offset");
		}
		*(uint32_t *)p = syms[r_sym].st_value + reloc->r_addend;
		break;
	case R_X86_64_64:
		if (r_sym >= num_syms) {
			return drgn_error_create(DRGN_ERROR_ELF_FORMAT,
						 "invalid relocation symbol");
		}
		if (reloc->r_offset > SIZE_MAX - sizeof(uint64_t) ||
		    reloc->r_offset + sizeof(uint64_t) > section->d_size) {
			return drgn_error_create(DRGN_ERROR_ELF_FORMAT,
						 "invalid relocation offset");
		}
		*(uint64_t *)p = syms[r_sym].st_value + reloc->r_addend;
		break;
	default:
		return drgn_error_format(DRGN_ERROR_ELF_FORMAT,
					 "unimplemented relocation type %" PRIu32,
					 r_type);
	}
	return NULL;
}

static size_t count_relocations(struct debug_file *files)
{
	struct debug_file *file = files;
	size_t count = 0;
	size_t i;

	while (file) {
		for (i = 0; i < NUM_SECTIONS; i++) {
			Elf_Data *data;

			data = file->rela_sections[i];
			if (data)
				count += data->d_size / sizeof(Elf64_Rela);
		}
		file = file->next;
	}
	return count;
}

static struct drgn_error *apply_relocations(struct debug_file *files)
{
	struct drgn_error *err = NULL;
	size_t total_num_relocs;

	total_num_relocs = count_relocations(files);
#pragma omp parallel
	{
		struct debug_file *file;
		size_t section_idx = 0, reloc_idx = 0;
		size_t i;
		bool first = true;
		size_t num_relocs = 0;
		struct drgn_error *err2;

#pragma omp for
		for (i = 0; i < total_num_relocs; i++) {
			if (err)
				continue;

			if (first) {
				size_t cur = 0;

				file = files;
				while (file) {
					for (section_idx = 0; section_idx < NUM_SECTIONS; section_idx++) {
						Elf_Data *data;

						data = file->rela_sections[section_idx];
						if (!data)
							continue;
						num_relocs = (data->d_size /
							      sizeof(Elf64_Rela));
						if (cur + num_relocs > i) {
							reloc_idx = i - cur;
							goto done;
						} else {
							cur += num_relocs;
						}
					}
					file = file->next;
				}
done:
				first = false;
			}

			if ((err2 = apply_relocation(file->sections[section_idx],
						     file->rela_sections[section_idx],
						     file->sections[SECTION_SYMTAB],
						     reloc_idx))) {
#pragma omp critical(relocations_err)
				{
					if (err)
						drgn_error_destroy(err2);
					else
						err = err2;
				}
				continue;
			}

			if (file) {
				reloc_idx++;
				while (reloc_idx >= num_relocs) {
					Elf_Data *data;

					reloc_idx = 0;
					if (++section_idx >= NUM_SECTIONS) {
						section_idx = 0;
						file = file->next;
						if (!file)
							break;
					}
					data = file->rela_sections[section_idx];
					if (data)
						num_relocs = (data->d_size /
							      sizeof(Elf64_Rela));
					else
						num_relocs = 0;
				}
			}
		}
	}

	return err;
}

static struct drgn_error *read_compilation_unit_header(const char *ptr,
						       const char *end,
						       struct compilation_unit *cu)
{
	uint32_t tmp;

	if (!read_u32(&ptr, end, cu->file->bswap, &tmp))
		return drgn_eof();
	cu->is_64_bit = tmp == UINT32_C(0xffffffff);
	if (cu->is_64_bit) {
		if (!read_u64(&ptr, end, cu->file->bswap, &cu->unit_length))
			return drgn_eof();
	} else {
		cu->unit_length = tmp;
	}

	if (!read_u16(&ptr, end, cu->file->bswap, &cu->version))
		return drgn_eof();
	if (cu->version != 2 && cu->version != 3 && cu->version != 4) {
		return drgn_error_format(DRGN_ERROR_DWARF_FORMAT,
					 "unknown DWARF CU version %" PRIu16,
					 cu->version);
	}

	if (cu->is_64_bit) {
		if (!read_u64(&ptr, end, cu->file->bswap,
			      &cu->debug_abbrev_offset))
			return drgn_eof();
	} else {
		if (!read_u32_into_u64(&ptr, end, cu->file->bswap,
				       &cu->debug_abbrev_offset))
			return drgn_eof();
	}

	if (!read_u8(&ptr, end, &cu->address_size))
		return drgn_eof();

	return NULL;
}

static struct drgn_error *read_cus(struct debug_file *file,
				   struct compilation_unit **cus,
				   size_t *num_cus, size_t *cus_capacity)
{
	struct drgn_error *err;
	Elf_Data *debug_info = file->sections[SECTION_DEBUG_INFO];
	const char *ptr = section_ptr(debug_info, 0);
	const char *end = section_end(debug_info);

	while (ptr < end) {
		struct compilation_unit *cu;

		if (*num_cus >= *cus_capacity) {
			size_t capacity = *cus_capacity;

			if (capacity == 0)
				capacity = 1;
			else
				capacity *= 2;
			if (!resize_array(cus, capacity))
				return &drgn_enomem;
			*cus_capacity = capacity;
		}

		cu = &(*cus)[(*num_cus)++];
		cu->file = file;
		cu->ptr = ptr;
		if ((err = read_compilation_unit_header(ptr, end, cu)))
			return err;

		ptr += (cu->is_64_bit ? 12 : 4) + cu->unit_length;
	}
	if (ptr > end)
		return drgn_eof();
	return NULL;
}

static struct drgn_error *append_insn(struct abbrev_table *table, uint8_t insn,
				      size_t *num_insns, size_t *insns_capacity)
{
	if (*num_insns >= *insns_capacity) {
		if (*insns_capacity == 0)
			*insns_capacity = 32;
		else
			*insns_capacity *= 2;
		if (!resize_array(&table->insns, *insns_capacity))
			return &drgn_enomem;
	}
	table->insns[(*num_insns)++] = insn;
	return NULL;
}

static inline bool is_type_tag(uint64_t tag)
{
	return (tag == DW_TAG_base_type ||
		tag == DW_TAG_class_type ||
		tag == DW_TAG_enumeration_type ||
		tag == DW_TAG_structure_type ||
		tag == DW_TAG_typedef ||
		tag == DW_TAG_union_type);
}

static struct drgn_error *read_abbrev_decl(int flags, const char **ptr,
					   const char *end,
					   const struct compilation_unit *cu,
					   struct abbrev_table *table,
					   size_t *decls_capacity,
					   size_t *num_insns,
					   size_t *insns_capacity)
{
	struct drgn_error *err;
	uint64_t code;
	uint64_t tag;
	uint8_t children;
	uint8_t die_flags;
	bool should_index;
	bool first = true;

	static_assert(ATTRIB_MAX_INSN == UINT8_MAX,
		      "maximum DWARF attribute instruction is invalid");

	if ((err = read_uleb128(ptr, end, &code)))
		return err;
	if (code == 0)
		return (struct drgn_error *)-1;
	if (code != table->num_decls + 1) {
		return drgn_error_create(DRGN_ERROR_DWARF_FORMAT,
					 "DWARF abbreviation table is not sequential");
	}

	if (table->num_decls >= *decls_capacity) {
		if (*decls_capacity == 0)
			*decls_capacity = 1;
		else
			*decls_capacity *= 2;
		if (!resize_array(&table->decls, *decls_capacity))
			return &drgn_enomem;
	}
	table->decls[table->num_decls++] = *num_insns;

	if ((err = read_uleb128(ptr, end, &tag)))
		return err;

	should_index = (((flags & DRGN_DWARF_INDEX_TYPES) && is_type_tag(tag)) ||
			((flags & DRGN_DWARF_INDEX_VARIABLES) && tag == DW_TAG_variable) ||
			((flags & DRGN_DWARF_INDEX_ENUMERATORS) && tag == DW_TAG_enumerator) ||
			((flags & DRGN_DWARF_INDEX_FUNCTIONS) && tag == DW_TAG_subprogram));

	if (should_index || tag == DW_TAG_compile_unit ||
	    ((flags & DRGN_DWARF_INDEX_ENUMERATORS) &&
	     tag == DW_TAG_enumeration_type))
		die_flags = tag;
	else
		die_flags = 0;

	if (!read_u8(ptr, end, &children))
		return drgn_eof();
	if (children)
		die_flags |= TAG_FLAG_CHILDREN;

	for (;;) {
		uint64_t name, form;
		uint8_t insn;

		if ((err = read_uleb128(ptr, end, &name)))
			return err;
		if ((err = read_uleb128(ptr, end, &form)))
			return err;
		if (name == 0 && form == 0)
			break;

		if (name == DW_AT_sibling &&
		    !((flags & DRGN_DWARF_INDEX_ENUMERATORS) &&
		      tag == DW_TAG_enumeration_type)) {
			/*
			 * If we are indexing enumerators, we must descend into
			 * DW_TAG_enumeration_type to find the DW_TAG_enumerator
			 * children instead of skipping to the sibling DIE.
			 */
			switch (form) {
			case DW_FORM_ref1:
				insn = ATTRIB_SIBLING_REF1;
				goto append_insn;
			case DW_FORM_ref2:
				insn = ATTRIB_SIBLING_REF2;
				goto append_insn;
			case DW_FORM_ref4:
				insn = ATTRIB_SIBLING_REF4;
				goto append_insn;
			case DW_FORM_ref8:
				insn = ATTRIB_SIBLING_REF8;
				goto append_insn;
			case DW_FORM_ref_udata:
				insn = ATTRIB_SIBLING_REF_UDATA;
				goto append_insn;
			default:
				break;
			}
		} else if (name == DW_AT_name && should_index) {
			switch (form) {
			case DW_FORM_strp:
				if (cu->is_64_bit)
					insn = ATTRIB_NAME_STRP8;
				else
					insn = ATTRIB_NAME_STRP4;
				goto append_insn;
			case DW_FORM_string:
				insn = ATTRIB_NAME_STRING;
				goto append_insn;
			default:
				break;
			}
		} else if (name == DW_AT_stmt_list &&
			   tag == DW_TAG_compile_unit &&
			   cu->file->sections[SECTION_DEBUG_LINE]) {
			switch (form) {
			case DW_FORM_data4:
				insn = ATTRIB_STMT_LIST_LINEPTR4;
				goto append_insn;
			case DW_FORM_data8:
				insn = ATTRIB_STMT_LIST_LINEPTR8;
				goto append_insn;
			case DW_FORM_sec_offset:
				if (cu->is_64_bit)
					insn = ATTRIB_STMT_LIST_LINEPTR8;
				else
					insn = ATTRIB_STMT_LIST_LINEPTR4;
				goto append_insn;
			default:
				break;
			}
		} else if (name == DW_AT_decl_file && should_index) {
			switch (form) {
			case DW_FORM_data1:
				insn = ATTRIB_DECL_FILE_DATA1;
				goto append_insn;
			case DW_FORM_data2:
				insn = ATTRIB_DECL_FILE_DATA2;
				goto append_insn;
			case DW_FORM_data4:
				insn = ATTRIB_DECL_FILE_DATA4;
				goto append_insn;
			case DW_FORM_data8:
				insn = ATTRIB_DECL_FILE_DATA8;
				goto append_insn;
			/*
			 * decl_file must be positive, so if the compiler uses
			 * DW_FORM_sdata for some reason, just treat it as
			 * udata.
			 */
			case DW_FORM_sdata:
			case DW_FORM_udata:
				insn = ATTRIB_DECL_FILE_UDATA;
				goto append_insn;
			default:
				break;
			}
		} else if (name == DW_AT_declaration) {
			/*
			 * In theory, this could be DW_FORM_flag with a value of
			 * zero, but in practice, GCC always uses
			 * DW_FORM_flag_present.
			 */
			die_flags |= TAG_FLAG_DECLARATION;
		} else if (name == DW_AT_specification && should_index) {
			switch (form) {
			case DW_FORM_ref1:
				insn = ATTRIB_SPECIFICATION_REF1;
				goto append_insn;
			case DW_FORM_ref2:
				insn = ATTRIB_SPECIFICATION_REF2;
				goto append_insn;
			case DW_FORM_ref4:
				insn = ATTRIB_SPECIFICATION_REF4;
				goto append_insn;
			case DW_FORM_ref8:
				insn = ATTRIB_SPECIFICATION_REF8;
				goto append_insn;
			case DW_FORM_ref_udata:
				insn = ATTRIB_SPECIFICATION_REF_UDATA;
				goto append_insn;
			default:
				break;
			}
		}

		switch (form) {
		case DW_FORM_addr:
			insn = cu->address_size;
			break;
		case DW_FORM_data1:
		case DW_FORM_ref1:
		case DW_FORM_flag:
			insn = 1;
			break;
		case DW_FORM_data2:
		case DW_FORM_ref2:
			insn = 2;
			break;
		case DW_FORM_data4:
		case DW_FORM_ref4:
			insn = 4;
			break;
		case DW_FORM_data8:
		case DW_FORM_ref8:
		case DW_FORM_ref_sig8:
			insn = 8;
			break;
		case DW_FORM_block1:
			insn = ATTRIB_BLOCK1;
			goto append_insn;
		case DW_FORM_block2:
			insn = ATTRIB_BLOCK2;
			goto append_insn;
		case DW_FORM_block4:
			insn = ATTRIB_BLOCK4;
			goto append_insn;
		case DW_FORM_exprloc:
			insn = ATTRIB_EXPRLOC;
			goto append_insn;
		case DW_FORM_sdata:
		case DW_FORM_udata:
		case DW_FORM_ref_udata:
			insn = ATTRIB_LEB128;
			goto append_insn;
		case DW_FORM_ref_addr:
		case DW_FORM_sec_offset:
		case DW_FORM_strp:
			insn = cu->is_64_bit ? 8 : 4;
			break;
		case DW_FORM_string:
			insn = ATTRIB_STRING;
			goto append_insn;
		case DW_FORM_flag_present:
			continue;
		case DW_FORM_indirect:
			return drgn_error_create(DRGN_ERROR_DWARF_FORMAT,
						 "DW_FORM_indirect is not implemented");
		default:
			return drgn_error_format(DRGN_ERROR_DWARF_FORMAT,
						 "unknown attribute form %" PRIu64,
						 form);
		}

		if (!first && table->insns[*num_insns - 1] < INSN_MAX_SKIP) {
			if ((uint16_t)table->insns[*num_insns - 1] + insn <= INSN_MAX_SKIP) {
				table->insns[*num_insns - 1] += insn;
				continue;
			} else {
				insn = (uint16_t)table->insns[*num_insns - 1] + insn - INSN_MAX_SKIP;
				table->insns[*num_insns - 1] = INSN_MAX_SKIP;
			}
		}

append_insn:
		first = false;
		if ((err = append_insn(table, insn, num_insns, insns_capacity)))
			return err;
	}
	if ((err = append_insn(table, 0, num_insns, insns_capacity)))
		return err;
	return append_insn(table, die_flags, num_insns, insns_capacity);
}

static struct drgn_error *read_abbrev_table(int flags, const char *ptr,
					    const char *end,
					    const struct compilation_unit *cu,
					    struct abbrev_table *table)
{
	struct drgn_error *err;
	size_t decls_capacity = 0;
	size_t num_insns = 0;
	size_t insns_capacity = 0;

	for (;;) {
		err = read_abbrev_decl(flags, &ptr, end, cu, table,
				       &decls_capacity, &num_insns,
				       &insns_capacity);
		if (err == (struct drgn_error *)-1)
			break;
		else if (err)
			return err;
	}
	return NULL;
}

static struct drgn_error *skip_lnp_header(struct debug_file *file,
					  const char **ptr, const char *end)
{
	uint32_t tmp;
	bool is_64_bit;
	uint16_t version;
	uint8_t opcode_base;

	if (!read_u32(ptr, end, file->bswap, &tmp))
		return drgn_eof();
	is_64_bit = tmp == UINT32_C(0xffffffff);
	if (is_64_bit)
		*ptr += sizeof(uint64_t);

	if (!read_u16(ptr, end, file->bswap, &version))
		return drgn_eof();
	if (version != 2 && version != 3 && version != 4) {
		return drgn_error_format(DRGN_ERROR_DWARF_FORMAT,
					 "unknown DWARF LNP version %" PRIu16,
					 version);
	}

	/*
	 * header_length
	 * minimum_instruction_length
	 * maximum_operations_per_instruction (DWARF 4 only)
	 * default_is_stmt
	 * line_base
	 * line_range
	 */
	*ptr += (is_64_bit ? 8 : 4) + 4 + (version >= 4);

	if (!read_u8(ptr, end, &opcode_base))
		return drgn_eof();
	/* standard_opcode_lengths */
	*ptr += opcode_base - 1;

	return NULL;
}

/*
 * Hash the canonical path of a directory. Components are hashed in reverse
 * order. We always include a trailing slash.
 */
static void hash_directory(struct siphash *hash, const char *path,
			   size_t path_len)
{
	struct path_iterator it = {
		.components = (struct path_iterator_component []){
			{ path, path_len, },
		},
		.num_components = 1,
	};
	const char *component;
	size_t component_len;

	while (path_iterator_next(&it, &component, &component_len)) {
		siphash_update(hash, component, component_len);
		siphash_update(hash, "/", 1);
	}
}

static struct drgn_error *read_file_name_table(struct drgn_dwarf_index *dindex,
					       struct compilation_unit *cu,
					       size_t stmt_list,
					       struct file_name_table *table)
{
	/*
	 * We don't care about hash flooding attacks, so don't bother with the
	 * random key.
	 */
	static const uint64_t siphash_key[2];
	struct drgn_error *err;
	struct debug_file *file = cu->file;
	Elf_Data *debug_line = file->sections[SECTION_DEBUG_LINE];
	const char *ptr = section_ptr(debug_line, stmt_list);
	const char *end = section_end(debug_line);
	struct siphash *directories = NULL;
	size_t num_directories = 0;
	size_t directories_capacity = 0;
	size_t files_capacity = 0;

	if ((err = skip_lnp_header(file, &ptr, end)))
		return err;

	for (;;) {
		struct siphash *hash;
		const char *path;
		size_t path_len;

		if (!read_string(&ptr, end, &path, &path_len))
			return drgn_eof();
		if (!path_len)
			break;

		if (num_directories >= directories_capacity) {
			if (directories_capacity == 0)
				directories_capacity = 16;
			else
				directories_capacity *= 2;
			if (!resize_array(&directories, directories_capacity)) {
				err = &drgn_enomem;
				goto out;
			}
		}

		hash = &directories[num_directories++];
		siphash_init(hash, siphash_key);
		hash_directory(hash, path, path_len);
	}

	for (;;) {
		const char *path;
		size_t path_len;
		uint64_t directory_index;
		struct siphash hash;

		if (!read_string(&ptr, end, &path, &path_len)) {
			err = drgn_eof();
			goto out;
		}
		if (!path_len)
			break;

		if ((err = read_uleb128(&ptr, end, &directory_index)))
			goto out;
		/* mtime, size */
		if (!skip_leb128(&ptr, end) || !skip_leb128(&ptr, end)) {
			err = drgn_eof();
			goto out;
		}

		if (directory_index > num_directories) {
			err = drgn_error_format(DRGN_ERROR_DWARF_FORMAT,
						"directory index %" PRIu64 " is invalid",
						directory_index);
			goto out;
		}

		if (directory_index)
			hash = directories[directory_index - 1];
		else
			siphash_init(&hash, siphash_key);
		siphash_update(&hash, path, path_len);

		if (table->num_files >= files_capacity) {
			if (files_capacity == 0)
				files_capacity = 16;
			else
				files_capacity *= 2;
			if (!resize_array(&table->file_name_hashes,
					  files_capacity)) {
				err = &drgn_enomem;
				goto out;
			}
		}
		table->file_name_hashes[table->num_files++] = siphash_final(&hash);
	}

	err = NULL;
out:
	free(directories);
	return err;
}

static bool append_die_entry(struct dwarf_index_shard *shard, uint64_t tag,
			     uint64_t file_name_hash, struct debug_file *file,
			     uint64_t offset)
{
	struct die_entry *entry;

	if (shard->num_entries >= shard->entries_capacity) {
		size_t new_capacity;

		new_capacity = shard->entries_capacity * 2;
		if (!resize_array(&shard->entries, new_capacity))
			return false;
		shard->entries_capacity = new_capacity;
	}

	entry = &shard->entries[shard->num_entries++];
	entry->tag = tag;
	entry->file_name_hash = file_name_hash;
	entry->file = file;
	entry->offset = offset;
	entry->next = SIZE_MAX;
	return true;
}

static struct drgn_error *index_die(struct drgn_dwarf_index *dindex,
				    const char *name, uint64_t tag,
				    uint64_t file_name_hash,
				    struct debug_file *file, uint64_t offset)
{
	struct drgn_error *err;
	struct string key = {
		.str = name,
		.len = strlen(name),
	};
	struct hash_pair hp;
	struct dwarf_index_shard *shard;
	size_t *value, index;
	struct die_entry *entry;

	hp = die_map_hash(&key);
	shard = &dindex->shards[hash_pair_to_shard(hp)];
	omp_set_lock(&shard->lock);
	value = die_map_search_hashed(&shard->map, &key, hp);
	if (!value) {
		if (!append_die_entry(shard, tag, file_name_hash, file,
				      offset)) {
			err = &drgn_enomem;
			goto out;
		}
		index = shard->num_entries - 1;
		if (die_map_insert_searched(&shard->map, &key, &index, hp))
			err = NULL;
		else
			err = &drgn_enomem;
		goto out;
	}

	entry = &shard->entries[*value];
	for (;;) {
		if (entry->tag == tag &&
		    entry->file_name_hash == file_name_hash) {
			err = NULL;
			goto out;
		}

		if (entry->next == SIZE_MAX)
			break;
		entry = &shard->entries[entry->next];
	}

	index = entry - shard->entries;
	if (!append_die_entry(shard, tag, file_name_hash, file, offset)) {
		err = &drgn_enomem;
		goto out;
	}
	shard->entries[index].next = shard->num_entries - 1;
	err = NULL;
out:
	omp_unset_lock(&shard->lock);
	return err;
}

struct die {
	const char *sibling;
	const char *name;
	size_t stmt_list;
	size_t decl_file;
	const char *specification;
	uint8_t flags;
};

static struct drgn_error *read_die(struct compilation_unit *cu,
				   const struct abbrev_table *abbrev_table,
				   const char **ptr, const char *end,
				   const char *debug_str_buffer,
				   const char *debug_str_end, struct die *die)
{
	struct drgn_error *err;
	uint64_t code;
	uint8_t *insnp;
	uint8_t insn;

	if ((err = read_uleb128(ptr, end, &code)))
		return err;
	if (code == 0)
		return (struct drgn_error *)-1;

	if (code < 1 || code > abbrev_table->num_decls) {
		return drgn_error_format(DRGN_ERROR_DWARF_FORMAT,
					 "unknown abbreviation code %" PRIu64,
					 code);
	}
	insnp = &abbrev_table->insns[abbrev_table->decls[code - 1]];

	while ((insn = *insnp++)) {
		size_t skip, tmp;

		switch (insn) {
		case ATTRIB_BLOCK1:
			if (!read_u8_into_size_t(ptr, end, &skip))
				return drgn_eof();
			goto skip;
		case ATTRIB_BLOCK2:
			if (!read_u16_into_size_t(ptr, end, cu->file->bswap,
						  &skip))
				return drgn_eof();
			goto skip;
		case ATTRIB_BLOCK4:
			if (!read_u32_into_size_t(ptr, end, cu->file->bswap,
						  &skip))
				return drgn_eof();
			goto skip;
		case ATTRIB_EXPRLOC:
			if ((err = read_uleb128_into_size_t(ptr, end, &skip)))
				return err;
			goto skip;
		case ATTRIB_LEB128:
			if (!skip_leb128(ptr, end))
				return drgn_eof();
			break;
		case ATTRIB_NAME_STRING:
			die->name = *ptr;
			/* fallthrough */
		case ATTRIB_STRING:
			if (!skip_string(ptr, end))
				return drgn_eof();
			break;
		case ATTRIB_SIBLING_REF1:
			if (!read_u8_into_size_t(ptr, end, &tmp))
				return drgn_eof();
			goto sibling;
		case ATTRIB_SIBLING_REF2:
			if (!read_u16_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
			goto sibling;
		case ATTRIB_SIBLING_REF4:
			if (!read_u32_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
			goto sibling;
		case ATTRIB_SIBLING_REF8:
			if (!read_u64_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
			goto sibling;
		case ATTRIB_SIBLING_REF_UDATA:
			if ((err = read_uleb128_into_size_t(ptr, end, &tmp)))
				return err;
sibling:
			if (!read_in_bounds(cu->ptr, end, tmp))
				return drgn_eof();
			die->sibling = &cu->ptr[tmp];
			__builtin_prefetch(die->sibling);
			break;
		case ATTRIB_NAME_STRP4:
			if (!read_u32_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
			goto strp;
		case ATTRIB_NAME_STRP8:
			if (!read_u64_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
strp:
			if (!read_in_bounds(debug_str_buffer, debug_str_end,
					    tmp))
				return drgn_eof();
			die->name = &debug_str_buffer[tmp];
			__builtin_prefetch(die->name);
			break;
		case ATTRIB_STMT_LIST_LINEPTR4:
			if (!read_u32_into_size_t(ptr, end, cu->file->bswap,
						  &die->stmt_list))
				return drgn_eof();
			break;
		case ATTRIB_STMT_LIST_LINEPTR8:
			if (!read_u64_into_size_t(ptr, end, cu->file->bswap,
						  &die->stmt_list))
				return drgn_eof();
			break;
		case ATTRIB_DECL_FILE_DATA1:
			if (!read_u8_into_size_t(ptr, end, &die->decl_file))
				return drgn_eof();
			break;
		case ATTRIB_DECL_FILE_DATA2:
			if (!read_u16_into_size_t(ptr, end, cu->file->bswap,
						  &die->decl_file))
				return drgn_eof();
			break;
		case ATTRIB_DECL_FILE_DATA4:
			if (!read_u32_into_size_t(ptr, end, cu->file->bswap,
						  &die->decl_file))
				return drgn_eof();
			break;
		case ATTRIB_DECL_FILE_DATA8:
			if (!read_u64_into_size_t(ptr, end, cu->file->bswap,
						  &die->decl_file))
				return drgn_eof();
			break;
		case ATTRIB_DECL_FILE_UDATA:
			if ((err = read_uleb128_into_size_t(ptr, end,
							    &die->decl_file)))
				return err;
			break;
		case ATTRIB_SPECIFICATION_REF1:
			if (!read_u8_into_size_t(ptr, end, &tmp))
				return drgn_eof();
			goto specification;
		case ATTRIB_SPECIFICATION_REF2:
			if (!read_u16_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
			goto specification;
		case ATTRIB_SPECIFICATION_REF4:
			if (!read_u32_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
			goto specification;
		case ATTRIB_SPECIFICATION_REF8:
			if (!read_u64_into_size_t(ptr, end, cu->file->bswap,
						  &tmp))
				return drgn_eof();
			goto specification;
		case ATTRIB_SPECIFICATION_REF_UDATA:
			if ((err = read_uleb128_into_size_t(ptr, end, &tmp)))
				return err;
specification:
			if (!read_in_bounds(cu->ptr, end, tmp))
				return drgn_eof();
			die->specification = &cu->ptr[tmp];
			__builtin_prefetch(die->specification);
			break;
		default:
			skip = insn;
skip:
			if (!read_in_bounds(*ptr, end, skip))
				return drgn_eof();
			*ptr += skip;
			break;
		}
	}

	die->flags = *insnp;

	return NULL;
}

static struct drgn_error *index_cu(struct drgn_dwarf_index *dindex,
				   struct compilation_unit *cu)
{
	struct drgn_error *err;
	struct abbrev_table abbrev_table = {};
	struct file_name_table file_name_table = {};
	struct debug_file *file = cu->file;
	Elf_Data *debug_abbrev = file->sections[SECTION_DEBUG_ABBREV];
	const char *debug_abbrev_end = section_end(debug_abbrev);
	const char *ptr = &cu->ptr[cu->is_64_bit ? 23 : 11];
	const char *end = &cu->ptr[(cu->is_64_bit ? 12 : 4) + cu->unit_length];
	Elf_Data *debug_info = file->sections[SECTION_DEBUG_INFO];
	const char *debug_info_buffer = section_ptr(debug_info, 0);
	Elf_Data *debug_str = file->sections[SECTION_DEBUG_STR];
	const char *debug_str_buffer = section_ptr(debug_str, 0);
	const char *debug_str_end = section_end(debug_str);
	unsigned int depth = 0;
	uint64_t enum_die_offset = 0;

	if ((err = read_abbrev_table(dindex->flags,
				     section_ptr(debug_abbrev,
						 cu->debug_abbrev_offset),
				     debug_abbrev_end, cu, &abbrev_table)))
		goto out;

	for (;;) {
		struct die die = {
			.stmt_list = SIZE_MAX,
		};
		uint64_t die_offset = ptr - debug_info_buffer;
		uint64_t tag;

		err = read_die(cu, &abbrev_table, &ptr, end, debug_str_buffer,
			       debug_str_end, &die);
		if (err == (struct drgn_error *)-1) {
			depth--;
			if (depth == 1)
				enum_die_offset = 0;
			else if (depth == 0)
				break;
			continue;
		} else if (err) {
			goto out;
		}

		tag = die.flags & TAG_MASK;
		if (tag == DW_TAG_compile_unit) {
			if (depth == 0 && die.stmt_list != SIZE_MAX &&
			    (err = read_file_name_table(dindex, cu,
							die.stmt_list,
							&file_name_table)))
				goto out;
		} else if (tag && !(die.flags & TAG_FLAG_DECLARATION)) {
			uint64_t file_name_hash;

			/*
			 * NB: the enumerator name points to the
			 * enumeration_type DIE instead of the enumerator DIE.
			 */
			if (depth == 1 && tag == DW_TAG_enumeration_type)
				enum_die_offset = die_offset;
			else if (depth == 2 && tag == DW_TAG_enumerator &&
				 enum_die_offset)
				die_offset = enum_die_offset;
			else if (depth != 1)
				goto next;

			if (die.specification && (!die.name || !die.decl_file)) {
				struct die decl = {};
				const char *decl_ptr = die.specification;

				if ((err = read_die(cu, &abbrev_table, &decl_ptr, end,
						    debug_str_buffer, debug_str_end,
						    &decl)))
					goto out;
				if (!die.name && decl.name)
					die.name = decl.name;
				if (!die.decl_file && decl.decl_file)
					die.decl_file = decl.decl_file;
			}

			if (die.name) {
				if (die.decl_file > file_name_table.num_files) {
					err = drgn_error_format(DRGN_ERROR_DWARF_FORMAT,
								"invalid DW_AT_decl_file %zu",
								die.decl_file);
					goto out;
				}
				if (die.decl_file)
					file_name_hash = file_name_table.file_name_hashes[die.decl_file - 1];
				else
					file_name_hash = 0;
				if ((err = index_die(dindex, die.name, tag,
						     file_name_hash, file,
						     die_offset)))
					goto out;
			}
		}

next:
		if (die.flags & TAG_FLAG_CHILDREN) {
			if (die.sibling)
				ptr = die.sibling;
			else
				depth++;
		} else if (depth == 0) {
			break;
		}
	}

	err = NULL;
out:
	free(file_name_table.file_name_hashes);
	free(abbrev_table.decls);
	free(abbrev_table.insns);
	return err;
}

static struct drgn_error *index_cus(struct drgn_dwarf_index *dindex,
				    struct compilation_unit *cus,
				    size_t num_cus)
{
	struct drgn_error *err = NULL;

#pragma omp parallel
	{
		size_t i;
		struct drgn_error *err2;

#pragma omp for schedule(dynamic)
		for (i = 0; i < num_cus; i++) {
			if (err)
				continue;
			if ((err2 = index_cu(dindex, &cus[i])))
#pragma omp critical(cus_err)
			{
				if (err)
					drgn_error_destroy(err2);
				else
					err = err2;
			}
		}
	}

	return err;
}

static void unindex_files(struct drgn_dwarf_index *dindex,
			  struct debug_file *files)
{
	struct debug_file *file;
	size_t i;

	/* First, mark all of the files that failed. */
	file = files;
	do {
		file->failed = true;
		file = file->next;
	} while (file);

	/* Then, delete all of the entries pointing to those files. */
	for (i = 0; i < ARRAY_SIZE(dindex->shards); i++) {
		struct dwarf_index_shard *shard = &dindex->shards[i];
		struct die_map_pos pos;

		/*
		 * Because we're deleting everything that was added since the
		 * last update, we can just shrink the entries array to the
		 * first entry that was added for this update.
		 */
		while (shard->num_entries) {
			struct die_entry *entry;

			entry = &shard->entries[shard->num_entries - 1];
			if (entry->file->failed)
				shard->num_entries--;
			else
				break;
		}

		/*
		 * We also need to delete those entries in the map. Note that
		 * any entries chained on the entries we delete must have also
		 * been added for this update, so there's no need to preserve
		 * them.
		 */
		pos = die_map_first_pos(&shard->map);
		while (pos.item) {
			if (pos.item->value >= shard->num_entries)
				die_map_delete(&shard->map, &pos.item->key);
			die_map_next_pos(&pos);
		}
	}
}

struct drgn_error *drgn_dwarf_index_update(struct drgn_dwarf_index *dindex)
{
	struct drgn_error *err;
	struct debug_file *first, *last, *file;
	struct compilation_unit *cus = NULL;
	size_t num_cus = 0, cus_capacity = 0;

	first = dindex->opened_first;
	last = dindex->opened_last;
	dindex->opened_first = dindex->opened_last = NULL;
	if (!first)
		return NULL;

	if ((err = apply_relocations(first)))
		goto out;

	file = first;
	do {
		Elf_Data *debug_str;

		debug_str = file->sections[SECTION_DEBUG_STR];
		if (debug_str->d_size == 0 ||
		    ((char *)debug_str->d_buf)[debug_str->d_size - 1] != '\0') {
			err = drgn_error_create(DRGN_ERROR_DWARF_FORMAT,
						".debug_str is not null terminated");
			goto out;
		}

		if ((err = read_cus(file, &cus, &num_cus, &cus_capacity)))
			goto out;
		file = file->next;
	} while (file);

	if ((err = index_cus(dindex, cus, num_cus))) {
		unindex_files(dindex, first);
		goto out;
	}

	if (dindex->indexed_last)
		dindex->indexed_last->next = first;
	else
		dindex->indexed_first = first;
	dindex->indexed_last = last;
	first = NULL;

out:
	free_files(dindex, first);
	free(cus);
	return err;
}

uint8_t drgn_dwarf_index_word_size(struct drgn_dwarf_index *dindex)
{
	if (!dindex->indexed_first)
		return sizeof(void *);
	return elf_word_size(dindex->indexed_first->elf);
}

bool drgn_dwarf_index_is_little_endian(struct drgn_dwarf_index *dindex)
{
	if (!dindex->indexed_first)
		return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
	return elf_is_little_endian(dindex->indexed_first->elf);
}

void drgn_dwarf_index_iterator_init(struct drgn_dwarf_index_iterator *it,
				    struct drgn_dwarf_index *dindex,
				    const char *name, size_t name_len,
				    const uint64_t *tags, size_t num_tags)
{
	it->dindex = dindex;
	if (name) {
		struct string key = {
			.str = name,
			.len = name_len,
		};
		struct hash_pair hp;
		struct dwarf_index_shard *shard;
		size_t *value;

		hp = die_map_hash(&key);
		it->shard = hash_pair_to_shard(hp);
		shard = &dindex->shards[it->shard];
		value = die_map_search_hashed(&shard->map, &key, hp);
		it->index = value ? *value : SIZE_MAX;
		it->any_name = false;
	} else {
		it->index = 0;
		for (it->shard = 0; it->shard < ARRAY_SIZE(dindex->shards);
		     it->shard++) {
			if (dindex->shards[it->shard].num_entries)
				break;
		}
		it->any_name = true;
	}
	it->tags = tags;
	it->num_tags = num_tags;
}

static inline bool
drgn_dwarf_index_iterator_matches_tag(struct drgn_dwarf_index_iterator *it,
				      struct die_entry *entry)
{
	size_t i;

	if (it->num_tags == 0)
		return true;
	for (i = 0; i < it->num_tags; i++) {
		if (entry->tag == it->tags[i])
			return true;
	}
	return false;
}

struct drgn_error *
drgn_dwarf_index_iterator_next(struct drgn_dwarf_index_iterator *it,
			       Dwarf_Die *die)
{
	struct drgn_dwarf_index *dindex = it->dindex;
	struct die_entry *entry;
	struct debug_file *file;

	if (it->any_name) {
		for (;;) {
			struct dwarf_index_shard *shard;

			if (it->shard >= ARRAY_SIZE(dindex->shards))
				return &drgn_stop;

			shard = &dindex->shards[it->shard];
			entry = &shard->entries[it->index];

			if (++it->index >= shard->num_entries) {
				it->index = 0;
				while (++it->shard < ARRAY_SIZE(dindex->shards)) {
					if (dindex->shards[it->shard].num_entries)
						break;
				}
			}

			if (drgn_dwarf_index_iterator_matches_tag(it, entry))
				break;
		}
	} else {
		for (;;) {
			struct dwarf_index_shard *shard;

			if (it->index == SIZE_MAX)
				return &drgn_stop;

			shard = &dindex->shards[it->shard];
			entry = &shard->entries[it->index];

			it->index = entry->next;

			if (drgn_dwarf_index_iterator_matches_tag(it, entry))
				break;
		}
	}

	file = entry->file;
	if (!file->dwarf) {
		file->dwarf = dwarf_begin_elf(file->elf,
					      DWARF_C_READ,
					      NULL);
		if (!file->dwarf)
			return drgn_error_libdw();
	}
	if (!dwarf_offdie(file->dwarf, entry->offset, die))
		return drgn_error_libdw();
	return NULL;
}
