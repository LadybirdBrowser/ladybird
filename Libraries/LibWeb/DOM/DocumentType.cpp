/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentType.h>

namespace Web::DOM {

static_assert(is_shell_of_rust_object<DocumentType, RustFFI::DocumentTypeObject>);

GC::Ref<DocumentType> DocumentType::create(Document& document)
{
    return static_cast<DocumentType&>(*RustFFI::rust_dom_create_document_type(&document));
}

DocumentType::DocumentType(VtableProbe probe)
    : Node(probe)
{
    static_assert(offsetof(DocumentType, m_document_type_core) == offsetof(RustFFI::DocumentTypeObject, core));
}

// https://dom.spec.whatwg.org/#valid-doctype-name
bool is_valid_doctype_name(Utf16View const& name)
{
    // A string is a valid doctype name if it does not contain ASCII whitespace, U+0000 NULL, or U+003E (>).
    constexpr Array<u32, 7> INVALID_DOCTYPE_CHARACTERS { '\t', '\n', '\f', '\r', ' ', '\0', '>' };
    return !name.contains_any_of(INVALID_DOCTYPE_CHARACTERS);
}

}
