/*
 * xemu: the guest PC hooks' address expressions (QAPI XemuHookRegion's
 * addr, target/i386/tcg/xemu-hooks.h) for target-independent code, such
 * as XEMU_TRACE_RECORD's regions (ui/xemu-trace-record.c).
 * Implementation: target/i386/tcg/system/xemu-hooks.c.
 *
 * Copyright (c) 2026 xemu-macos contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_HOOK_EXPR_H
#define XEMU_HOOK_EXPR_H

typedef struct XemuHookExpr XemuHookExpr;

/* Parse @s as a hook region's address (g_free() the result), or NULL. */
XemuHookExpr *xemu_hook_expr_new(const char *s, Error **errp);

typedef enum XemuHookExprRead {
    XEMU_HOOK_EXPR_READ_OK,
    XEMU_HOOK_EXPR_UNRESOLVED,  /* a dereference read memory not guest RAM */
    XEMU_HOOK_EXPR_UNBACKED,    /* the bytes there aren't all guest RAM */
} XemuHookExprRead;

/*
 * Outside a hook, with the BQL held and no guest instruction running (at
 * a vblank under icount, or on the vCPU thread between blocks): evaluate
 * @e with CPU 0's registers and page tables and copy the @len bytes at
 * the result into @buf, as a hook's collect region reads them.
 */
XemuHookExprRead xemu_hook_expr_read_cpu0(const XemuHookExpr *e, void *buf,
                                          unsigned len);

#endif
