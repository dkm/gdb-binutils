/* Target-dependent code for the Kalray K1 for GDB, the GNU debugger.

   Copyright (C) 2010 Kalray

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "arch-utils.h"
#include "cli/cli-decode.h"
#include "dis-asm.h"
#include "dwarf2-frame.h"
#include "frame-unwind.h"
#include "gdbcmd.h"
#include "gdbcore.h"
#include "gdbtypes.h"
#include "inferior.h"
#include "objfiles.h"
#include "observer.h"
#include "regcache.h"
#include "symtab.h"
#include "target.h"
#include "target-descriptions.h"
#include "user-regs.h"
#include "opcode/k1.h"

struct gdbarch_tdep {
    int ls_regnum;
    int le_regnum;
    int lc_regnum;
    int ps_regnum;
    int ra_regnum;
    int ev_regnum;
    int spc_regnum;

    int local_regnum;
};

struct k1_frame_cache
{
    /* Base address.  */
    CORE_ADDR base;
    CORE_ADDR pc;
    LONGEST framesize;
    //  CORE_ADDR saved_regs[];
    CORE_ADDR saved_sp;
};

extern const char *k1_pseudo_register_name (struct gdbarch *gdbarch,
					    int regnr);
extern struct type *k1_pseudo_register_type (struct gdbarch *gdbarch, 
					     int reg_nr);
extern int k1_pseudo_register_reggroup_p (struct gdbarch *gdbarch, 
					  int regnum, 
					  struct reggroup *reggroup);

extern void k1_pseudo_register_read (struct gdbarch *gdbarch, 
				     struct regcache *regcache,
				     int regnum, gdb_byte *buf);

extern void k1_pseudo_register_write (struct gdbarch *gdbarch, 
				      struct regcache *regcache,
				      int regnum, const gdb_byte *buf);

extern int k1_dwarf2_reg_to_regnum (struct gdbarch *gdbarch, int reg);

extern const int k1_num_pseudo_regs;
extern const char *k1_pc_name;
extern const char *k1_sp_name;

struct op_list {
    k1opc_t        *op;
    struct op_list *next;
};

static struct op_list *sp_adjust_insns;
static struct op_list *sp_store_insns;
static struct op_list *prologue_helper_insns;

const char *
k1_dummy_register_name (struct gdbarch *gdbarch, int regno)
{
    return "";
}

struct type *
k1_dummy_register_type (struct gdbarch *gdbarch, int regno)
{
    return builtin_type (gdbarch)->builtin_int;
}

static CORE_ADDR k1_fetch_tls_load_module_address (struct objfile *objfile)
{
    struct regcache *regs = get_current_regcache ();
    ULONGEST val;

    regcache_raw_read_unsigned (regs,
				gdbarch_tdep (target_gdbarch)->local_regnum, 
				&val);
    return val;
}


static const gdb_byte *
k1_breakpoint_from_pc (struct gdbarch *gdbarch, CORE_ADDR *pc, int *len)
{
    static const gdb_byte BREAK[4] = { 0, 0, 0x0c, 0 };
    *len = 4;
    return BREAK;
}

static CORE_ADDR
k1_adjust_breakpoint_address (struct gdbarch *gdbarch, CORE_ADDR bpaddr)
{
  gdb_byte syllab_buf[4];
  uint32_t syllab;
  enum bfd_endian order = gdbarch_byte_order_for_code (gdbarch);
  CORE_ADDR adjusted = bpaddr;
  int i = 0;

  /* Look for the end of the bundle preceeding the requested address. */
  do {
    if (target_read_memory (adjusted - 4, syllab_buf, 4) != 0)
      return adjusted;
    
    syllab = extract_unsigned_integer (syllab_buf, 4, order);
  } while ((i++ < 7) && (syllab >> 31) && (adjusted -= 4));

  if (i == 8) /* Impossible for correct instructions */
      return bpaddr;

  return adjusted;
}

