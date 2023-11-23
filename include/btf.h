/* btf.h - BTF support header file
   Copyright (C) 2023 Free Software Foundation, Inc.

   Contributed by Oracle Inc.

   This file is part of GNU Binutils.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
   MA 02110-1301, USA.  */

#ifndef BTF_H
#define BTF_H

#include <stdint.h>
#include "bfd.h"

/* BTF entries are characterized by an "entry ID", which is an
   unsigned 32-bit number.  */

typedef uint32_t btf_entry_id;

/* BTF integral type.

   INFO is an unsigned 32-bit number with the following fields:

       unused:4 encoding:4 offset:8 unused:8 bits:8

   Where the `encoding' is itself decomposed in:

       unused:1 bool_p:1 char_p:1 signed_p:1

   The macros below provide convenient access to extract these
   values.  */

#define BTF_INT_ENCODING(INFO) (((INFO) >> 24) & 0xf)
#define BTF_INT_OFFSET(INFO) (((INFO) >> 16) & 0xff)
#define BTF_INT_BITS(INFO) ((INFO) & 0xff)

#define BTF_INT_ENCODING_BOOL_P(ENC) (((ENC) >> 2) & 0x1)
#define BTF_INT_ENCODING_CHAR_P(ENC) (((ENC) >> 1) & 0x1)
#define BTF_INT_ENCODING_SIGNED_P(ENC) ((ENC) & 0x1)

struct btf_integral
{
  uint32_t info;
};

/* BTF array.

   ELEM_TYPE is the type ID of the type of the elements in the array.

   INDEX_TYPE is the type ID of the type of the array index.

   NELEMS is the number of elements stored in the array.  XXX how to
   denote variable-length arrays?  */

struct btf_array
{
  btf_entry_id elem_type;
  btf_entry_id index_type;
  uint32_t nelems;
};

/* BTF 32-bit enumerated value.

   NAME is the byte offset into the BTF string table where the name of
   the enumerated value is located.

   VAL is the 32-bit value of the enumerated value.  */

struct btf_enum
{
  uint32_t name;
  int32_t val;
};

/* BTF 64-bit enumerated value.

   NAME is the byte offset into the BTF string table where the name of
   the enumerated value is located.

   VAL_LO32 contains the low 32 bits of the 64-bit value of the
   enumerated value.

   VAL_HI32 contains the high 32 bits of the 64-bit value of the
   enumerated value.

   A convenience macro is provided to easily assemble the 64-bit
   value.  */


struct btf_enum64
{
  uint32_t name;
  uint32_t val_lo32;
  int32_t val_hi32;
};

#define BTF_ENUM64_VALUE(ENUM64) \
  ((int64_t) ((((uint64_t) (ENUM64)->val_hi32) << 32) | (ENUM64)->val_lo32))

/* BTF function parameter.

   NAME is the byte offset into the BTF string table where the name of
   the parameter is located.

   PARAM_TYPE is the type ID of the parameter.  */

struct btf_param
{
  uint32_t name;
  btf_entry_id param_type;
};

/* BTF variable.

   LINKAGE denotes the linkage of the variable.  It must be one of the
   BTF_VAR_* values defined below.  */

#define BTF_VAR_STATIC 0
#define BTF_VAR_GLOBAL_ALLOCATED 1
#define BTF_VAR_GLOBAL_EXTERN 2

struct btf_var
{
  uint32_t linkage;
};

/* Unlike variable entries, function entries do not add any extra info
   to the btf_entry, but the linkage of the function is encoded in the
   INFO_VLEN field of the btf_entry using the values defined
   below.  */

#define BTF_FUNC_STATIC 0
#define BTF_FUNC_GLOBAL 1
#define BTF_FUNC_EXTERN 2

/* BTF variable.

   VAR_TYPE is the type ID of the variable.
   OFFSET is the offset of the variable into the section.
   SIZE is the size of the variable, measured in bytes.  */

struct btf_var_secinfo
{
  btf_entry_id var_type;
  uint32_t offset;
  uint32_t size;
};

/* BTF declaration tag.

   COMPONENT_ID is -1 if the tag is applied to the type itself.
   Otherwise it is the type ID of the tagged field/parameter/etc.  */

#define BTF_DECL_TAG_SELF -1

struct btf_decl_tag
{
  int32_t component_idx;
};

/* BTF member (of struct or union.)

   NAME is the byte offset into the BTF string table where the name of
   the member is located.

   OFFSET denotes the offset of the member in the containing
   structure.  If the INFO_KIND_FLAG of the entry is 1 then this
   member is a bitfield and OFFSET contains the size of the bitfield
   and its offset in the containing integral value.  Otherwise OFFSET
   is the offset of the member in bits from the beginning of the
   containing structure.  */

#define BTF_MEMBER_BITFIELD_SIZE(OFFSET) (((OFFSET) >> 24) & 0xff)
#define BTF_MEMBER_BITFIELD_OFFSET(OFFSET) ((OFFSET) & 0xffffff)

struct btf_member
{
  uint32_t name;
  btf_entry_id entry_id;
  uint32_t offset;
};

