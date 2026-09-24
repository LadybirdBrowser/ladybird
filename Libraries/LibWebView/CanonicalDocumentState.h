/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWebView/CanonicalDocument.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-2
// NB: Only the document states of a navigable's active session history entry and of the entry being populated for it
//     are kept, identified as the session history entries identify them.
class CanonicalDocumentState {
public:
    CanonicalDocumentState(Web::HTML::CrossProcessId id, NonnullRefPtr<CanonicalDocument> document)
        : m_id(id)
        , m_document(move(document))
    {
    }

    Web::HTML::CrossProcessId id() const { return m_id; }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-document
    CanonicalDocument& document() const { return m_document; }

private:
    Web::HTML::CrossProcessId m_id;
    NonnullRefPtr<CanonicalDocument> m_document;
};

}
