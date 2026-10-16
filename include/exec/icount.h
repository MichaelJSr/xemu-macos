/*
 * icount - Instruction Counter API
 * CPU timers state API
 *
 * Copyright 2020 SUSE LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_ICOUNT_H
#define EXEC_ICOUNT_H

/**
 * ICountMode: icount enablement state:
 *
 * @ICOUNT_DISABLED: Disabled - Do not count executed instructions.
 * @ICOUNT_PRECISE: Enabled - Fixed conversion of insn to ns via "shift" option
 * @ICOUNT_ADAPTATIVE: Enabled - Runtime adaptive algorithm to compute shift
 */
typedef enum {
    ICOUNT_DISABLED = 0,
    ICOUNT_PRECISE,
    ICOUNT_ADAPTATIVE,
} ICountMode;

#ifdef CONFIG_TCG
extern ICountMode use_icount;
#define icount_enabled() (use_icount)
#else
#define icount_enabled() ICOUNT_DISABLED
#endif

/* Protect the CONFIG_USER_ONLY test vs poisoning. */
#if defined(COMPILING_PER_TARGET) || defined(COMPILING_SYSTEM_VS_USER)
# ifdef CONFIG_USER_ONLY
#  undef  icount_enabled
#  define icount_enabled() ICOUNT_DISABLED
# endif
#endif

/*
 * Update the icount with the executed instructions. Called by
 * cpus-tcg vCPU thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu);

/* get raw icount value */
int64_t icount_get_raw(void);

/* return the virtual CPU time in ns, based on the instruction counter. */
int64_t icount_get(void);
/*
 * convert an instruction counter value to ns, based on the icount shift.
 * This shift is set as a fixed value with the icount "shift" option
 * (precise mode), or it is constantly approximated and corrected at
 * runtime in adaptive mode.
 */
int64_t icount_to_ns(int64_t icount);

/**
 * icount_configure: configure the icount options, including "shift"
 * @opts: Options to parse
 * @errp: pointer to a NULL-initialized error object
 *
 * Return: true on success, else false setting @errp with error
 */
bool icount_configure(QemuOpts *opts, Error **errp);

/* used by tcg vcpu thread to calc icount budget */
int64_t icount_round(int64_t count);

/* if the CPUs are idle, start accounting real time to virtual clock. */
void icount_start_warp_timer(void);
void icount_account_warp_timer(void);
#ifdef XBOX
/* sleep=off: the vCPU thread warps over idle time itself, between slices. */
bool icount_vcpu_warps(void);
bool icount_vcpu_timer_due(void);
/* XEMU_ICOUNT_LIVE_PADS=1 with sleep=on: exact warps paced on real time. */
bool icount_live_pads_requested(void); /* the variable is "1" */
int64_t icount_paced_wait_ns(void);
void icount_paced_align(int64_t *diff_clk);
#endif
void icount_notify_exit(void);

/*
 * xemu: a device thread that changes guest state on its own schedule
 * (nv2a's PFIFO, the APU) registers a sync hook. Under icount the vCPU
 * thread runs every hook, BQL held, before it executes guest code again;
 * a hook returns once its thread has done all the work the guest and the
 * virtual timers have asked of it, so that work lands at a fixed
 * instruction count. A hook may drop the BQL while it waits.
 */
void icount_add_sync_hook(void (*fn)(void *opaque), void *opaque);
void icount_run_sync_hooks(void);

#endif /* EXEC_ICOUNT_H */
