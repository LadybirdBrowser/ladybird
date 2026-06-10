/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/CDATASection.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(CDATASection);

CDATASection::CDATASection(Document& document, Utf16String data)
    : Text(document, NodeType::CDATA_SECTION_NODE, move(data))
{
}

GC::Ref<CDATASection> CDATASection::create(Document& document, Utf16String data)
{
    return GC::Heap::the().allocate<CDATASection>(document, move(data));
}

CDATASection::~CDATASection() = default;

}
