/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibURL/URL.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API CanonicalNavigable final {
public:
    enum class HostLocality : u8 {
        Local,
        Remote,
    };

    struct PendingNavigation {
        URL::URL target_url;
        HostLocality target_locality { HostLocality::Local };
        Optional<u64> remote_page_id;
    };

    CanonicalNavigable(String id, String parent_id);
    ~CanonicalNavigable();

    String const& id() const { return m_id; }
    String const& parent_id() const { return m_parent_id; }

    bool has_remote_host() const { return m_host_locality == HostLocality::Remote && m_remote_client && m_remote_page_id != 0; }
    WebContentClient& remote_host_client() const;
    u64 remote_host_page_id() const { return m_remote_page_id; }

    void set_remote_host(NonnullRefPtr<WebContentClient>, u64 remote_page_id);
    void detach_remote_host();

    Optional<Web::DevicePixelRect> const& viewport_rect() const { return m_viewport_rect; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_viewport(Web::DevicePixelRect, double device_pixel_ratio);

    void did_commit_navigation(URL::URL);
    Optional<URL::URL> document_url() const;

    void record_pending_navigation(URL::URL const&, HostLocality, Optional<u64> remote_page_id = {});
    void clear_pending_navigation() { m_pending_navigation.clear(); }
    bool has_matching_pending_navigation(URL::URL const&, HostLocality) const;

private:
    String m_id;
    String m_parent_id;

    Optional<URL::URL> m_last_committed_url;
    Optional<PendingNavigation> m_pending_navigation;
    Optional<Web::DevicePixelRect> m_viewport_rect;
    double m_device_pixel_ratio { 1 };

    HostLocality m_host_locality { HostLocality::Local };
    RefPtr<WebContentClient> m_remote_client;
    u64 m_remote_page_id { 0 };
};

}
