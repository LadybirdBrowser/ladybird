/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StaticRangePrototype.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(StaticRange);

StaticRange::StaticRange(Node& start_container, u32 start_offset, Node& end_container, u32 end_offset)
    : AbstractRange(start_container, start_offset, end_container, end_offset)
{
}

StaticRange::~StaticRange() = default;

// https://dom.spec.whatwg.org/#dom-staticrange-staticrange
WebIDL::ExceptionOr<JS::NonnullGCPtr<StaticRange>> StaticRange::construct_impl(JS::Realm& realm, StaticRangeInit& init)
{
    // 1. If init["startContainer"] or init["endContainer"] is a DocumentType or Attr node, then throw an "InvalidNodeTypeError" DOMException.
    if (is<DocumentType>(*init.start_container) || is<Attr>(*init.start_container))
        return WebIDL::InvalidNodeTypeError::create(realm, "startContainer cannot be a DocumentType or Attribute node."_fly_string);

    if (is<DocumentType>(*init.end_container) || is<Attr>(*init.end_container))
        return WebIDL::InvalidNodeTypeError::create(realm, "endContainer cannot be a DocumentType or Attribute node."_fly_string);

    // 2. Set this’s start to (init["startContainer"], init["startOffset"]) and end to (init["endContainer"], init["endOffset"]).
    return realm.heap().allocate<StaticRange>(realm, *init.start_container, init.start_offset, *init.end_container, init.end_offset);
}

void StaticRange::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StaticRange);
}

}