/* BTF entry.

   NAME is the byte offset into the BTF string table where the name of
   the entry is stored.  The meaning of this name depends on the kind
   of entry: the name of a type, the name of a function or variable,
   etc.  Some entries may have no name.  In function prototypes, this
   field is zero.

   INFO is an unsigned 32-bit integer encoding several fields with
   information about the entry:

      kind_flag:1 unused:2 kind:5 unused:8 vlen:16

   SIZE_OR_ENTRY_ID can be either the size of the entry in bytes, or a
   type ID.  Its interpretation depends on the particular kind of
   entry:

     In BTF_KIND_FUNC this is the entry ID of the corresponding
     BTF_KIND_FUNC_PROTO.

   The btf_entry is immediately followed by additional data, depending
   on the kind of entry:

   BTF_KIND_INT        -> btf_integral
   BTF_KIND_ARRAY      -> btf_array
   BTF_KIND_ENUM       -> btf_enum[VLEN]
   BTF_KIND_ENUM64     -> btf_enum64[VLEN]
   BTF_KIND_FUNC_PROTO -> btf_param[VLEN]
   BTF_KIND_VAR        -> btf_var
   BTF_KIND_UNION      -> btf_member[VLEN]
   BTF_KIND_STRUCT     -> btf_member[VLEN]
   BTF_KIND_DATASEC    -> btf_var_secinfo[VLEN]
   BTF_DECL_TAG        -> btf_decl_tag
*/

#define BTF_KIND_UNKNOWN 0
#define BTF_KIND_INT 1
#define BTF_KIND_PTR 2
#define BTF_KIND_ARRAY 3
#define BTF_KIND_STRUCT 4
#define BTF_KIND_UNION 5
#define BTF_KIND_ENUM 6
#define BTF_KIND_FWD 7
#define BTF_KIND_TYPEDEF 8
#define BTF_KIND_VOLATILE 9
#define BTF_KIND_CONST 10
#define BTF_KIND_RESTRICT 11
#define BTF_KIND_FUNC 12
#define BTF_KIND_FUNC_PROTO 13
#define BTF_KIND_VAR 14
#define BTF_KIND_DATASEC 15
#define BTF_KIND_FLOAT 16
#define BTF_KIND_DECL_TAG 17
#define BTF_KIND_TYPE_TAG 18
#define BTF_KIND_ENUM64 19

#define BTF_ENTRY_INFO_KIND_FLAG(INFO) (((INFO) >> 31) & 0x1)
#define BTF_ENTRY_INFO_KIND(INFO) (((INFO) >> 24) & 0x1f)
#define BTF_ENTRY_INFO_VLEN(INFO) ((INFO) & 0xffff)

struct btf_entry
{
  uint32_t name;
  uint32_t info;
  uint32_t size_or_entry_id;
};

/* BTF header.

   MAGIC shall be 0xEB9F in big-endian and 0x9FEB in little-endian.

   VERSION is not used as far as I know (but it better starts to be
   used.)

   FLAGS is not used as far as I know.

   HDR_LEN is the size in bytes of this header.

   ENTRY_OFF is the offset in bytes from the end of the header to the
   BTF entries.

   STR_OFF is the offset in bytes from the end of the header to the
   BTF string table.

   ENTRY_LEN is the size in bytes of the BTF entries.
   STR_LEN is the size in bytes of the BTF string table.  */

struct btf_header
{
  uint16_t magic;
  uint8_t version;
  uint8_t flags;
  uint32_t hdr_len;
  uint32_t entry_off;
  uint32_t entry_len;
  uint32_t str_off;
  uint32_t str_len;
};

/* Type for callback to be passed to btf_map below.

   ABFD is the bfd containing the BTF section.
   BUF is a pointer to the BTF section content.
   STRTAB_OFFSET is the offset of the BTF string table in BUF.
   ENTRY_OFFSET is the offset of ENTRY in BUF.
   ENTRY_ID is the ID of the entry being handled.
   ENTRY is the entry to be handled.  */

typedef void (*btf_entry_map_cb) (bfd *abfd, bfd_byte *buf,
                                  bfd_vma strtab_offset, bfd_vma entry_offset,
                                  btf_entry_id entry_id, struct btf_entry *entry);

/* Iterate over the entries in a BTF section.

   ABFD is the bfd containing the BTF section.
   SEC is the section containing BTF data.

   This function returns `false' if the section doesn't contain valid
   BTF data or if there is an out of memory condition.  */

bool btf_map (bfd *abfd, asection *sec, btf_entry_map_cb cb);

/* Get the total size in bytes of a given BTF entry, including its
   payload.  */

bfd_vma btf_entry_size (struct btf_entry *entry);

/* Decoding functions.  The functions returning pointers to structures
   allocate memory, and it is up to the user to free that memory when
   no longer used. */

struct btf_header *btf_read_header (bfd *abfd, bfd_byte *buf);
struct btf_entry *btf_read_entry (bfd *abfd, bfd_byte *buf);
struct btf_member *btf_read_member (bfd *abfd, bfd_byte *buf);
struct btf_decl_tag *btf_read_decl_tag (bfd *abfd, bfd_byte *buf);
struct btf_var_secinfo *btf_read_var_secinfo (bfd *abfd, bfd_byte *buf);
struct btf_var *btf_read_var (bfd *abfd, bfd_byte *buf);
struct btf_param *btf_read_param (bfd *abfd, bfd_byte *buf);
struct btf_enum64 *btf_read_enum64 (bfd *abfd, bfd_byte *buf);
struct btf_enum *btf_read_enum (bfd *abdf, bfd_byte *buf);
struct btf_array *btf_read_array (bfd *abfd, bfd_byte *buf);
struct btf_integral *btf_read_integral (bfd *abfd, bfd_byte *buf);

#endif /* ! BTF_H */
