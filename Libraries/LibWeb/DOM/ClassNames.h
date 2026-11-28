/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Span.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// The classes of an element, in order: one in place, which is all most elements with a class have, and all of them in
// an allocation of their own when there are more. libweb_rust changes and frees them (see Rust/src/dom/element_core.rs),
// for C++ too, and mirrors this layout, which ClassNames.cpp asserts.
class WEB_API ClassNames {
    AK_MAKE_NONCOPYABLE(ClassNames);

public:
    ClassNames() = default;
    ClassNames(ClassNames&&);
    ClassNames& operator=(ClassNames&&);
    ~ClassNames();

    ReadonlySpan<Utf16FlyString> span() const { return { m_outline_buffer ? m_outline_buffer : &m_inline_class, m_size }; }
    bool is_empty() const { return m_size == 0; }
    Utf16FlyString const& last() const { return span().last(); }

    void append(Utf16FlyString);
    void clear();

private:
    void take_classes_of(ClassNames&);

    u32 m_size { 0 };
    // NB: The capacity of an element with no outline buffer is the one class in place.
    u32 m_capacity { 1 };
    Utf16FlyString* m_outline_buffer { nullptr };
    Utf16FlyString m_inline_class;
};

}