static int k1_has_create_stack_frame (struct gdbarch *gdbarch, CORE_ADDR addr)
{
    gdb_byte syllab_buf[4];
    uint32_t syllab;
    enum bfd_endian order = gdbarch_byte_order_for_code (gdbarch);
    k1bfield *add_reg_desc, *sbf_reg_desc;
    int reg;
    int i = 0, ops_idx, has_make = 0;
    struct op_list *ops;
    k1_bitfield_t *bfield;

    struct {
	struct op_list *ops;
	int sp_idx;
    } prologue_insns[] = {
	{ sp_adjust_insns, 0 /* Dest register */},
	{ sp_store_insns, 1 /* Base register */},
	{ prologue_helper_insns, -1 /* unused */},
    };

    do {
    next_addr:
	if (target_read_memory (addr, syllab_buf, 4) != 0)
	    return 0;
	syllab = extract_unsigned_integer (syllab_buf, 4, order);

	for (ops_idx = 0; ops_idx < ARRAY_SIZE(prologue_insns); ++ops_idx) {
	    ops = prologue_insns[ops_idx].ops;
	    while (ops) {
		k1opc_t *op = ops->op;
		if ((syllab & op->codeword[0].mask) != op->codeword[0].opcode)
		    goto next;

		if (strcmp (op->as_op, "make") == 0) {
		    if (i == 0) has_make = 1; else return 0;
		} else if (has_make && i == 1) {
		    if (strcmp (op->as_op, "sbf")) return 0;
		} else if (has_make) {
		    return 0;
		}

		if (prologue_insns[ops_idx].sp_idx < 0) {
		    addr += op->coding_size/8;
		    ++i;
		    goto next_addr;
		}

		bfield = &op->format[prologue_insns[ops_idx].sp_idx]->bfield[0];
		reg = (syllab >> bfield->to_offset) & ((1 << bfield->size) - 1);
		if (reg == 12) return 1;
	    next:
		ops = ops->next;
	    }
	}
    } while (0);
    
    return 0;
}

/* Return PC of first real instruction.  */
/* This function is nearly cmopletely copied from
   symtab.c:skip_prologue_using_lineinfo (excpet the test in the loop
   at the end. */
static CORE_ADDR
k1_skip_prologue (struct gdbarch *gdbarch, CORE_ADDR func_addr)
{
    CORE_ADDR func_start, func_end;
    struct linetable *l;
    int ind, i, len;
    int best_lineno = 0;
    CORE_ADDR best_pc = func_addr;
    struct symtab_and_line sal;
    int gcc_compiled = 0;

    sal = find_pc_line (func_addr, 0);
    if (sal.symtab == NULL)
	return func_addr;

    if (sal.symtab->producer
	&& strncmp (sal.symtab->producer, "GNU C", 5) == 0)
	gcc_compiled = 1;

    /* Give up if this symbol has no lineinfo table.  */
    l = LINETABLE (sal.symtab);
    if (l == NULL)
	return func_addr;
    
    /* Get the range for the function's PC values, or give up if we
       cannot, for some reason.  */
    if (!find_pc_partial_function (func_addr, NULL, &func_start, &func_end))
	return func_addr;
    
     if (!gcc_compiled && !k1_has_create_stack_frame (gdbarch, func_addr))
     	return func_addr;

    /* Linetable entries are ordered by PC values, see the commentary in
       symtab.h where `struct linetable' is defined.  Thus, the first
       entry whose PC is in the range [FUNC_START..FUNC_END[ is the
       address we are looking for.  */
     if (gcc_compiled) {
	 for (i = 0; i < l->nitems; i++)
	     {
		 struct linetable_entry *item = &(l->item[i]);
		 
		 if (item->line > 0 && func_start <= item->pc && item->pc < func_end)
		     if (ind == 0)
			 ++ind;
		     else
			 return item->pc;
	     }
     } else {
	 for (i = 0; i < l->nitems; i++)
	     {
		 struct linetable_entry *item = &(l->item[i]);
		 
		 /* Don't use line numbers of zero, they mark special entries in
		    the table.  See the commentary on symtab.h before the
	       definition of struct linetable.  */
		 if (item->line > 0 && func_start < item->pc && item->pc < func_end)
		     return item->pc;
	     }
     }
    
    return func_addr;
}

struct displaced_step_closure {
    /* Take into account that ALUs might have extensions. */
    uint32_t insn_words[K1MAXBUNDLESIZE*2];
    int num_insn_words;
    
