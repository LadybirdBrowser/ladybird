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
#include <AK/WeakPtr.h>
#include <LibURL/Origin.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context
class WEBVIEW_API CanonicalBrowsingContext final : public RefCounted<CanonicalBrowsingContext> {
public:
    struct BrowsingContextAndDocument {
        NonnullRefPtr<CanonicalBrowsingContext> browsing_context;
        NonnullRefPtr<CanonicalDocument> document;
    };

    static BrowsingContextAndDocument create_a_new_browsing_context_and_document(CanonicalBrowsingContextGroup&, URL::Origin const& document_origin, Optional<WebContentClient&> document_process);
    static BrowsingContextAndDocument create_a_new_top_level_browsing_context_and_document(URL::Origin const& document_origin, Optional<WebContentClient&> document_process);
    static BrowsingContextAndDocument create_a_new_auxiliary_browsing_context_and_document(CanonicalNavigable& opener, URL::Origin const& document_origin, Optional<WebContentClient&> document_process);

    ~CanonicalBrowsingContext();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#active-document
    NonnullRefPtr<CanonicalDocument> active_document() const;
    void set_active_document(Badge<CanonicalDocument>, CanonicalDocument&);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#active-window
    CanonicalWindow& active_window() const { return *m_window_proxy_window; }
    void set_active_window(Badge<CanonicalDocument>, CanonicalWindow&);

    RefPtr<CanonicalBrowsingContextGroup> group() const;
    void set_group(Badge<CanonicalBrowsingContextGroup>, CanonicalBrowsingContextGroup*);

private:
    CanonicalBrowsingContext() = default;

    WeakPtr<CanonicalDocument> m_active_document;

    // NB: The [[Window]] internal slot value of the browsing context's WindowProxy.
    RefPtr<CanonicalWindow> m_window_proxy_window;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tlbc-group
    RefPtr<CanonicalBrowsingContextGroup> m_group;
};

}
