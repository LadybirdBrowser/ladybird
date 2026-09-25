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
#include <AK/Weakable.h>
#include <LibURL/Origin.h>
#include <LibWeb/Forward.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context
class WEBVIEW_API CanonicalBrowsingContext final
    : public RefCounted<CanonicalBrowsingContext>
    , public Weakable<CanonicalBrowsingContext> {
public:
    struct BrowsingContextAndDocument {
        NonnullRefPtr<CanonicalBrowsingContext> browsing_context;
        NonnullRefPtr<CanonicalDocument> document;
    };

    static BrowsingContextAndDocument create_a_new_browsing_context_and_document(CanonicalDocument const* creator, Optional<Web::HTML::ReplicatedContainerState const&> embedder, CanonicalBrowsingContextGroup&);
    static BrowsingContextAndDocument create_a_new_top_level_browsing_context_and_document();
    static BrowsingContextAndDocument create_a_new_auxiliary_browsing_context_and_document(CanonicalNavigable& opener);

    ~CanonicalBrowsingContext();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#active-document
    NonnullRefPtr<CanonicalDocument> active_document() const;
    void set_active_document(Badge<CanonicalDocument>, CanonicalDocument&);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#active-window
    CanonicalWindow& active_window() const { return *m_window_proxy_window; }
    void set_active_window(Badge<CanonicalDocument>, CanonicalWindow&);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#bc-tlbc
    CanonicalBrowsingContext& top_level_browsing_context();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-auxiliary
    bool is_auxiliary() const { return m_is_auxiliary; }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#opener-browsing-context
    RefPtr<CanonicalBrowsingContext> opener_browsing_context() const { return m_opener_browsing_context.strong_ref(); }
    void set_opener_browsing_context(RefPtr<CanonicalBrowsingContext>);

    RefPtr<CanonicalBrowsingContextGroup> group() const;
    void set_group(Badge<CanonicalBrowsingContextGroup>, CanonicalBrowsingContextGroup*);

private:
    CanonicalBrowsingContext() = default;

    WeakPtr<CanonicalDocument> m_active_document;

    // NB: The [[Window]] internal slot value of the browsing context's WindowProxy.
    RefPtr<CanonicalWindow> m_window_proxy_window;

    RefPtr<CanonicalBrowsingContext> m_top_level_browsing_context;

    bool m_is_auxiliary { false };

    WeakPtr<CanonicalBrowsingContext> m_opener_browsing_context;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tlbc-group
    RefPtr<CanonicalBrowsingContextGroup> m_group;
};

}
