/*
 * Copyright (C) 2008-2025 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 * Copyright (C) 2008 Christian Berentsen <jc.berentsen@gmail.com>
 * Copyright (C) 2024 Yannis Juglaret <yjuglaret@mozilla.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "guminterceptor-priv.h"

#include "gumlibc.h"
#include "gummemory.h"
#include "gumsysinternals.h"
#include "gumx86reader.h"
#include "gumx86relocator.h"

#include <string.h>

#define GUM_INTERCEPTOR_FULL_REDIRECT_SIZE  16
#define GUM_INTERCEPTOR_NEAR_REDIRECT_SIZE  5
#define GUM_X86_JMP_MAX_DISTANCE            (G_MAXINT32 - 16384)

#define GUM_FRAME_OFFSET_CPU_CONTEXT 0
#define GUM_FRAME_OFFSET_CPU_FLAGS \
    (GUM_FRAME_OFFSET_CPU_CONTEXT + sizeof (GumCpuContext))
#define GUM_FRAME_OFFSET_NEXT_HOP \
    (GUM_FRAME_OFFSET_CPU_FLAGS + sizeof (gpointer))
#define GUM_FRAME_OFFSET_TOP \
    (GUM_FRAME_OFFSET_NEXT_HOP + sizeof (gpointer))

#define GUM_FCDATA(context) \
    ((GumX86FunctionContextData *) (context)->backend_data.storage)

typedef struct _GumX86FunctionContextData GumX86FunctionContextData;

struct _GumInterceptorBackend
{
  GumCodeAllocator * allocator;

  GumX86Writer writer;
  GumX86Relocator relocator;

  GumCodeSlice * enter_thunk;
  GumCodeSlice * leave_thunk;
};

struct _GumX86FunctionContextData
{
  guint redirect_code_size;
  gpointer push_to_shadow_stack;
};

G_STATIC_ASSERT (sizeof (GumX86FunctionContextData)
    <= sizeof (GumFunctionContextBackendData));

static void gum_interceptor_backend_create_thunks (
    GumInterceptorBackend * self);
static void gum_interceptor_backend_destroy_thunks (
    GumInterceptorBackend * self);

static void gum_emit_enter_thunk (GumX86Writer * cw);
static void gum_emit_leave_thunk (GumX86Writer * cw);

static void gum_emit_prolog (GumX86Writer * cw, gssize stack_displacement);
static void gum_emit_epilog (GumX86Writer * cw, GumPointCut point_cut);

GumInterceptorBackend *
_gum_interceptor_backend_create (GRecMutex * mutex,
                                 GumCodeAllocator * allocator)
{
  GumInterceptorBackend * backend;

  backend = g_slice_new (GumInterceptorBackend);
  backend->allocator = allocator;

  gum_x86_writer_init (&backend->writer, NULL);
  gum_x86_relocator_init (&backend->relocator, NULL, &backend->writer);

  gum_interceptor_backend_create_thunks (backend);

  return backend;
}

void
_gum_interceptor_backend_destroy (GumInterceptorBackend * backend)
{
  gum_interceptor_backend_destroy_thunks (backend);

  gum_x86_relocator_clear (&backend->relocator);
  gum_x86_writer_clear (&backend->writer);

  g_slice_free (GumInterceptorBackend, backend);
}

gboolean
_gum_interceptor_backend_claim_grafted_trampoline (GumInterceptorBackend * self,
                                                   GumFunctionContext * ctx)
{
  return FALSE;
}

static gboolean
gum_interceptor_backend_prepare_trampoline (GumInterceptorBackend * self,
                                            GumFunctionContext * ctx)
{
  GumX86FunctionContextData * data = GUM_FCDATA (ctx);
#if GLIB_SIZEOF_VOID_P == 4
  data->redirect_code_size = GUM_INTERCEPTOR_NEAR_REDIRECT_SIZE;

  ctx->trampoline_slice = gum_code_allocator_alloc_slice (self->allocator);
#else
  GumAddressSpec spec;
  gsize default_alignment = 0;

  spec.near_address = ctx->function_address;
  spec.max_distance = GUM_X86_JMP_MAX_DISTANCE;

  /*
   * When creating a fast interceptor, we won't be vectoring from the target
   * function to the trampoline slice, we will instead be re-directing direct to
   * the target replacement function and therefore must consider the worst case
   * scenario of a JMP with RIP-relative immediate embedded in the code stream.
   * We will still use the trampoline slice for writing the trampoline for the
   * original function in the event that the patched function wishes to call the
   * original. Thus it isn't important where the trampoline slice is located.
   *
   * When creating a normal interceptor, the patch to the target function
   * re-directs first to the on_enter trampoline written to the trampoline
   * slice. If we are able to allocate the slice nearby the target function,
   * then we are able to use a near rather than far jump and hence a shorter
   * op-code. This reduces the amount of the target function prologue which
   * needs to be over-written. If we cannot allocate nearby, however, we
   * just revert to assuming the worst case scenario.
   */
  if (ctx->type == GUM_INTERCEPTOR_TYPE_DEFAULT)
  {
    ctx->trampoline_slice = gum_code_allocator_try_alloc_slice_near (
        self->allocator, &spec, default_alignment);
  }

  if (ctx->trampoline_slice == NULL)
  {
    data->redirect_code_size = GUM_INTERCEPTOR_FULL_REDIRECT_SIZE;

    ctx->trampoline_slice = gum_code_allocator_alloc_slice (self->allocator);
  }
  else
  {
    data->redirect_code_size = GUM_INTERCEPTOR_NEAR_REDIRECT_SIZE;
  }
