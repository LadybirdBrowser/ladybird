/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentType.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(DocumentType);

GC::Ref<DocumentType> DocumentType::create(Document& document)
{
    return GC::Heap::the().allocate<DocumentType>(document);
}

DocumentType::DocumentType(Document& document)
    : Node(document, NodeType::DOCUMENT_TYPE_NODE)
{
}

// https://dom.spec.whatwg.org/#valid-doctype-name
bool is_valid_doctype_name(String const& name)
{
    // A string is a valid doctype name if it does not contain ASCII whitespace, U+0000 NULL, or U+003E (>).
    constexpr Array<u32, 7> INVALID_DOCTYPE_CHARACTERS { '\t', '\n', '\f', '\r', ' ', '\0', '>' };
    return !name.code_points().contains_any_of(INVALID_DOCTYPE_CHARACTERS);
}

}
