/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibURL/Origin.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context
class WEBVIEW_API CanonicalBrowsingContext final : public RefCounted<CanonicalBrowsingContext> {
public:
    // A WebContent process creates the document of a new browsing context. The UI process models the browsing context
    // and the similar-origin window agent of that document, given the document's origin and the process creating it.
    static NonnullRefPtr<CanonicalBrowsingContext> create_a_new_browsing_context_and_document(CanonicalBrowsingContextGroup&, URL::Origin const& document_origin, Optional<WebContentClient&> document_process);
    static NonnullRefPtr<CanonicalBrowsingContext> create_a_new_top_level_browsing_context_and_document(URL::Origin const& document_origin, Optional<WebContentClient&> document_process);
    static NonnullRefPtr<CanonicalBrowsingContext> create_a_new_auxiliary_browsing_context_and_document(CanonicalNavigable& opener, URL::Origin const& document_origin, Optional<WebContentClient&> document_process);

    ~CanonicalBrowsingContext();

    RefPtr<CanonicalBrowsingContextGroup> group() const;
    void set_group(Badge<CanonicalBrowsingContextGroup>, CanonicalBrowsingContextGroup*);

private:
    CanonicalBrowsingContext() = default;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tlbc-group
    RefPtr<CanonicalBrowsingContextGroup> m_group;
};

}
