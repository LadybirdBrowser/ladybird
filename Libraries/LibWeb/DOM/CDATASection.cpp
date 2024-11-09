/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CDATASectionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/CDATASection.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(CDATASection);

CDATASection::CDATASection(Document& document, String const& data)
    : Text(document, NodeType::CDATA_SECTION_NODE, data)
{
}

CDATASection::~CDATASection() = default;

void CDATASection::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CDATASection);
}

}
