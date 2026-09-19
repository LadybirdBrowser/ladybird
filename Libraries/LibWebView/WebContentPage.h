/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/Page/PageId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API WebContentPage final : public RefCounted<WebContentPage> {
    AK_MAKE_NONCOPYABLE(WebContentPage);
    AK_MAKE_NONMOVABLE(WebContentPage);
    AK_ALLOC_WITH_KMALLOC;

    friend class WebContentClient;

public:
    WebContentPage(WebContentClient&, Web::PageId, CanonicalTraversable&, ViewImplementation*);
    ~WebContentPage();

    WebContentClient& client() const;
    Web::PageId id() const { return m_id; }

    // False once the page can no longer host work: the page is unregistered or the process is gone. A page
    // awaiting a detached close remains open; it still coordinates its own close.
    bool is_open() const { return m_is_open && m_traversable; }
    bool is_live() const;
    void close();

    CanonicalTraversable* traversable() const;
    Optional<ViewImplementation&> view() const;
    Optional<ViewImplementation&> owning_view() const;
    Optional<CanonicalNavigable&> hosted_navigable(Web::HTML::CrossProcessId) const;

    bool needs_beforeunload_check() const { return m_needs_beforeunload_check; }
    void set_needs_beforeunload_check(bool needs_beforeunload_check) { m_needs_beforeunload_check = needs_beforeunload_check; }
    bool detached_close_pending() const { return m_detached_close_pending; }
    void set_detached_close_pending(bool detached_close_pending) { m_detached_close_pending = detached_close_pending; }
    void clear_history_recorded_url_for_current_load() { m_history_recorded_url_for_current_load.clear(); }

private:
    WeakPtr<WebContentClient> m_client;
    Web::PageId m_id { 0 };
    WeakPtr<CanonicalTraversable> m_traversable;
    ViewImplementation* m_view { nullptr };
    bool m_is_open { true };
    bool m_needs_beforeunload_check { true };
    bool m_detached_close_pending { false };
    Optional<String> m_history_recorded_url_for_current_load;
};

}
