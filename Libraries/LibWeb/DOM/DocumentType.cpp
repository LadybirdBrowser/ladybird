/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DocumentTypePrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentType.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(DocumentType);

JS::NonnullGCPtr<DocumentType> DocumentType::create(Document& document)
{
    return document.heap().allocate<DocumentType>(document.realm(), document);
}

DocumentType::DocumentType(Document& document)
    : Node(document, NodeType::DOCUMENT_TYPE_NODE)
{
}

void DocumentType::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DocumentType);
}

}
