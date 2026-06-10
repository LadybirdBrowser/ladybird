/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGNumber.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGNumber);

GC::Ref<SVGNumber> SVGNumber::create(float value, ReadOnly read_only)
{
    return GC::Heap::the().allocate<SVGNumber>(value, read_only);
}

SVGNumber::SVGNumber(float value, ReadOnly read_only)
    : m_value(value)
    , m_read_only(read_only)
{
}

// https://www.w3.org/TR/SVG2/types.html#__svg__SVGNumber__value
WebIDL::ExceptionOr<void> SVGNumber::set_value(float value)
{
    // 1. If the SVGNumber is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify value of read-only SVGNumber"_utf16);

    // 2. Set the SVGNumber's value to the value being assigned to the value member.
    m_value = value;

    // FIXME: 3. If the SVGNumber reflects an element of the base value of a reflected attribute, then reserialize the
    //    reflected attribute.

    return {};
}

}
