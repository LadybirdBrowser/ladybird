/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Forward.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibCompositing/DisplayList/DisplayListCommandsGenerated.h>
#include <LibGfx/Rect.h>

namespace Compositing {

constexpr i64 caret_blink_interval_ns = 500'000'000;
inline bool caret_is_visible_at_time(PaintCaret const& caret, i64 monotonic_time_ns)
{
    if (!caret.should_blink)
        return true;
    auto elapsed_ns = monotonic_time_ns > caret.blink_cycle_start_time_ns
        ? monotonic_time_ns - caret.blink_cycle_start_time_ns
        : 0;
    return (elapsed_ns / caret_blink_interval_ns) % 2 == 0;
}

template<typename Command>
concept DisplayListCommand = requires {
    Command::command_type;
};

template<typename T>
requires(IsTriviallyCopyable<T>)
ReadonlyBytes display_list_object_bytes(T const& object)
{
    return { &object, sizeof(T) };
}

template<typename T>
requires(IsTriviallyCopyable<T>)
T read_display_list_object(ReadonlyBytes bytes)
{
    VERIFY(bytes.size() >= sizeof(T));
    T object;
    __builtin_memcpy(&object, bytes.data(), sizeof(T));
    return object;
}

static_assert(IsTriviallyCopyable<DisplayListCommandHeader>);
static_assert(sizeof(DisplayListCommandHeader) == 24);
static_assert(IsTriviallyCopyable<DisplayListCommandRun>);
static_assert(sizeof(DisplayListCommandRun) == 40);
static_assert(IsTriviallyCopyable<DisplayListGlyph>);
static_assert(IsTriviallyCopyable<TextShadowLayer>);
static_assert(IsTriviallyCopyable<DisplayListInlineClip>);
static_assert(sizeof(DisplayListInlineClip) == 64);
static_assert(IsTriviallyCopyable<DisplayListInlineTransform>);
static_assert(sizeof(DisplayListInlineTransform) == 32);

inline bool operator==(DisplayListCommandRun const& a, DisplayListCommandRun const& b)
{
    return a.offset == b.offset
        && a.size == b.size
        && a.context == b.context
        && a.ink_bounds == b.ink_bounds
        && a.has_unbounded_draw == b.has_unbounded_draw
        && a.has_compositor_metadata == b.has_compositor_metadata;
}

#define VERIFY_DISPLAY_LIST_COMMAND(command)     \
    static_assert(IsTriviallyCopyable<command>); \
    static_assert(alignof(command) <= display_list_payload_alignment);
ENUMERATE_DISPLAY_LIST_COMMANDS(VERIFY_DISPLAY_LIST_COMMAND)
#undef VERIFY_DISPLAY_LIST_COMMAND

}
