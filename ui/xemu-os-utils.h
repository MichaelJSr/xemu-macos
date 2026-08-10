/*
 * OS-specific Helpers
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_OS_UTILS_H
#define XEMU_OS_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

const char *xemu_get_os_info(void);

#ifdef CONFIG_CPUID_H
#include <cpuid.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

static inline const char *xemu_get_cpu_info(void)
{
    const char *cpu_info = "";
#ifdef CONFIG_CPUID_H
    static uint32_t brand[12];
    if (__get_cpuid_max(0x80000004, NULL)) {
        __get_cpuid(0x80000002, brand+0x0, brand+0x1, brand+0x2, brand+0x3);
        __get_cpuid(0x80000003, brand+0x4, brand+0x5, brand+0x6, brand+0x7);
        __get_cpuid(0x80000004, brand+0x8, brand+0x9, brand+0xa, brand+0xb);
    }
    cpu_info = (const char *)brand;
#elif defined(__APPLE__)
    /*
     * Apple Silicon / ARM macOS: use sysctl to read the marketing brand
     * ("Apple M2 Ultra" etc.). Matches the chip-detection pattern in
     * build.sh.
     */
    static char brand[128];
    if (brand[0] == '\0') {
        size_t len = sizeof(brand);
        if (sysctlbyname("machdep.cpu.brand_string", brand, &len, NULL, 0)
            != 0) {
            brand[0] = '\0';
        }
    }
    cpu_info = brand;
#endif
    return cpu_info;
}

#ifdef __cplusplus
}
#endif

#endif
