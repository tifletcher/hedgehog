/* This file is part of Hedgehog LISP.
 * Copyright (C) 2003, 2004, 2005 Oliotalo Ltd.
 * See file LICENSE.LGPL for pertinent licensing conditions.
 *
 * Author: Kenneth Oksanen <cessu@iki.fi>
 */

/* The byte code interpreter.
 */

#include "hh_common.h"
#include "hh_interp.h"
#include "hh_error.h"
#include "hh_data.h"
#include "hh_avl.h"
#include "hh_printf.h"


/* Include `HH_INSN_COOKIE' from `hh_seed.h'.  This cookie contains a
   checksum of all insns and their kinds, making it possible to detect
   a mismatch between the instructions sets understood by the
   interpreter and generated by the compiler. */
#include "hh_seed.h"

#include <stdlib.h>


#ifndef HH_CHECK
#ifdef HH_TESTING

#define HH_CHECK(value, expr, err)		\
  do {						\
    if (!(expr)) {				\
      ctx->offending_value = (value);		\
      error = (err);				\
      goto error;				\
    }						\
  } while (0)

#else

#define HH_CHECK(value, expr, err)  /* Do nothing. */

#endif
#endif



/* The `program' is a sequence of bytes so that
     - Bytes 0..3 are a magic cookie and contain the values
       0x4E, 0xD6, 0xE4 and 0x06, respectively.
     - Bytes 4..7 contain a 32-bit checksum of the rest of the
       program.  This can be used to check that the program has been
       received correctly, but it is not resilient to malicious
       attacks.
     - Byte 8 contains a version number of the byte code file format.
       If it is different from `HH_BCODE_VERSION', then the byte code
       is not executable on this byte code interpreter.
     - Bytes 9..11 tell `proglen', the length of the program block, in
       bytes.
     - Bytes 12..12+proglen-1 contain the program, the execution
       starts at the 12th byte.
     - Bytes 12+proglen..end contain constant data, most significantly
       strings and names of quoted symbols for the `symboltostring'
       primitive.

   All values are encoded in the most significant byte first order,
   but data in the constant pool is any byte order and
   `hh_program_check' fixes this to current byte order.

   `program' must be aligned to word boundary. */


/* Check the given program file is correct and executable on this byte
   code interpreter.  Returns non-zero on success. */

hh_error_t hh_program_check(unsigned char *program, unsigned int n_bytes)
{
  hh_word_t h = HH_COOKIE, i;

  HH_ASSERT(HH_NUMBER_OF_INSNS < 64);
  HH_ASSERT(HH_NUMBER_OF_IMMS < 64);

  /* Check for the alignment of `program'. */
  HH_ASSERT((((hh_word_t) program) & 0x3) == 0x0);

  if (n_bytes < 16		/* The minimal program length. */
      || HH_GET_UINT32(program) != h)
    return HH_ERROR_PROGRAM_CORRUPT;
  if (program[8] != HH_BCODE_VERSION)
    return HH_ERROR_PROGRAM_WRONG_VERSION;
  
  h = HH_INSN_COOKIE;
  for (i = 8; i < n_bytes; i++) {
    h += program[i];
    h += h << 10;
    h ^= h >> 7;
  }
  if (HH_GET_UINT32(program + 4) != h) {
    HH_PRINT("computed hash 0x%08X != found hash 0x%08X\n",
	     h, HH_GET_UINT32(program + 4));
    return HH_ERROR_PROGRAM_CORRUPT;
  }
  i = HH_GET_UINT24(program + 9);
  return hh_fix_byteorder((hh_word_t *) (program + i),
			  (n_bytes - i) / sizeof(hh_word_t));
}

