/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbstractRange.h>
#include <LibWeb/DOM/Document.h>

namespace Web::DOM {

AbstractRange::AbstractRange(GC::Ref<Node> start_container, WebIDL::UnsignedLong start_offset, GC::Ref<Node> end_container, WebIDL::UnsignedLong end_offset)
    : m_range_core {
        .start_container = start_container,
        .start_offset = start_offset,
        .end_container = end_container,
        .end_offset = end_offset,
        .flags = 0,
    }
{
    // NB: libweb_rust reads the core of a range in place.
    static_assert(offsetof(AbstractRange, m_range_core) == RustFFI::RANGE_CORE_OFFSET);

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(AbstractRange, m_range_core) >= sizeof(Bindings::GCAllocatedWrappable));
}

// NB: The Rust definition of the core asserts the same layout.
static_assert(sizeof(RustFFI::RangeCore) == 20 && alignof(RustFFI::RangeCore) == 4);
static_assert(offsetof(RustFFI::RangeCore, start_container) == 0);
static_assert(offsetof(RustFFI::RangeCore, start_offset) == 4);
static_assert(offsetof(RustFFI::RangeCore, end_container) == 8);
static_assert(offsetof(RustFFI::RangeCore, end_offset) == 12);
static_assert(offsetof(RustFFI::RangeCore, flags) == 16);

AbstractRange::~AbstractRange() = default;

void AbstractRange::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    RustFFI::rust_dom_trace_range_core(&m_range_core, reinterpret_cast<GCVisitor*>(&visitor));
}

}