    uint32_t rewrite_LE;
};

static CORE_ADDR k1_step_pad_address;

static void
k1_inferior_created (struct target_ops *target, int from_tty)
{
    k1_step_pad_address = 0;
}

static CORE_ADDR
k1_displaced_step_location (struct gdbarch *gdbarch)
{
    if (!k1_step_pad_address) {
	struct minimal_symbol *msym = lookup_minimal_symbol ("_debug_start", 
							     NULL, NULL);
	if (msym == NULL)
	    error ("Can not locate a suitable step pad area.");
	k1_step_pad_address = SYMBOL_VALUE_ADDRESS(msym);;
    }

    return k1_step_pad_address;
}

static void
patch_bcu_instruction (struct gdbarch *gdbarch, 
		       CORE_ADDR from, CORE_ADDR to, struct regcache *regs,
		       struct displaced_step_closure *dsc)
{
    if (debug_displaced)
	printf_filtered ("displaced: Looking at BCU instruction\n");

    if (((dsc->insn_words[0] >> 27) & 0x3) == 0x2 /* CALL */
	|| ((dsc->insn_words[0] >> 27) & 0x3) == 0x1 /* GOTO */) {
	int displacement = dsc->insn_words[0] << 5;
	
	if (debug_displaced) printf_filtered ("displaced: CALL/GOTO\n");

	displacement >>= 3;
	displacement = from + displacement - to;
	displacement >>= 2;
	dsc->insn_words[0] &= 0Xf8000000;
	dsc->insn_words[0] |= (displacement & ~0Xf8000000);
    } else if (((dsc->insn_words[0] >> 27) & 0x3) == 0x3 /* CB */) {
	int off = ((dsc->insn_words[0] >> 6) & ((1<<18)-1)) << 2;
	off = from + off - to;
	if (off >= (1 << 20))
	    error ("displacement of conditional branch too big");
	dsc->insn_words[0] &= 0xff00003f;
	off &= (1<<20)-1;
	dsc->insn_words[0] |= (off << 4); /* Shift by 4 instead of 6
					     as the value has to be scaled by 4 */
	if (debug_displaced) printf_filtered ("displaced: CB\n");
    } else if (((dsc->insn_words[0] >> 23) & 0xff) == 0x6 /* LOOPGTZ */
	       || ((dsc->insn_words[0] >> 23) & 0xff) == 0x4 /* LOOPNEZ */
	       || ((dsc->insn_words[0] >> 23) & 0xff) == 0x2 /* LOOPDO */) {
	int off = ((dsc->insn_words[0] >> 6) & ((1<<18)-1)) << 2;
	dsc->rewrite_LE = from + off;
	if (debug_displaced) printf_filtered ("displaced: H/W LOOP setup\n");
    }
}

static struct displaced_step_closure *
k1_displaced_step_copy_insn (struct gdbarch *gdbarch, 
			     CORE_ADDR from, CORE_ADDR to, struct regcache *regs)
{
    struct displaced_step_closure *dsc
	= xzalloc (sizeof (struct displaced_step_closure));

    if (debug_displaced)
	printf_filtered ("displaced: copying from %s\n", paddress (gdbarch, from));

    do {
	read_memory (from + dsc->num_insn_words*4,
		     (gdb_byte*)(dsc->insn_words + dsc->num_insn_words), 4);
    } while (dsc->insn_words[dsc->num_insn_words++] & (1<<31));
    
    if (debug_displaced) {
	int i;
	printf_filtered ("displaced: copied a %i word(s)\n", dsc->num_insn_words);
	for (i = 0; i < dsc->num_insn_words; ++i)
	    printf_filtered ("displaced: insn[%i] = %08x\n", 
			     i, dsc->insn_words[i]);
    }

    dsc->insn_words[0] = extract_unsigned_integer ((const gdb_byte*)dsc->insn_words, 4, gdbarch_byte_order (gdbarch));
    if (((dsc->insn_words[0] >> 29) & 0x3) == 0)
	patch_bcu_instruction (gdbarch, from, to, regs, dsc);
    store_unsigned_integer ((gdb_byte*)dsc->insn_words, 4, gdbarch_byte_order (gdbarch), dsc->insn_words[0]);

    write_memory (to, (gdb_byte*)dsc->insn_words, dsc->num_insn_words*4);
    
    return dsc;
}

