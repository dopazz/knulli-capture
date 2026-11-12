/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * compat.c - keep knulli-record runnable on an older glibc than the one it
 * was linked against.
 *
 * glibc 2.43 added a new version of sqrtf, which the statically linked x264
 * and Opus libraries call; a binary built with a 2.43 toolchain then refuses
 * to start on a device with glibc 2.40 ("version `GLIBC_2.43' not found").
 * Defining sqrtf here satisfies those calls inside the executable. It is an
 * exact IEEE square root either way: one fsqrt instruction.
 */
float sqrtf(float x);

float sqrtf(float x)
{
#if defined(__aarch64__)
    float r;

    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
#else
    return __builtin_sqrtf(x);
#endif
}