hh_context_t *hh_context_allocate(unsigned char *program,
				  unsigned long heap_n_words,
				  unsigned long stack_n_words,
				  int enable_profiling)
{
  hh_context_t *ctx = HH_MALLOC(sizeof(hh_context_t) +
				(stack_n_words - 1) * sizeof(hh_word_t));
  if (ctx == NULL)
    return NULL;

  ctx->heap = HH_MALLOC(sizeof(hh_word_t) * heap_n_words);
  ctx->old_heap = HH_MALLOC(sizeof(hh_word_t) * heap_n_words);
  if (ctx->heap == NULL || ctx->old_heap == NULL) {
    HH_PRINT("Could not allocate %ld bytes for heaps.\n",
	     2 * sizeof(hh_word_t) * heap_n_words);
    if (ctx->heap != NULL)
      HH_FREE(ctx->heap);
    if (ctx->old_heap != NULL)
      HH_FREE(ctx->old_heap);
    HH_FREE(ctx);
    return NULL;
  }

  ctx->program = program;
  ctx->pc = program + 12;
  ctx->sp = &ctx->stack[0];
  ctx->accu = ctx->env = ctx->new_env = HH_NIL;
  ctx->constant = (hh_word_t *) (program + HH_GET_UINT24(program + 9));
  ctx->heap_free = ctx->heap;
  ctx->heap_ptr = ctx->heap + heap_n_words;
  ctx->heap_n_words = heap_n_words;
  ctx->stack_n_words = stack_n_words;

#ifdef HH_UNIX
  FD_ZERO(&ctx->select_read_fds);
  FD_ZERO(&ctx->select_write_fds);
  ctx->select_retval = 0;
  ctx->program_wants_to_select = 0;
#endif

#ifdef HH_TESTING
  ctx->gc_trace_enabled = 0;
  ctx->profile_data = NULL;
  ctx->insn_trace_enabled = 0;
  ctx->redzone = HH_COOKIE;
  if (enable_profiling) {
    hh_word_t i, n_insns = HH_GET_UINT24(program + 9);

    ctx->profile_data = HH_MALLOC(n_insns * sizeof(hh_word_t));
    for (i = 0; i < n_insns; i++)
      ctx->profile_data[i] = 0;
  }

  {
    /* Initialize the stack with HH_COOKIE.  When we free the context,
       we can check which cookies are left and thereby get a
       guesstimate of how much stack was consumed. */
    hh_word_t i;
    for (i = 0; i < stack_n_words; i++)
      ctx->stack[i] = HH_COOKIE;
  }
#endif

  return ctx;
}


long hh_context_free(hh_context_t *ctx)
{
  long i = 0;

#ifdef HH_TESTING
  for (i = ctx->stack_n_words - 1; i >= 0; i--)
    if (ctx->stack[i] != HH_COOKIE)
      break;
  if (ctx->redzone != HH_COOKIE)
    HH_PRINT("The red zone below to stack has been stomped.\n");
  /* Place this after testing the red zone, since if it is stomped,
     then probably this too will crash, but we already have an
     indication of it. */
  if (ctx->profile_data) {
    hh_word_t i, n_insns = HH_GET_UINT24(ctx->program + 9);
    HH_PRINT("Instruction execution count follows:\n");
    for (i = 0; i < n_insns; i++)
      if (ctx->profile_data[i])
	HH_PRINT("%06d %8u\n", i - 12, ctx->profile_data[i]);
  }
#endif
  HH_FREE(ctx);
  return i;
}


/* Auxiliary routines to the snprintf instruction. */

typedef struct {
  hh_lisp_print_ctx_t lpx;
  hh_signed_word_t n_chars, max_n_chars;
  unsigned char *s;
} hh_lisp_snprint_ctx_t;

static int hh_lisp_snprint_count_chars_cb(char ch, void *ctx)
{
  hh_lisp_snprint_ctx_t *lspx = (hh_lisp_snprint_ctx_t *) ctx;

  if (lspx->max_n_chars >= 0 && lspx->n_chars > lspx->max_n_chars)
    return 0;
  lspx->n_chars++;
  return 1;
}
				       
static int hh_lisp_snprint_write_chars_cb(char ch, void *ctx)
{
  hh_lisp_snprint_ctx_t *lspx = (hh_lisp_snprint_ctx_t *) ctx;

  if (lspx->max_n_chars >= 0 && lspx->n_chars > lspx->max_n_chars)
    return 0;
  lspx->s[lspx->n_chars++] = ch;
  return 1;
}
				       
				       
/* Interpret the byte code program for some time.  Return 0 if the
   program exited, in which case the next call to interp will rerun
   the same program. */
   
