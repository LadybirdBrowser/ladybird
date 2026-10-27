/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibWebView/NotificationBackend.h>

namespace WebView {

namespace Detail {

struct NotificationCenterBridge {
    AK_ALLOC_WITH_KMALLOC;
    void* delegate { nullptr };
    void* center { nullptr };
};

}

// Shows notifications through the user notification center. Needs a bundled application: Outside one there is no
// notification center to ask, and every show fails.
class WEBVIEW_API NotificationBackendMacOS final : public NotificationBackend {
public:
    NotificationBackendMacOS();
    virtual ~NotificationBackendMacOS() override;

    virtual void show(PlatformNotification const&) override;
    virtual void close(u64 id) override;

    // What the notification center's delegate reports: the user activated or dismissed a request.
    void did_activate(String const& identifier);
    void did_dismiss(String const& identifier);

private:
    void request_authorization_then(Function<void(bool granted)>);

    // The notification center knows requests by a string identifier. A replacement keeps the identifier of the
    // notification it replaces, so the center swaps it in place; these maps keep track of which id holds it now.
    HashMap<String, u64> m_id_for_identifier;
    HashMap<u64, String> m_identifier_for_id;

    enum class Authorization {
        Unknown,
        Pending,
        Granted,
        Denied,
    };
    Authorization m_authorization { Authorization::Unknown };
    Vector<Function<void(bool)>> m_waiting_for_authorization;

    OwnPtr<Detail::NotificationCenterBridge> m_bridge;
    bool m_bundled { false };
};

}