static void
k1_displaced_step_fixup (struct gdbarch *gdbarch, 
			 struct displaced_step_closure *dsc, 
			 CORE_ADDR from, CORE_ADDR to, struct regcache *regs)
{
    ULONGEST ps, lc, le, pc;
    struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
    /* FIXME: handle traps */

    if (debug_displaced) printf_filtered ("displaced: Fixup\n");

    regcache_raw_read_unsigned (regs, tdep->ps_regnum, &ps);    
    pc = regcache_read_pc (regs);
    if ((int)(pc - to) > 0) {
	pc = from + (pc - to);
	if (debug_displaced) printf_filtered ("displaced: Didn't branch\n");
    } else {
	ULONGEST ev, spc;
	/* We branched. */
	regcache_raw_read_unsigned (regs, tdep->ev_regnum, &ev);
	if (pc - ev > 0 && pc - ev <= 0xc) {
	    /* It was a trap */
	    regcache_raw_read_unsigned (regs, tdep->spc_regnum, &spc);
	    if (debug_displaced) printf_filtered ("displaced: trapped SPC=%llx\n", spc);
	    spc = from + (spc - k1_step_pad_address);
	    regcache_raw_write_unsigned (regs, tdep->spc_regnum, spc);
	} else if (((dsc->insn_words[0] >> 27) & 0x3) == 0x2) {
	    /* It was a call */
	    regcache_raw_write_unsigned (regs, tdep->ra_regnum, 
					 from + dsc->num_insn_words*4);
	    if (debug_displaced) printf_filtered ("displaced: call\n");
	}
    }

    if (dsc->rewrite_LE) {
	regcache_raw_write_unsigned (regs, tdep->ls_regnum,
				     from + dsc->num_insn_words*4);
	regcache_raw_write_unsigned (regs, tdep->le_regnum,
				     dsc->rewrite_LE);
	if (debug_displaced) printf_filtered ("displaced: rewrite LE\n");
    } else if (((ps >> 5)&1) /* HLE */) {
	if (debug_displaced) printf_filtered ("displaced: active loop\n");
	regcache_raw_read_unsigned (regs, tdep->le_regnum, &le);
	if (pc == le) {
	    if (debug_displaced) printf_filtered ("displaced: at loop end\n");
	    regcache_raw_read_unsigned (regs, tdep->lc_regnum, &lc);
	    if (lc - 1 == 0)
		regcache_raw_read_unsigned (regs, tdep->le_regnum, &pc);
	    else
		regcache_raw_read_unsigned (regs, tdep->ls_regnum, &pc);
	    if (lc != 0)
		regcache_raw_write_unsigned (regs, tdep->lc_regnum, lc - 1);
	}
    }
	
    regcache_write_pc (regs, pc);
    if (debug_displaced) 
	printf_filtered ("displaced: writing PC %s\n", paddress (gdbarch, pc));
}

static void 
k1_displaced_step_free_closure (struct gdbarch *gdbarch, 
				struct displaced_step_closure *closure)
{
    xfree (closure);
}

static CORE_ADDR
k1_unwind_pc (struct gdbarch *gdbarch, struct frame_info *next_frame)
{
    return frame_unwind_register_unsigned (next_frame, gdbarch_pc_regnum(gdbarch));
}

/* Given a GDB frame, determine the address of the calling function's
   frame.  This will be used to create a new GDB frame struct.  */

static void
k1_frame_this_id (struct frame_info *this_frame,
		  void **this_prologue_cache, struct frame_id *this_id)
{
    if (get_frame_func (this_frame) == get_frame_pc (this_frame)) {
	*this_id = frame_id_build (get_frame_sp (this_frame) - 16, 
				   get_frame_func (this_frame));
    }
    /* struct k1_frame_cache *cache = k1_frame_cache (this_frame, */
    /* 						   this_prologue_cache); */
    
    /* /\* This marks the outermost frame.  *\/ */
    /* if (cache->base == 0) */
    /* 	return; */
    
    /* *this_id = frame_id_build (cache->saved_sp, cache->pc); */
}

