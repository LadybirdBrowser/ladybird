/*
 * Copyright (c) 2024, Sönke Holz <sholz8530@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Types.h>

namespace AK {

struct StackFrame {
    FlatPtr return_address;
    FlatPtr previous_frame_pointer;
};

// This function only returns errors if on_stack_frame returns an error.
// It doesn't return an error on failed memory reads, since the last frame record sometimes contains invalid addresses when using frame pointer-based unwinding.
ErrorOr<void> unwind_stack_from_frame_pointer(FlatPtr frame_pointer, CallableAs<ErrorOr<FlatPtr>, FlatPtr> auto read_memory, CallableAs<ErrorOr<IterationDecision>, StackFrame> auto on_stack_frame)
{
    // aarch64/x86_64 frame record layout:
    // fp/rbp+8: return address
    // fp/rbp+0: previous base/frame pointer

    // riscv64 frame record layout:
    // fp-8: return address
    // fp-16: previous frame pointer

#if ARCH(AARCH64) || ARCH(X86_64)
    static constexpr ptrdiff_t FRAME_POINTER_RETURN_ADDRESS_OFFSET = 8;
    static constexpr ptrdiff_t FRAME_POINTER_PREVIOUS_FRAME_POINTER_OFFSET = 0;
#elif ARCH(RISCV64)
    static constexpr ptrdiff_t FRAME_POINTER_RETURN_ADDRESS_OFFSET = -8;
    static constexpr ptrdiff_t FRAME_POINTER_PREVIOUS_FRAME_POINTER_OFFSET = -16;
#else
#    error Unknown architecture
#endif

    FlatPtr current_frame_pointer = frame_pointer;

    while (current_frame_pointer != 0) {
        StackFrame stack_frame;

        auto maybe_return_address = read_memory(current_frame_pointer + FRAME_POINTER_RETURN_ADDRESS_OFFSET);
        if (maybe_return_address.is_error())
            return {};
        stack_frame.return_address = maybe_return_address.value();

        if (stack_frame.return_address == 0)
            return {};

        auto maybe_previous_frame_pointer = read_memory(current_frame_pointer + FRAME_POINTER_PREVIOUS_FRAME_POINTER_OFFSET);
        if (maybe_previous_frame_pointer.is_error())
            return {};
        stack_frame.previous_frame_pointer = maybe_previous_frame_pointer.value();

        if (TRY(on_stack_frame(stack_frame)) == IterationDecision::Break)
            return {};

        current_frame_pointer = maybe_previous_frame_pointer.value();
    }

    return {};
}

}
