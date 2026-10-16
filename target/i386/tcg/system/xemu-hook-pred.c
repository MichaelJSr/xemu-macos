/*
 * xemu guest PC hooks: conditions (QAPI XemuHookCond), compiled once and
 * tested at each arrival (see tcg/xemu-hooks.h). Shared by xemu-run-until
 * and the xemu-hooks-set hooks.
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qapi/qapi-types-xemu.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "fpu/softfloat.h"
#include "cpu.h"
#include "tcg/xemu-hooks.h"

QEMU_BUILD_BUG_ON(XEMU_HOOK_REG_EAX != R_EAX || XEMU_HOOK_REG_EDI != R_EDI ||
                  XEMU_HOOK_REG_EIP != XEMU_HREG_EIP ||
                  XEMU_HOOK_REG_EFLAGS != XEMU_HREG_EFLAGS);

bool xemu_hook_pred_test(XemuHookCtx *ctx, const XemuHookPred *c)
{
    uint64_t raw;
    int size = c->size;

    if (c->reg >= 0) {
        raw = xemu_hook_reg(ctx, c->reg);
    } else {
        uint32_t a;
        uint8_t b[8];

        if (!xemu_hook_expr_eval(ctx, &c->addr, &a) ||
            !xemu_hook_read(ctx->env, a, b, size)) {
            return false;
        }
        raw = xemu_hook_pred_raw(b, size);
    }
    return xemu_hook_pred_compare(c, raw);
}

uint64_t xemu_hook_pred_raw(const uint8_t *b, int size)
{
    return size == 1 ? b[0] : size == 2 ? lduw_le_p(b)
         : size == 4 ? (uint32_t)ldl_le_p(b) : ldq_le_p(b);
}

bool xemu_hook_pred_compare(const XemuHookPred *c, uint64_t raw)
{
    int size = c->size;
    int rel;

    switch (c->type) {
    case XEMU_HOOK_TYPE_S: {
        int64_t v = size == 8 ? (int64_t)raw : sextract64(raw, 0, size * 8);
        rel = v < c->s ? -1 : v > c->s;
        break;
    }
    case XEMU_HOOK_TYPE_F: {
        /* Softfloat on a local status: the host FPCR holds the guest's. */
        float_status st = { 0 };
        float64 v;

        if (size == 4) {
            if (float32_is_any_nan(make_float32(raw))) {
                return c->op == XEMU_HOOK_CMP_NE;
            }
            v = float32_to_float64(make_float32(raw), &st);
        } else {
            v = make_float64(raw);
            if (float64_is_any_nan(v)) {
                return c->op == XEMU_HOOK_CMP_NE;
            }
        }
        rel = float64_compare_quiet(v, make_float64(c->f), &st);
        break;
    }
    default:
        rel = raw < c->u ? -1 : raw > c->u;
        break;
    }

    switch (c->op) {
    case XEMU_HOOK_CMP_EQ:
        return rel == 0;
    case XEMU_HOOK_CMP_NE:
        return rel != 0;
    case XEMU_HOOK_CMP_LT:
        return rel < 0;
    case XEMU_HOOK_CMP_GT:
        return rel > 0;
    case XEMU_HOOK_CMP_LE:
        return rel <= 0;
    default:
        return rel >= 0;
    }
}

static bool pred_value(QObject *o, XemuHookPred *c, Error **errp)
{
    QNum *qn = qobject_to(QNum, o);
    QString *qs = qobject_to(QString, o);
    int bits = c->size * 8;
    enum { V_INT, V_UINT, V_DBL } kind;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;

    if (qn) {
        if (qnum_get_try_int(qn, &i)) {
            kind = V_INT;
        } else if (qnum_get_try_uint(qn, &u)) {
            kind = V_UINT;
        } else {
            d = qnum_get_double(qn);
            kind = V_DBL;
        }
    } else if (qs) {
        const char *str = qstring_get_str(qs);

        if (!qemu_strtoi64(str, NULL, 0, &i)) {
            kind = V_INT;
        } else if (!qemu_strtou64(str, NULL, 0, &u)) {
            kind = V_UINT;
        } else if (!qemu_strtod_finite(str, NULL, &d)) {
            kind = V_DBL;
        } else {
            error_setg(errp, "value '%s' is not a number", str);
            return false;
        }
    } else {
        error_setg(errp, "a condition's value must be a number");
        return false;
    }

    if (c->type == XEMU_HOOK_TYPE_F) {
        union { double d; uint64_t u; } x;

        x.d = kind == V_INT ? (double)i : kind == V_UINT ? (double)u : d;
        c->f = x.u;
        return true;
    }
    if (kind == V_DBL) {
        if (d != floor(d) || d < -0x1p63 || d >= 0x1p64) {
            error_setg(errp, "value %g is not an integer", d);
            return false;
        }
        if (d < 0) {
            i = d;
            kind = V_INT;
        } else {
            u = d;
            kind = V_UINT;
        }
    }
    if (c->type == XEMU_HOOK_TYPE_U) {
        if (kind == V_INT) {
            if (i < 0) {
                error_setg(errp, "negative value for an unsigned compare");
                return false;
            }
            u = i;
        }
        if (bits < 64 && u >> bits) {
            error_setg(errp, "value 0x%" PRIx64 " doesn't fit in %d bytes",
                       u, c->size);
            return false;
        }
        c->u = u;
        return true;
    }
    if (kind == V_UINT) {
        if (u > INT64_MAX) {
            error_setg(errp, "value %" PRIu64 " out of signed range", u);
            return false;
        }
        i = u;
    }
    if (bits < 64 && (i < -(INT64_C(1) << (bits - 1)) ||
                      i >= (INT64_C(1) << (bits - 1)))) {
        error_setg(errp, "value %" PRId64 " doesn't fit in %d signed bytes",
                   i, c->size);
        return false;
    }
    c->s = i;
    return true;
}

bool xemu_hook_pred_compile(XemuHookCond *in, XemuHookPred *c,
                            bool *reads_memory, Error **errp)
{
    if (in->has_reg == !!in->addr) {
        error_setg(errp, "a condition names exactly one of 'reg' and 'addr'");
        return false;
    }
    c->type = in->has_type ? in->type : XEMU_HOOK_TYPE_U;
    c->op = in->op;
    if (in->has_reg) {
        if (in->has_size && in->size != 4) {
            error_setg(errp, "a register condition has size 4");
            return false;
        }
        c->reg = in->reg;
        c->size = 4;
    } else {
        c->reg = -1;
        c->size = in->has_size ? in->size : 4;
        if (c->size != 1 && c->size != 2 && c->size != 4 && c->size != 8) {
            error_setg(errp, "size must be 1, 2, 4 or 8");
            return false;
        }
        if (!xemu_hook_expr_parse(in->addr, &c->addr, errp)) {
            return false;
        }
        *reads_memory = true;
    }
    if (c->type == XEMU_HOOK_TYPE_F && c->size != 4 && c->size != 8) {
        error_setg(errp, "a float condition has size 4 or 8");
        return false;
    }
    return pred_value(in->value, c, errp);
}