/* Get the value of register regnum in the previous stack frame.  */

static struct value *
k1_frame_prev_register (struct frame_info *this_frame,
			void **this_prologue_cache, int regnum)
{
    if (get_frame_func (this_frame) == get_frame_pc (this_frame)) {
	if (regnum == gdbarch_pc_regnum (get_frame_arch (this_frame))) {
	    return frame_unwind_got_register (this_frame,
					      regnum,
					      user_reg_map_name_to_regnum (get_frame_arch (this_frame), "ra", -1));
	}
	return frame_unwind_got_register (this_frame, regnum, regnum);
    }
    /* struct k1_frame_cache *cache = k1_frame_cache (this_frame, */
    /* 						   this_prologue_cache); */
    
    /* gdb_assert (regnum >= 0); */
    
    /* if (regnum == MOXIE_SP_REGNUM && cache->saved_sp) */
    /* 	return frame_unwind_got_constant (this_frame, regnum, cache->saved_sp); */
    
    /* if (regnum < MOXIE_NUM_REGS && cache->saved_regs[regnum] != REG_UNAVAIL) */
    /* 	return frame_unwind_got_memory (this_frame, regnum, */
    /* 					cache->saved_regs[regnum]); */
    
    /* return frame_unwind_got_register (this_frame, regnum, regnum); */

    return frame_unwind_got_optimized (this_frame, regnum);
}

static const struct frame_unwind k1_frame_unwind = {
    NORMAL_FRAME,
    k1_frame_this_id,
    k1_frame_prev_register,
    NULL,
    default_frame_sniffer
};

static void
k1_dwarf2_frame_init_reg (struct gdbarch *gdbarch, int regnum,
		   struct dwarf2_frame_state_reg *reg,
		   struct frame_info *this_frame)
{
    if (regnum == gdbarch_pc_regnum (gdbarch))
	reg->how = DWARF2_FRAME_REG_RA;
    else if (regnum == gdbarch_sp_regnum (gdbarch)) {
	reg->how = DWARF2_FRAME_REG_CFA_OFFSET;
	reg->loc.offset = -16; /* Scratch area */
    }
}

static int k1_print_insn (bfd_vma pc, disassemble_info *di)
{
    return print_insn_k1 (pc, di);
}

static CORE_ADDR
k1_frame_align (struct gdbarch *gdbarch, CORE_ADDR addr)
{
    return addr - addr % 8;
}

static struct frame_id
k1_dummy_id (struct gdbarch *gdbarch, struct frame_info *this_frame)
{
  CORE_ADDR sp = get_frame_register_unsigned (this_frame, 
					      gdbarch_sp_regnum (gdbarch));
  return frame_id_build (sp+16, get_frame_pc (this_frame));
}

static CORE_ADDR
k1_push_dummy_call (struct gdbarch *gdbarch,
		    struct value *function,
		    struct regcache *regcache,
		    CORE_ADDR bp_addr, int nargs,
		    struct value **args,
		    CORE_ADDR sp, int struct_return,
		    CORE_ADDR struct_addr)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  CORE_ADDR orig_sp = sp;
  int i, j;
  const gdb_byte *val;
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  gdb_byte *argslotsbuf = NULL;
  unsigned int argslotsnb = 0;
  int len;

  /* Allocate arguments to the virtual argument slots  */
  for (i = 0; i < nargs; i++)
    {
      int typelen = TYPE_LENGTH (value_enclosing_type (args[i]));
      int newslots = (typelen+3)/4;
      if (typelen > 4 && argslotsnb & 1) ++argslotsnb;
      argslotsbuf = xrealloc (argslotsbuf, (argslotsnb+newslots)*4);
      memset (&argslotsbuf[argslotsnb*4], 0, newslots*4);
      memcpy (&argslotsbuf[argslotsnb*4], value_contents (args[i]),
	      typelen);
	argslotsnb += newslots;
    }

  for (i = 0; i < argslotsnb; i++)
    regcache_cooked_write (regcache, i, &argslotsbuf[i*4]);

  sp = k1_frame_align (gdbarch, sp);
  len = argslotsnb - 8;
  if (len > 0) 
    {
      /* Align stack correctly and copy args there */
      if (argslotsnb & 0x1) sp -= 4;

      sp -= len*4;
      write_memory (sp, argslotsbuf + 8*4, len*4);
    }

  /* Scratch area */
  sp -= 16;

  if (struct_return)
    regcache_cooked_write_unsigned (regcache, 15, struct_addr);

  regcache_cooked_write_unsigned (regcache, gdbarch_sp_regnum (gdbarch),
				  sp);
  regcache_cooked_write_unsigned (regcache, tdep->ra_regnum, bp_addr);

  return sp+16;
}

