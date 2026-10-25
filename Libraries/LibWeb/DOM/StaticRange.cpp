/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/StaticRange.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(StaticRange);

StaticRange::StaticRange(Node& start_container, u32 start_offset, Node& end_container, u32 end_offset)
    : AbstractRange(start_container, start_offset, end_container, end_offset)
{
}

StaticRange::~StaticRange() = default;

// https://dom.spec.whatwg.org/#dom-staticrange-staticrange
WebIDL::ExceptionOr<GC::Ref<StaticRange>> StaticRange::create(Bindings::StaticRangeInit const& init)
{
    // 1. If init["startContainer"] or init["endContainer"] is a DocumentType or Attr node, then throw an "InvalidNodeTypeError" DOMException.
    if (is<DocumentType>(*init.start_container) || is<Attr>(*init.start_container))
        return WebIDL::InvalidNodeTypeError::create("startContainer cannot be a DocumentType or Attribute node."_utf16);

    if (is<DocumentType>(*init.end_container) || is<Attr>(*init.end_container))
        return WebIDL::InvalidNodeTypeError::create("endContainer cannot be a DocumentType or Attribute node."_utf16);

    // 2. Set this’s start to (init["startContainer"], init["startOffset"]) and end to (init["endContainer"], init["endOffset"]).
    return GC::Heap::the().allocate<StaticRange>(*init.start_container, init.start_offset, *init.end_container, init.end_offset);
}

// https://dom.spec.whatwg.org/#staticrange-valid
bool StaticRange::is_valid() const
{
    // A StaticRange is valid if all of the following are true:

    // - Its start and end are in the same node tree.
    if (&start_container()->root() != &end_container()->root())
        return false;

    // - Its start offset is between 0 and its start node’s length, inclusive.
    if (start_offset() > start_container()->length())
        return false;

    // - Its end offset is between 0 and its end node’s length, inclusive.
    if (end_offset() > end_container()->length())
        return false;

    // - Its start is before or equal to its end.
    return position_of_boundary_point_relative_to_other_boundary_point(start(), end()) != RelativeBoundaryPointPosition::After;
}

}
