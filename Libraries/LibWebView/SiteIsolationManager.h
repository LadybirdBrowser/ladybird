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
#include <LibURL/URL.h>
#include <LibWeb/Page/PageId.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API SiteIsolationManager {
public:
    static SiteIsolationManager& the();

    struct RemoteChildFrameInputTarget {
        RefPtr<WebContentClient> remote_client;
        Web::PageId remote_page_id { 0 };
        CanonicalNavigable const* navigable { nullptr };
        Optional<Web::Compositor::CompositorContextId> compositor_context_id;
        Web::DevicePixelRect viewport_rect;
    };

    [[nodiscard]] bool top_level_navigation_requires_process_swap(CanonicalBrowsingContext const&, URL::URL const& current_url, URL::URL const& target_url) const;

    struct DocumentHost {
        NonnullRefPtr<WebContentClient> client;
        Web::PageId page_id;
    };
    ErrorOr<DocumentHost> obtain_child_document_host(CanonicalNavigable&, CanonicalSimilarOriginWindowAgent&);
    void set_child_document_host(CanonicalNavigable&, DocumentHost const&);

    void transition_child_frame_to_remote(WebContentClient& parent_client, Web::PageId page_id, Web::HTML::CrossProcessId frame_id, NonnullRefPtr<WebContentClient>, Web::PageId remote_page_id);
    void transition_child_frame_to_local(CanonicalNavigable&);

    void remove_child_frame_subtree(CanonicalNavigable&);

    void remove_page(WebContentClient&, Web::PageId page_id);
    void remove_all_pages_for_client(WebContentClient&);

    // The remote child under a local root of a page at a position in that root's coordinates, if any.
    Optional<RemoteChildFrameInputTarget> remote_child_frame_input_target_at(WebContentClient&, Web::PageId page_id, CanonicalNavigable const& root, Web::DevicePixelPoint) const;
    String dump_process_tree(WebContentClient&, Web::PageId page_id) const;
    HashMap<pid_t, pid_t> remote_frame_process_embedders() const;

private:
    SiteIsolationManager() = default;
};

}