static void k1_store_return_value (struct gdbarch *gdbarch,
				   struct type *type,
                                   struct regcache *regcache,
                                   const gdb_byte *buf)
{
    int len = TYPE_LENGTH (type);
    // FIXME: extract first arg id from MDS
    int i = 0;
    int sz = register_size (gdbarch, 0);
    
    while (len > sz) {
	regcache_raw_write (regcache, i, buf + i*sz);
	i++, len -= sz;
    }
    if (len > 0) {
	gdb_byte tmp[4] = {0};
	memcpy (tmp, buf + i*sz, len);
	regcache_raw_write (regcache, i, tmp);
    }
}

static void k1_extract_return_value (struct gdbarch *gdbarch,
				     struct type *type,
                                     struct regcache *regcache,
                                     gdb_byte *buf)
{
    int len = TYPE_LENGTH (type);
    // FIXME: extract first arg id from MDS
    int i = 0;
    int sz = register_size (gdbarch, 0);
    
    while (len > sz) {
	regcache_raw_read (regcache, i, buf + i*sz);
	i++, len -= sz;
    }
    if (len > 0) {
	gdb_byte tmp[4];
	regcache_raw_read (regcache, i, tmp);
	memcpy (buf+i*sz, tmp, len);
    }
}

static enum return_value_convention
k1_return_value (struct gdbarch *gdbarch, struct type *func_type,
                 struct type *type, struct regcache *regcache,
                 gdb_byte *readbuf, const gdb_byte *writebuf)
{
    if (TYPE_LENGTH (type) > 32)
        return RETURN_VALUE_STRUCT_CONVENTION;

    if (writebuf)
        k1_store_return_value (gdbarch, type, regcache, writebuf);
    else if (readbuf)
        k1_extract_return_value (gdbarch, type, regcache, readbuf);
    return RETURN_VALUE_REGISTER_CONVENTION;
}

int k1_get_longjmp_target (struct frame_info *frame, CORE_ADDR *pc)
{
    /* R0 point to the jmpbuf, and RA is at offset 0x34 in the buf */
    gdb_byte buf[4];
    CORE_ADDR r0;
    struct gdbarch *gdbarch = get_frame_arch (frame);
    enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

    get_frame_register (frame, 0, buf);
    r0 = extract_unsigned_integer (buf, 4, byte_order);
    
    if (target_read_memory (r0 + 0x34, buf, 4))
	return 0;

    *pc = extract_unsigned_integer (buf, 4, byte_order);
    return 1;
}