#endif

  if (!gum_x86_relocator_can_relocate (ctx->function_address,
        data->redirect_code_size, NULL))
    goto not_enough_space;

  return TRUE;

not_enough_space:
  {
    gum_code_slice_unref (ctx->trampoline_slice);
    ctx->trampoline_slice = NULL;
    return FALSE;
  }
}

gboolean
_gum_interceptor_backend_create_trampoline (GumInterceptorBackend * self,
                                            GumFunctionContext * ctx)
{
  GumX86Writer * cw = &self->writer;
  GumX86Relocator * rl = &self->relocator;
  GumX86FunctionContextData * data = GUM_FCDATA (ctx);
  GumAddress function_ctx_ptr;
  gpointer after_push_to_shadow_stack;
  guint reloc_bytes;

  if (!gum_interceptor_backend_prepare_trampoline (self, ctx))
    return FALSE;

  gum_x86_writer_reset (cw, ctx->trampoline_slice->data);

  if (ctx->type != GUM_INTERCEPTOR_TYPE_FAST)
  {
    function_ctx_ptr = GUM_ADDRESS (gum_x86_writer_cur (cw));
    gum_x86_writer_put_bytes (cw, (guint8 *) &ctx,
        sizeof (GumFunctionContext *));

    ctx->on_enter_trampoline = gum_x86_writer_cur (cw);

    gum_x86_writer_put_push_near_ptr (cw, function_ctx_ptr);
    gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (self->enter_thunk->data));

    if ((cw->cpu_features & GUM_CPU_CET_SS) != 0)
    {
      /*
       * Jumping to push_to_shadow_stack will push the on_leave_trampoline
       * address onto the shadow stack, thereby making it a legit address to
       * return to. Then it will jump back through XAX.
       */

      after_push_to_shadow_stack = gum_x86_writer_cur (cw);

      gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XSP,
          GUM_X86_XSP, (gssize) sizeof (gpointer));

      gum_x86_writer_put_jmp_reg (cw, GUM_X86_XAX);

      data->push_to_shadow_stack = gum_x86_writer_cur (cw);

      gum_x86_writer_put_call_address (cw,
          GUM_ADDRESS (after_push_to_shadow_stack));
    }

    ctx->on_leave_trampoline = gum_x86_writer_cur (cw);

    gum_x86_writer_put_push_near_ptr (cw, function_ctx_ptr);
    gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (self->leave_thunk->data));

    gum_x86_writer_flush (cw);
    g_assert (gum_x86_writer_offset (cw) <= ctx->trampoline_slice->size);
  }

  ctx->on_invoke_trampoline = gum_x86_writer_cur (cw);
  gum_x86_relocator_reset (rl, (guint8 *) ctx->function_address, cw);

  do
  {
    reloc_bytes = gum_x86_relocator_read_one (rl, NULL);
    g_assert (reloc_bytes != 0);
  }
  while (reloc_bytes < data->redirect_code_size);
  gum_x86_relocator_write_all (rl);

  if (!gum_x86_relocator_eoi (rl))
  {
    gum_x86_writer_put_jmp_address (cw,
        GUM_ADDRESS (ctx->function_address) + reloc_bytes);
  }

  gum_x86_writer_flush (cw);
  g_assert (gum_x86_writer_offset (cw) <= ctx->trampoline_slice->size);

  ctx->overwritten_prologue_len = reloc_bytes;
  gum_memcpy (ctx->overwritten_prologue, ctx->function_address, reloc_bytes);

  return TRUE;
}

