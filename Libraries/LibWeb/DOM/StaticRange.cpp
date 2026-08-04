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

}
