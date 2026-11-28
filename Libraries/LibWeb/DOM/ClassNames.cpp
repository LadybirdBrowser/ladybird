/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibWeb/DOM/ClassNames.h>

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
    // NB: libweb_rust reads the classes in place, with a mirror of this layout (see Rust/src/dom/element_core.rs).
    static_assert(sizeof(ClassNames) == 24 && alignof(ClassNames) == 8);
    static_assert(offsetof(ClassNames, m_size) == 0);
    static_assert(offsetof(ClassNames, m_capacity) == 4);
    static_assert(offsetof(ClassNames, m_outline_buffer) == 8);
    static_assert(offsetof(ClassNames, m_inline_class) == 16);
    delete[] m_outline_buffer;
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
    if (!m_outline_buffer && m_size == 0) {
        m_inline_class = move(class_name);
        m_size = 1;
        return;
    }
    if (m_size == m_capacity) {
        Checked<u32> capacity = m_capacity;
        capacity *= 2;
        VERIFY(!capacity.has_overflow());
        auto new_capacity = max(capacity.value(), 4u);
        auto* buffer = new Utf16FlyString[new_capacity];
        auto* classes = m_outline_buffer ? m_outline_buffer : &m_inline_class;
        for (u32 index = 0; index < m_size; ++index)
            buffer[index] = move(classes[index]);
        delete[] m_outline_buffer;
        m_inline_class = Utf16FlyString {};
        m_outline_buffer = buffer;
        m_capacity = new_capacity;
    }
    m_outline_buffer[m_size++] = move(class_name);
}

void ClassNames::clear()
{
    delete[] exchange(m_outline_buffer, nullptr);
    m_inline_class = Utf16FlyString {};
    m_size = 0;
    m_capacity = 1;
}

}