hh_error_t hh_interp_step(hh_context_t *ctx, hh_signed_word_t n_ticks)
{
  hh_word_t accu, w, env, new_env;
  hh_signed_word_t imm, sw;
  unsigned char *pc, *prev_pc;
  hh_word_t *sp, *accu_as_ptr = NULL, *p;
  unsigned char insn, insn_mnemonic;
  hh_word_t n_words_reserved;
  int just_collected = 0;
#ifdef HH_TESTING
  hh_error_t error = HH_OK;
#endif

  HH_ASSERT(ctx != NULL);

  pc = ctx->pc;
  HH_ASSERT(pc != NULL);
  sp = ctx->sp;
  accu = ctx->accu;
  env = ctx->env;
  new_env = ctx->new_env;
  goto maybe_accu_to_ptr;

#define HH_DO_RESERVE(n_words)					\
  do {								\
    if (HH_UNLIKELY(!HH_CAN_ALLOCATE(ctx, (n_words)))) {	\
      n_words_reserved = (n_words);				\
      goto garbage_collect;					\
    }								\
  } while (0)
#define HH_RESERVE(n_words)  HH_DO_RESERVE((n_words) + 16)

 reserve_and_maybe_accu_to_ptr:
  if (HH_WORD_IS_PTR(accu)) {
  reserve_and_accu_to_ptr:
    accu_as_ptr = HH_WORD_TO_PTR(ctx, accu);
  }
 reserve_for_next:
  /* Reserve 16 words of memory so that instructions that allocate at
     most a tuple, a cons cell, or an AVL tree node do not have to
     check for the heap exhaustion themselves.  This reduces code size
     reduction a little. */
  prev_pc = pc;
  HH_DO_RESERVE(16);
  HH_ASSERT(HH_BOX_N_WORDS <= 16);
  HH_ASSERT(HH_CONS_N_WORDS <= 16);
  HH_ASSERT(HH_BOX_N_WORDS + HH_CONS_N_WORDS <= 16);
  HH_ASSERT(HH_AVL_MAKE_NODE_N_WORDS <= 16);
  HH_ASSERT(HH_STRING_N_WORDS(1) <= 16);

  while (HH_LIKELY(n_ticks-- > 0)) {
    /* Put `pc' to `prev_pc' in case we issue gc or a branch here. */
    prev_pc = pc;
    insn = *pc++;
    /* Decompose the insn to 64 lower 6 bits telling the mnemonic and
       upper two bits telling the instruction kind: whether it is an
       instruction without immediate fields, or whether it has a one,
       two or four-byte immediate field. */
    insn_mnemonic = insn & 0x3F;
    insn >>= 6;
#ifdef HH_TESTING
    if (ctx->profile_data)
      ctx->profile_data[prev_pc - ctx->program]++;
#endif
    if (insn == 0) {
      /* A normal insn. */
      switch (insn_mnemonic) {
#ifdef HH_TESTING
#define INSN(mnemonic, flags, block)					  \
      case HH_INSN_ ## mnemonic:					  \
	if (ctx->insn_trace_enabled)					  \
	  HH_PRINT("About to execute " #mnemonic			  \
		   ", pc = %06d, accu = %d, sp = %d\n",			  \
		   prev_pc - (ctx->program + 12), accu, sp - ctx->stack); \
        block								  \
        break;
#else
#define INSN(mnemonic, flags, block)		\
      case HH_INSN_ ## mnemonic:		\
        block					\
        break;
#endif
#define IMM(mnemonic, flags, block)	 /* Nothing */
#define EXT_INSN(mnemonic, flags, block) /* Nothing */
#include "hh_insn.def"

      }
    } else {
      /* Gather the immediate value byte by byte.  First sign-extend
	 it, then bitwise-or byte by byte. */
      imm = (hh_signed_word_t) * (signed char *) pc++;
      if (insn > 1) {
	imm <<= 8;
	imm |= *pc++;
	if (insn == 3) {
	  imm <<= 8;
	  imm |= *pc++;
	  imm <<= 8;
	  imm |= *pc++;
	}
      }
      switch (insn_mnemonic) {
#define INSN(mnemonic, flags, block)	/* Nothing */
#ifdef HH_TESTING
#define IMM(mnemonic, flags, block)					\
      case HH_IMM_ ## mnemonic:						\
	if (ctx->insn_trace_enabled)					\
	  HH_PRINT("About to execute imm " #mnemonic			\
		   ", pc = %06d, imm = %d, accu = %d, sp = %d\n",	\
		   prev_pc - (ctx->program + 12), imm, accu,		\
		   sp - ctx->stack);					\
        block								\
	break;
#else
#define IMM(mnemonic, flags, block)		\
      case HH_IMM_ ## mnemonic:			\
        block					\
	break;
#endif
#define EXT_INSN(mnemonic, flags, block) /* Nothing */
#include "hh_insn.def"

      case HH_IMM_ext:
	switch (imm) {
#define INSN(mnemonic, flags, block)	/* Nothing */
#define IMM(mnemonic, flags, block)	/* Nothing */
#ifdef HH_TESTING
#define EXT_INSN(mnemonic, flags, block)			\
      case HH_INSN_ ## mnemonic - 256:				\
	if (ctx->insn_trace_enabled)				\
	  HH_PRINT("About to execute ext insn " #mnemonic	\
		   ", pc = %06d, accu = %d, sp = %d\n",		\
		   prev_pc - (ctx->program + 12), accu,		\
		   sp - ctx->stack);				\
        block							\
	break;
#else
#define EXT_INSN(mnemonic, flags, block)			\
      case HH_INSN_ ## mnemonic - 256:				\
        block							\
	break;
#endif
#include "hh_insn.def"

	}
	break;
      }
    }
  }

 end_time_slice:
  /* The time slice ended, store some local-variable-cached data back
     to the `ctx' and return. */
  ctx->pc = pc;
  ctx->sp = sp;
  ctx->accu = accu;
  ctx->env = env;
  ctx->new_env = new_env;
  return HH_OK;

#ifdef HH_TESTING
  /* The macro HH_CHECK does nothing when HH_TESTING is defined, so
     leave also this code out. */
 error:
  /* Put information into `ctx' in order to ease debugging. */
  ctx->pc = prev_pc;
  ctx->sp = sp;
  ctx->accu = accu;
  ctx->env = env;
  ctx->new_env = new_env;
  return error;
#endif /* HH_TESTING */

 garbage_collect:
  /* Byte code instructions should goto here if they need more memory
     than in available. */
#ifdef HH_TESTING
  if (ctx->gc_trace_enabled)
    HH_PRINT("[gc...");
#endif
  hh_gc_start(ctx);
  HH_ROOT(ctx, accu);
  HH_ROOT(ctx, env);
  HH_ROOT(ctx, new_env);
  {
    hh_word_t *gc_sp;

    for (gc_sp = ctx->stack; gc_sp < sp; gc_sp++)
      HH_ROOT(ctx, *gc_sp);
  }
  hh_gc_finish(ctx);
#ifdef HH_TESTING
  if (ctx->gc_trace_enabled)
    HH_PRINT("%d of %d words in use]\n",
	     ctx->heap_free - ctx->heap, ctx->heap_n_words);
#endif
  if (HH_UNLIKELY(!HH_CAN_ALLOCATE(ctx, n_words_reserved))) {
    /* Try to find a catch for tag "out-of-memory", for which the
       compiler ensures to be catch_tag == 1, and if one is found, pop
       stuff from stack and try gc again. */
    for (p = sp - 3; p >= ctx->stack; p--)
      if (*p == 0x312) {
#ifdef HH_TESTING
	if (ctx->gc_trace_enabled)
	  HH_PRINT("[out-of-memory-exception]\n");
#endif
	prev_pc = pc = HH_WORD_TO_PC(ctx, p[1]);
	env = p[2];
	new_env = accu = HH_NIL;
	sp = p;
	n_words_reserved = 0;
	goto garbage_collect;
      }
#ifdef HH_TESTING
    error = HH_ERROR_HEAP_FULL;
    goto error;
#else
    return HH_ERROR_HEAP_FULL;
#endif
  }
  pc = prev_pc;
  goto maybe_accu_to_ptr;
}


#ifndef HH_SMALL

/* Print, using HH_PRINT, a function call backtrace of the current
   state of execution. */
void hh_backtrace(hh_context_t *ctx)
{
  hh_word_t *sp;

  HH_PRINT("Error occurred at ");
  hh_lisp_print_interpreter(ctx, HH_PC_TO_WORD(ctx, ctx->pc), 1);
  HH_PRINT("\nStack dump follows:\n");
  for (sp = ctx->sp - 1; sp >= ctx->stack; sp--)
    if (HH_IS_PC(*sp))
      {
	HH_PRINT("  ");
	hh_lisp_print_interpreter(ctx, *sp, (sp + 20 > ctx->sp) ? 3 : 2);
	HH_PRINT("\n");
      }
}

#endif