static struct gdbarch *
k1_gdbarch_init (struct gdbarch_info info, struct gdbarch_list *arches)
{
  struct gdbarch *gdbarch;
  struct gdbarch_tdep *tdep;
  const struct target_desc *tdesc;
  struct tdesc_arch_data *tdesc_data;
  int i;
  int has_pc = -1, has_sp = -1, has_le = -1, has_ls = -1, has_ps = -1, has_lc = -1, has_local = -1, has_ra = -1, has_ev = -1, has_spc = -1;

  static const char k1_lc_name[] = "lc";
  static const char k1_ls_name[] = "ls";
  static const char k1_le_name[] = "le";
  static const char k1_ps_name[] = "ps";
  static const char k1_ra_name[] = "ra";
  static const char k1_ev_name[] = "ev";
  static const char k1_spc_name[] = "spc";
  static const char k1_local_name[] = "r13";

  /* If there is already a candidate, use it.  */
  arches = gdbarch_list_lookup_by_info (arches, &info);
  if (arches != NULL)
    return arches->gdbarch;

  tdep = xzalloc (sizeof (struct gdbarch_tdep));
  gdbarch = gdbarch_alloc (&info, tdep);

  /* This could (should?) be extracted from MDS */
  set_gdbarch_short_bit (gdbarch, 16);
  set_gdbarch_int_bit (gdbarch, 32);
  set_gdbarch_long_bit (gdbarch, 32);
  set_gdbarch_long_long_bit (gdbarch, 64);
  set_gdbarch_float_bit (gdbarch, 32);
  set_gdbarch_double_bit (gdbarch, 64);
  set_gdbarch_long_double_bit (gdbarch, 64);
  set_gdbarch_ptr_bit (gdbarch, 32);

  /* Get the k1 target description from INFO.  */
  tdesc = info.target_desc;
  if (tdesc_has_registers (tdesc)) {
      set_gdbarch_num_regs (gdbarch, 0);
      tdesc_data = tdesc_data_alloc ();
      tdesc_use_registers (gdbarch, tdesc, tdesc_data);

      for (i = 0; i < gdbarch_num_regs (gdbarch); ++i)
	  if (strcmp (tdesc_register_name(gdbarch, i), k1_pc_name) == 0)
	      has_pc = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_sp_name) == 0)
	      has_sp = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_le_name) == 0)
	      has_le = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_ls_name) == 0)
	      has_ls = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_ps_name) == 0)
	      has_ps = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_lc_name) == 0)
	      has_lc = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_local_name) == 0)
	      has_local = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_ra_name) == 0)
	      has_ra = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_ev_name) == 0)
	      has_ev = i;
	  else if (strcmp (tdesc_register_name(gdbarch, i), k1_spc_name) == 0)
	      has_spc = i;
      
      if (has_pc < 0)
	  error ("There's no '%s' register!", k1_pc_name);
      if (has_sp < 0)
	  error ("There's no '%s' register!", k1_sp_name);
      if (has_le < 0)
	  error ("There's no '%s' register!", k1_le_name);
      if (has_ls < 0)
	  error ("There's no '%s' register!", k1_ls_name);
      if (has_lc < 0)
	  error ("There's no '%s' register!", k1_lc_name);
      if (has_ps < 0)
	  error ("There's no '%s' register!", k1_ps_name);
      if (has_local < 0)
	  error ("There's no '%s' register!", k1_local_name);
      if (has_ra < 0)
	  error ("There's no '%s' register!", k1_ra_name);
      if (has_ev < 0)
	  error ("There's no '%s' register!", k1_ev_name);
      if (has_spc < 0)
	  error ("There's no '%s' register!", k1_spc_name);

      tdep->le_regnum = has_le;
      tdep->ls_regnum = has_ls;
      tdep->lc_regnum = has_lc;
      tdep->ps_regnum = has_ps;
      tdep->ra_regnum = has_ra;
      tdep->ev_regnum = has_ev;
      tdep->spc_regnum = has_spc;
      tdep->local_regnum = has_local;
      set_gdbarch_pc_regnum (gdbarch, has_pc);
      set_gdbarch_sp_regnum (gdbarch, has_sp);
  } else {
      set_gdbarch_num_regs (gdbarch, 1);
      set_gdbarch_register_name (gdbarch, k1_dummy_register_name);
      set_gdbarch_register_type (gdbarch, k1_dummy_register_type);
  }

  set_gdbarch_num_pseudo_regs (gdbarch, k1_num_pseudo_regs);

  set_tdesc_pseudo_register_name (gdbarch, k1_pseudo_register_name);
  set_tdesc_pseudo_register_type (gdbarch, k1_pseudo_register_type);
  set_tdesc_pseudo_register_reggroup_p (gdbarch,
					k1_pseudo_register_reggroup_p);

  set_gdbarch_pseudo_register_read (gdbarch, k1_pseudo_register_read);
  set_gdbarch_pseudo_register_write (gdbarch, k1_pseudo_register_write);
  set_gdbarch_dwarf2_reg_to_regnum (gdbarch, k1_dwarf2_reg_to_regnum);
  dwarf2_frame_set_init_reg (gdbarch, k1_dwarf2_frame_init_reg);

  set_gdbarch_return_value (gdbarch, k1_return_value);
  set_gdbarch_push_dummy_call (gdbarch, k1_push_dummy_call);
  set_gdbarch_dummy_id (gdbarch, k1_dummy_id);

  set_gdbarch_skip_prologue (gdbarch, k1_skip_prologue);
  set_gdbarch_unwind_pc (gdbarch, k1_unwind_pc);
  dwarf2_append_unwinders (gdbarch);
  frame_unwind_append_unwinder (gdbarch, &k1_frame_unwind);

  set_gdbarch_fetch_tls_load_module_address (gdbarch,
					     k1_fetch_tls_load_module_address);

  set_gdbarch_decr_pc_after_break (gdbarch, 4);
  set_gdbarch_breakpoint_from_pc (gdbarch, k1_breakpoint_from_pc);
  set_gdbarch_adjust_breakpoint_address (gdbarch, 
					 k1_adjust_breakpoint_address);
  /* Settings that should be unnecessary.  */
  set_gdbarch_inner_than (gdbarch, core_addr_lessthan);
  set_gdbarch_print_insn (gdbarch, k1_print_insn);

  /* Displaced stepping */
  set_gdbarch_displaced_step_copy_insn (gdbarch, k1_displaced_step_copy_insn);
  set_gdbarch_displaced_step_fixup (gdbarch, k1_displaced_step_fixup);
  set_gdbarch_displaced_step_free_closure (gdbarch, k1_displaced_step_free_closure);
  set_gdbarch_displaced_step_location (gdbarch, k1_displaced_step_location);
  set_gdbarch_max_insn_length (gdbarch, K1MAXBUNDLESIZE*2);

  set_gdbarch_get_longjmp_target (gdbarch, k1_get_longjmp_target);

  return gdbarch;
}