void
_gum_interceptor_backend_destroy_trampoline (GumInterceptorBackend * self,
                                             GumFunctionContext * ctx)
{
  gum_code_slice_unref (ctx->trampoline_slice);
  ctx->trampoline_slice = NULL;
}

void
_gum_interceptor_backend_activate_trampoline (GumInterceptorBackend * self,
                                              GumFunctionContext * ctx,
                                              gpointer prologue)
{
  GumX86Writer * cw = &self->writer;
  guint padding;

  gum_x86_writer_reset (cw, prologue);
  cw->pc = GPOINTER_TO_SIZE (ctx->function_address);

  if (ctx->type == GUM_INTERCEPTOR_TYPE_FAST)
  {
    gum_x86_writer_put_jmp_address (cw,
        GUM_ADDRESS (ctx->replacement_function));
  }
  else
  {
    gum_x86_writer_put_jmp_address (cw,
        GUM_ADDRESS (ctx->on_enter_trampoline));
  }

  gum_x86_writer_flush (cw);
  g_assert (gum_x86_writer_offset (cw) <= GUM_FCDATA (ctx)->redirect_code_size);
  g_assert (gum_x86_writer_offset (cw) <= ctx->overwritten_prologue_len);

  padding = ctx->overwritten_prologue_len - gum_x86_writer_offset (cw);
  gum_x86_writer_put_nop_padding (cw, padding);
  gum_x86_writer_flush (cw);
}

void
_gum_interceptor_backend_deactivate_trampoline (GumInterceptorBackend * self,
                                                GumFunctionContext * ctx,
                                                gpointer prologue)
{
  gum_memcpy (prologue, ctx->overwritten_prologue,
      ctx->overwritten_prologue_len);
}

gpointer
_gum_interceptor_backend_get_function_address (GumFunctionContext * ctx)
{
  return ctx->function_address;
}

gpointer
_gum_interceptor_backend_resolve_redirect (GumInterceptorBackend * self,
                                           gpointer address)
{
  gpointer target;

  target = gum_x86_reader_try_get_relative_jump_target (address);
  if (target == NULL)
    target = gum_x86_reader_try_get_indirect_jump_target (address);

  return target;
}

