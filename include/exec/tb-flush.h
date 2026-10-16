/*
 * tb-flush prototype for use by the rest of the system.
 *
 * Copyright (c) 2022 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _TB_FLUSH_H_
#define _TB_FLUSH_H_

/**
 * tb_flush__exclusive_or_serial()
 *
 * Used to flush all the translation blocks in the system.  Mostly this is
 * used to empty the code generation buffer after it is full.  Sometimes it
 * is used when it is simpler to flush everything than work out which
 * individual translations are now invalid.
 *
 * Must be called from an exclusive or serial context, e.g. start_exclusive,
 * vm_stop, or when there is only one vcpu.  Note that start_exclusive cannot
 * be called from within the cpu run loop, so this cannot be called from
 * within target code.
 */
void tb_flush__exclusive_or_serial(void);

/**
 * queue_tb_flush() - add flush to the cpu work queue
 * @cs: CPUState
 *
 * Flush all translation blocks the next time @cs processes the work queue.
 * This should generally be followed by cpu_loop_exit(), so that the work
 * queue is processed promptly.
 */
void queue_tb_flush(CPUState *cs);

void tcg_flush_jmp_cache(CPUState *cs);

#ifdef XBOX
/*
 * xemu_tb_revalidate_after_load() - after a successful load, drop the
 * translations whose guest bytes changed and re-protect the code pages
 * that keep some. Same calling context as tb_flush__exclusive_or_serial().
 */
void xemu_tb_revalidate_after_load(void);
/*
 * xemu_tb_revalidate_pages() - the same after an incremental checkpoint
 * restore, visiting only the TBs on the ram_addr pages set in @scope
 * (@pages bits), the pages anything wrote since the last check, plus every
 * XEMU_TB_PEEKS TB. XEMU_TB_REVAL_REFUTE=1 also runs the full check and
 * reports what only it would do.
 */
void xemu_tb_revalidate_pages(const unsigned long *scope, uint64_t pages);
#endif

#endif /* _TB_FLUSH_H_ */
