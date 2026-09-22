/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/kmalloc.h>
#include <LibCompositing/PageId.h>
#include <LibCompositing/PixelUnits.h>
#include <LibURL/URL.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

class WEBVIEW_API SiteIsolationManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static SiteIsolationManager& the();

    [[nodiscard]] bool top_level_navigation_requires_process_swap(CanonicalBrowsingContext const&, URL::URL const& current_url, URL::URL const& target_url) const;

    ErrorOr<NonnullRefPtr<WebContentPage>> obtain_child_document_host(CanonicalNavigable&, CanonicalSimilarOriginWindowAgent&);
    void host_opaque_origin_agent_with_initiator(CanonicalBrowsingContextGroup&, CanonicalSimilarOriginWindowAgent&, URL::Origin const& origin, Optional<URL::Origin> const& initiator_origin);
    void set_child_document_host(CanonicalNavigable&, WebContentPage&);

    void transition_child_frame_to_remote(WebContentPage& parent_page, Web::HTML::CrossProcessId frame_id, NonnullRefPtr<WebContentPage> remote_page);
    void transition_child_frame_to_local(CanonicalNavigable&);
    void detach_child_frame_host(CanonicalNavigable&);

    void remove_child_frame_subtree(CanonicalNavigable&);

    void remove_page(WebContentPage&);
    void remove_all_pages_for_client(WebContentClient&);

    String dump_process_tree(WebContentClient&, Compositing::PageId page_id) const;
    HashMap<pid_t, pid_t> remote_frame_process_embedders() const;

private:
    SiteIsolationManager() = default;
};

}
