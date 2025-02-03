/*
 * Copyright (c) 2024, circl <circl.lastname@gmail.com>
 * Copyright (c) 2025, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Utils.h>

namespace Web::DOM {

template<typename T>
concept DocumentOrShadowRoot = OneOf<T, Document, ShadowRoot>;

// https://html.spec.whatwg.org/multipage/interaction.html#dom-documentorshadowroot-activeelement
template<DocumentOrShadowRoot T>
GC::Ptr<Element> calculate_active_element(T& self)
{
    // 1. Let candidate be this's node document's focused area's DOM anchor.
    Node* candidate = self.document().focused_area();

    // 2. Set candidate to the result of retargeting candidate against this.
    candidate = as<Node>(retarget(candidate, &self));

    // 3. If candidate's root is not this, then return null.
    if (&candidate->root() != &self)
        return nullptr;

    // 4. If candidate is not a Document object, then return candidate.
    if (!is<Document>(candidate))
        return as<Element>(candidate);

    auto* candidate_document = as_if<Document>(candidate);

    // 5. If candidate has a body element, then return that body element.
    if (auto* body = candidate_document->body())
        return body;

    // 6. If candidate's document element is non-null, then return that document element.
    if (auto* document_element = candidate_document->document_element())
        return document_element;

    // 7. Return null.
    return nullptr;
}

}
