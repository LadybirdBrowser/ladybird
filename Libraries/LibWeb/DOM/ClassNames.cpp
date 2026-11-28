/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ClassNames.h>
#include <LibWeb/DOM/RustFFI.h>

namespace Web::DOM {

ClassNames::ClassNames(ClassNames&& other)
{
    take_classes_of(other);
}

ClassNames& ClassNames::operator=(ClassNames&& other)
{
    if (this != &other) {
        clear();
        take_classes_of(other);
    }
    return *this;
}

ClassNames::~ClassNames()
{
    // NB: libweb_rust keeps the classes, with a mirror of this layout (see Rust/src/dom/element_core.rs).
    static_assert(sizeof(ClassNames) == 24 && alignof(ClassNames) == 8);
    static_assert(offsetof(ClassNames, m_size) == 0);
    static_assert(offsetof(ClassNames, m_capacity) == 4);
    static_assert(offsetof(ClassNames, m_outline_buffer) == 8);
    static_assert(offsetof(ClassNames, m_inline_class) == 16);
    // NB: Classes that never held one hold no storage.
    if (m_size != 0)
        clear();
}

void ClassNames::take_classes_of(ClassNames& other)
{
    m_size = exchange(other.m_size, 0);
    m_capacity = exchange(other.m_capacity, 1);
    m_outline_buffer = exchange(other.m_outline_buffer, nullptr);
    m_inline_class = exchange(other.m_inline_class, Utf16FlyString {});
}

void ClassNames::append(Utf16FlyString class_name)
{
    RustFFI::rust_dom_class_names_append(this, move(class_name).into_raw());
}

void ClassNames::clear()
{
    RustFFI::rust_dom_class_names_clear(this);
}

}