static void add_op (struct op_list **list, k1opc_t *op)
{
    struct op_list *op_l = malloc (sizeof (struct op_list));
    
    op_l->op = op;
    op_l->next = *list;
    
    *list = op_l;
}

static void k1_look_for_insns (void)
{
    /* FIXME: consider the k1cp ABI */
    k1opc_t *op = k1dp_k1optab;

    while (op->as_op[0]) {
	if (strcmp ("add", op->as_op) == 0) {
            add_op (&sp_adjust_insns, op);
            add_op (&prologue_helper_insns, op);
	} else if (strcmp ("sbf", op->as_op) == 0)
            add_op (&sp_adjust_insns, op);
	else if (strcmp ("swm", op->as_op) == 0)
            add_op (&sp_store_insns, op);
	else if (strcmp ("sdm", op->as_op) == 0)
            add_op (&sp_store_insns, op);
	else if (strcmp ("shm", op->as_op) == 0)
            add_op (&sp_store_insns, op);
	else if (strcmp ("sb", op->as_op) == 0)
            add_op (&sp_store_insns, op);
	else if (strcmp ("sh", op->as_op) == 0)
            add_op (&sp_store_insns, op);
	else if (strcmp ("sw", op->as_op) == 0)
            add_op (&sp_store_insns, op);
	else if (strcmp ("sd", op->as_op) == 0)
            add_op (&sp_store_insns, op);
	else if (strcmp ("make", op->as_op) == 0)
	    add_op (&prologue_helper_insns, op);
	else if (strcmp ("sxb", op->as_op) == 0)
            add_op (&prologue_helper_insns, op);
	else if (strcmp ("sxh", op->as_op) == 0)
            add_op (&prologue_helper_insns, op);
	else if (strcmp ("extfz", op->as_op) == 0)
            add_op (&prologue_helper_insns, op);

	++op;
    }
}

extern initialize_file_ftype _initialize_k1_tdep; /* -Wmissing-prototypes */

void
_initialize_k1_tdep (void)
{
  k1_look_for_insns ();
  gdbarch_register (bfd_arch_k1, k1_gdbarch_init, NULL);

  observer_attach_inferior_created (k1_inferior_created);
}