gsize
_gum_interceptor_backend_detect_hook_size (gconstpointer code,
                                           csh capstone,
                                           cs_insn * insn)
{
  gsize hook_size;
  const uint8_t * cursor;
  size_t size;
  uint64_t addr;
  const cs_x86_op * dst;

  cursor = code;
  size = 16;
  addr = GPOINTER_TO_SIZE (cursor);

  if (!cs_disasm_iter (capstone, &cursor, &size, &addr, insn))
    return 0;
  if (insn->id != X86_INS_JMP)
    return 0;
  dst = &insn->detail->x86.operands[0];
  switch (dst->type)
  {
    case X86_OP_IMM:
      hook_size = insn->size;
      break;
    case X86_OP_MEM:
      if (dst->mem.segment == X86_REG_INVALID &&
          dst->mem.base == X86_REG_RIP &&
          dst->mem.index == X86_REG_INVALID &&
          dst->mem.scale == 1 &&
          dst->mem.disp == 2)
      {
        const gsize inline_data_size = dst->mem.disp + sizeof (gpointer);

        cursor += inline_data_size;
        addr += inline_data_size;

        hook_size = insn->size + inline_data_size;
      }
      else
      {
        return 0;
      }
      break;
    default:
      return 0;
  }

  while (cs_disasm_iter (capstone, &cursor, &size, &addr, insn) &&
      insn->id == X86_INS_NOP)
    hook_size += insn->size;

  return hook_size;
}

static void
gum_interceptor_backend_create_thunks (GumInterceptorBackend * self)
{
  GumX86Writer * cw = &self->writer;

  self->enter_thunk = gum_code_allocator_alloc_slice (self->allocator);
  gum_x86_writer_reset (cw, self->enter_thunk->data);
  gum_emit_enter_thunk (cw);
  gum_x86_writer_flush (cw);
  g_assert (gum_x86_writer_offset (cw) <= self->enter_thunk->size);

  self->leave_thunk = gum_code_allocator_alloc_slice (self->allocator);
  gum_x86_writer_reset (cw, self->leave_thunk->data);
  gum_emit_leave_thunk (cw);
  gum_x86_writer_flush (cw);
  g_assert (gum_x86_writer_offset (cw) <= self->leave_thunk->size);
}

static void
gum_interceptor_backend_destroy_thunks (GumInterceptorBackend * self)
{
  gum_code_slice_unref (self->leave_thunk);

  gum_code_slice_unref (self->enter_thunk);
}

static void
gum_emit_enter_thunk (GumX86Writer * cw)
{
  const gssize return_address_stack_displacement = 0;
  const gchar * prepare_trap_on_leave = "prepare_trap_on_leave";
  GumX86Reg function_ctx_reg = (sizeof (gpointer) == 8)
      ? GUM_X86_R12
      : GUM_X86_XDI;

  gum_emit_prolog (cw, return_address_stack_displacement);

  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XSI,
      GUM_X86_XBX, GUM_FRAME_OFFSET_CPU_CONTEXT);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XDX,
      GUM_X86_XBX, GUM_FRAME_OFFSET_TOP);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XCX,
      GUM_X86_XBX, GUM_FRAME_OFFSET_NEXT_HOP);
  gum_x86_writer_put_mov_reg_reg_ptr (cw, function_ctx_reg, GUM_X86_XCX);

  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (_gum_function_context_begin_invocation), 4,
      GUM_ARG_REGISTER, function_ctx_reg,
      GUM_ARG_REGISTER, GUM_X86_XSI,
      GUM_ARG_REGISTER, GUM_X86_XDX,
      GUM_ARG_REGISTER, GUM_X86_XCX);

  if ((cw->cpu_features & GUM_CPU_CET_SS) != 0)
  {
    gpointer epilog;

    gum_x86_writer_put_test_reg_reg (cw, GUM_X86_EAX, GUM_X86_EAX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, prepare_trap_on_leave,
        GUM_NO_HINT);

    epilog = gum_x86_writer_cur (cw);
    gum_emit_epilog (cw, GUM_POINT_ENTER);

    gum_x86_writer_put_label (cw, prepare_trap_on_leave);
    gum_x86_writer_put_mov_reg_address (cw, GUM_X86_XAX, GUM_ADDRESS (epilog));
    gum_x86_writer_put_jmp_reg_offset_ptr (cw, function_ctx_reg,
        G_STRUCT_OFFSET (GumFunctionContext, backend_data) +
        G_STRUCT_OFFSET (GumX86FunctionContextData, push_to_shadow_stack));
  }
  else
  {
    gum_emit_epilog (cw, GUM_POINT_ENTER);
  }
}

