/*
 * XEMU_GUEST_BUGCHECK=1: name the guest's bugchecks and dead halts
 * (system/xemu-bugcheck.c).
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_I386_XEMU_BUGCHECK_H
#define TARGET_I386_XEMU_BUGCHECK_H

#include "cpu.h"

bool xemu_guest_bugcheck_enabled(void);
/* helper_hlt, vCPU thread: a HLT with IF clear (EIP past it). */
void xemu_guest_dead_halt(CPUX86State *env);
/* ui/xemu.c (BQL held): launch, each vblank while running, each load. */
void xemu_guest_bugcheck_init(void);
void xemu_guest_bugcheck_frame(void);
void xemu_guest_bugcheck_state_loaded(void);

#endif
