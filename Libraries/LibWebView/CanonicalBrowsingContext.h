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
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
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
    RefPtr<CanonicalBrowsingContext> opener_browsing_context() const { return m_opener_browsing_context; }
    void set_opener_browsing_context(RefPtr<CanonicalBrowsingContext>);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#virtual-browsing-context-group-id
    u64 virtual_browsing_context_group_id() const { return m_virtual_browsing_context_group_id; }

    // https://html.spec.whatwg.org/multipage/browsers.html#popup-sandboxing-flag-set
    Web::HTML::SandboxingFlagSet popup_sandboxing_flag_set() const { return m_popup_sandboxing_flag_set; }
    void set_popup_sandboxing_flag_set(Web::HTML::SandboxingFlagSet flags) { m_popup_sandboxing_flag_set = flags; }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#bcg-remove
    void remove();

    RefPtr<CanonicalBrowsingContextGroup> group() const;
    void set_group(Badge<CanonicalBrowsingContextGroup>, CanonicalBrowsingContextGroup*);

private:
    CanonicalBrowsingContext() = default;

    WeakPtr<CanonicalDocument> m_active_document;

    // NB: The [[Window]] internal slot value of the browsing context's WindowProxy.
    RefPtr<CanonicalWindow> m_window_proxy_window;

    RefPtr<CanonicalBrowsingContext> m_top_level_browsing_context;

    bool m_is_auxiliary { false };

    // NB: A browsing context keeps its opener browsing context once that is discarded: the opener's window is closed
    //     then, and still the opener.
    RefPtr<CanonicalBrowsingContext> m_opener_browsing_context;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#opener-origin-at-creation
    Optional<URL::Origin> m_opener_origin_at_creation;

    u64 m_virtual_browsing_context_group_id { 0 };

    Web::HTML::SandboxingFlagSet m_popup_sandboxing_flag_set {};

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tlbc-group
    RefPtr<CanonicalBrowsingContextGroup> m_group;
};

WEBVIEW_API Web::HTML::SandboxingFlagSet determine_the_creation_sandboxing_flags(CanonicalBrowsingContext const&, Optional<Web::HTML::ReplicatedContainerState const&> embedder);

}
