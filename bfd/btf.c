/* btf.c -- display BTF contents of a BFD binary file
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
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

#include "sysdep.h"
#include "bfd.h"
#include "libbfd.h"
#include "btf.h"

struct btf_header *
btf_read_header (bfd *abfd, bfd_byte *buf)
{
  struct btf_header *header = bfd_zmalloc (sizeof (struct btf_header));

  header->magic = bfd_get_16 (abfd, buf);
  buf += 2;
  header->version = bfd_get_8 (abfd, buf);
  buf += 1;
  header->flags = bfd_get_8 (abfd, buf);
  buf += 1;
  header->hdr_len = bfd_get_32 (abfd, buf);
  buf += 4;
  header->entry_off = bfd_get_32 (abfd, buf);
  buf += 4;
  header->entry_len = bfd_get_32 (abfd, buf);
  buf += 4;
  header->str_off = bfd_get_32 (abfd, buf);
  buf += 4;
  header->str_len = bfd_get_32 (abfd, buf);

  return header;
}

struct btf_entry *
btf_read_entry (bfd *abfd, bfd_byte *buf)
{
  struct btf_entry *entry = bfd_zmalloc (sizeof (struct btf_entry));

  entry->name = bfd_get_32 (abfd, buf);
  buf += 4;
  entry->info = bfd_get_32 (abfd, buf);
  buf += 4;
  entry->size_or_entry_id = bfd_get_32 (abfd, buf);

  return entry;
}

struct btf_member *
btf_read_member (bfd *abfd, bfd_byte *buf)
{
  struct btf_member *member = bfd_zmalloc (sizeof (struct btf_member));

  member->name = bfd_get_32 (abfd, buf);
  buf += 4;
  member->entry_id = bfd_get_32 (abfd, buf);
  buf += 4;
  member->offset = bfd_get_32 (abfd, buf);

  return member;
}

struct btf_decl_tag *
btf_read_decl_tag (bfd *abfd, bfd_byte *buf)
{
  struct btf_decl_tag *decl_tag = bfd_zmalloc (sizeof (struct btf_decl_tag));

  decl_tag->component_idx = bfd_get_32 (abfd, buf);
  return decl_tag;
}

struct btf_var_secinfo *
btf_read_var_secinfo (bfd *abfd, bfd_byte *buf)
{
  struct btf_var_secinfo *var_secinfo = bfd_zmalloc (sizeof (struct btf_var_secinfo));

  var_secinfo->var_type = bfd_get_32 (abfd, buf);
  buf += 4;
  var_secinfo->offset = bfd_get_32 (abfd, buf);
  buf += 4;
  var_secinfo->size = bfd_get_32 (abfd, buf);

  return var_secinfo;
}

struct btf_var *
btf_read_var (bfd *abfd, bfd_byte *buf)
{
  struct btf_var *var = bfd_zmalloc (sizeof (struct btf_var));

  var->linkage = bfd_get_32 (abfd, buf);
  return var;
}

struct btf_param *
btf_read_param (bfd *abfd, bfd_byte *buf)
{
  struct btf_param *param = bfd_zmalloc (sizeof (struct btf_param));

  param->name = bfd_get_32 (abfd, buf);
  buf += 4;
  param->param_type = bfd_get_32 (abfd, buf);

  return param;
}

struct btf_enum64 *
btf_read_enum64 (bfd *abfd, bfd_byte *buf)
{
  struct btf_enum64 *enum64 = bfd_zmalloc (sizeof (struct btf_enum64));

  enum64->name = bfd_get_32 (abfd, buf);
  buf += 4;
  enum64->val_lo32 = bfd_get_32 (abfd, buf);
  buf += 4;
  enum64->val_hi32 = bfd_get_32 (abfd, buf);

  return enum64;
}

struct btf_enum *
btf_read_enum (bfd *abfd, bfd_byte *buf)
{
  struct btf_enum *anenum = bfd_zmalloc (sizeof (struct btf_enum));

  anenum->name = bfd_get_32 (abfd, buf);
  buf += 4;
  anenum->val = bfd_get_32 (abfd, buf);

  return anenum;
}

struct btf_array *
btf_read_array (bfd *abfd, bfd_byte *buf)
{
  struct btf_array *array = bfd_zmalloc (sizeof (struct btf_array));

  array->elem_type = bfd_get_32 (abfd, buf);
  buf += 4;
  array->index_type = bfd_get_32 (abfd, buf);
  buf += 4;
  array->nelems = bfd_get_32 (abfd, buf);

  return array;
}

struct btf_integral *
btf_read_integral (bfd *abfd, bfd_byte *buf)
{
  struct btf_integral *integral = bfd_zmalloc (sizeof (struct btf_integral));

  integral->info = bfd_get_32 (abfd, buf);
  return integral;
}

bfd_vma
btf_entry_size (struct btf_entry *entry)
{
  bfd_vma size = sizeof (struct btf_entry);
  uint16_t vlen = BTF_ENTRY_INFO_VLEN (entry->info);

  switch (BTF_ENTRY_INFO_KIND (entry->info))
    {
    case BTF_KIND_INT:
      size += 4;
      break;
    case BTF_KIND_ARRAY:
      size += sizeof (struct btf_array);
      break;
    case BTF_KIND_ENUM:
      size += sizeof (struct btf_enum) * vlen;
      break;
    case BTF_KIND_ENUM64:
      size += sizeof (struct btf_enum64) * vlen;
      break;
    case BTF_KIND_FUNC_PROTO:
      size += sizeof (struct btf_param) * vlen;
      break;
    case BTF_KIND_VAR:
      size += sizeof (struct btf_var);
      break;
    case BTF_KIND_UNION:
      /* Fallthrough.  */
    case BTF_KIND_STRUCT:
      size += sizeof (struct btf_member) * vlen;
      break;
    case BTF_KIND_DATASEC:
      size += sizeof (struct btf_var_secinfo) * vlen;
      break;
    case BTF_KIND_DECL_TAG:
      size += sizeof (struct btf_decl_tag);
      break;
    default:
      /* No entry specific additional data follows this entry.  */
      break;
    }

  return size;
}

bool
btf_map (bfd *abfd, asection *sec, btf_entry_map_cb cb)
{
  bfd_byte *btfdata = NULL;
  struct btf_header *header = NULL;
  bfd_size_type section_size = bfd_section_size (sec);

  if (!bfd_malloc_and_get_section (abfd, sec, &btfdata))
    goto error;

  header = btf_read_header (abfd, btfdata);
  if (header->magic != 0xeb9f)
    goto error;

  if (header->version != 1)
    goto error;

  /* Make sure the BTF regions specified by the header (entries and
     string table) fall within the section.  A corrupted header may
     lead to a buffer overflow below.  */

  if (header->hdr_len + header->str_off + header->str_len > section_size
      || header->hdr_len + header->entry_off + header->entry_len > section_size)
    goto error;

  /* Iterate over all the entries in the section and invoke the user
     provided callback.  */
  {
    bfd_byte *str_base = btfdata + header->hdr_len + header->str_off;
    bfd_byte *entry_base = btfdata + header->hdr_len + header->entry_off;
    bfd_byte *entry_off = entry_base;
    btf_entry_id entry_id = 1;

    while (entry_off < entry_base + header->entry_len)
      {
        struct btf_entry *entry = btf_read_entry (abfd, entry_off);

        (*cb) (abfd, btfdata,
               str_base - btfdata,
               entry_off - btfdata,
               entry_id++, entry);
        entry_off += btf_entry_size (entry);
        free (entry);
      }
  }

  free (btfdata);
  return true;

 error:
  free (btfdata);
  return false;
}