static void
gum_emit_leave_thunk (GumX86Writer * cw)
{
  const gssize next_hop_stack_displacement = -((gssize) sizeof (gpointer));

  gum_emit_prolog (cw, next_hop_stack_displacement);

  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XSI,
      GUM_X86_XBX, GUM_FRAME_OFFSET_CPU_CONTEXT);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XDX,
      GUM_X86_XBX, GUM_FRAME_OFFSET_NEXT_HOP);
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_X86_XDI, GUM_X86_XDX);

  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (_gum_function_context_end_invocation), 3,
      GUM_ARG_REGISTER, GUM_X86_XDI,
      GUM_ARG_REGISTER, GUM_X86_XSI,
      GUM_ARG_REGISTER, GUM_X86_XDX);

  gum_emit_epilog (cw, GUM_POINT_LEAVE);
}

static void
gum_emit_prolog (GumX86Writer * cw,
                 gssize stack_displacement)
{
  guint8 fxsave[] = {
    0x0f, 0xae, 0x04, 0x24 /* fxsave [esp] */
  };

  /*
   * Set up our stack frame:
   *
   * [function_ctx/next_hop] <-- already pushed before the branch to our thunk
   * [cpu_flags]
   * [cpu_context] <-- xbx points to the start of the cpu_context
   * [alignment_padding]
   * [extended_context]
   */
  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_cld (cw); /* C ABI mandates this */
  gum_x86_writer_put_pushax (cw); /* all of GumCpuContext except for xip */
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XSP,
      GUM_X86_XSP, -((gssize) sizeof (gpointer))); /* GumCpuContext.xip */

  /* fixup the GumCpuContext stack pointer */
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XAX,
      GUM_X86_XSP, GUM_FRAME_OFFSET_TOP + stack_displacement);
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_X86_XSP, GUM_CPU_CONTEXT_OFFSET_XSP,
      GUM_X86_XAX);

  gum_x86_writer_put_mov_reg_reg (cw, GUM_X86_XBX, GUM_X86_XSP);
  gum_x86_writer_put_and_reg_u32 (cw, GUM_X86_XSP, (guint32) ~(16 - 1));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_X86_XSP, 512);
  gum_x86_writer_put_bytes (cw, fxsave, sizeof (fxsave));
}

static void
gum_emit_epilog (GumX86Writer * cw,
                 GumPointCut point_cut)
{
  guint8 fxrstor[] = {
    0x0f, 0xae, 0x0c, 0x24 /* fxrstor [esp] */
  };

  gum_x86_writer_put_bytes (cw, fxrstor, sizeof (fxrstor));
  gum_x86_writer_put_mov_reg_reg (cw, GUM_X86_XSP, GUM_X86_XBX);

  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XSP,
      GUM_X86_XSP, sizeof (gpointer)); /* discard
                                          GumCpuContext.xip */
  gum_x86_writer_put_popax (cw);
  gum_x86_writer_put_popfx (cw);

  if (point_cut == GUM_POINT_LEAVE)
  {
    gum_x86_writer_put_ret (cw);
  }
  else
  {
    /* Emulate a ret without affecting the shadow stack. */
    gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_X86_XSP,
        GUM_X86_XSP, sizeof (gpointer));
    gum_x86_writer_put_jmp_reg_offset_ptr (cw, GUM_X86_XSP,
        -((gssize) sizeof (gpointer)));
  }
}
