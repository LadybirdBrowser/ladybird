/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>

extern "C" [[gnu::visibility("default"), gnu::naked, noreturn]] void crash_with_unresolved_frames(size_t);

// Terminate the frame chain inside this library while retaining the caller's
// return address on the stack. Each recursive call adds one unresolved frame.
void crash_with_unresolved_frames(size_t)
{
    asm volatile(
        "endbr64\n"
        "xor %ebp, %ebp\n"
        "1:\n"
        "push %rbp\n"
        "mov %rsp, %rbp\n"
        "sub $1, %rdi\n"
        "jz 2f\n"
        "call 1b\n"
        "2:\n"
        "ud2\n");
}
