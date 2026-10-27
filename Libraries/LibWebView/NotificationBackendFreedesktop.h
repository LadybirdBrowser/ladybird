/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/NotificationFreedesktopArguments.h>

struct DBusConnection;
struct DBusMessage;
struct DBusPendingCall;
struct DBusTimeout;
struct DBusWatch;

namespace WebView {

// Shows notifications through the Desktop Notifications daemon on the session bus (org.freedesktop.Notifications),
// over libdbus, with its sockets and timers on the browser's event loop. Without a session bus or a daemon, every show
// fails.
class WEBVIEW_API NotificationBackendFreedesktop final : public NotificationBackend {
public:
    NotificationBackendFreedesktop();
    virtual ~NotificationBackendFreedesktop() override;

    virtual void show(PlatformNotification const&) override;
    virtual void close(u64 id) override;

private:
    bool connect();
    void request_capabilities();
    void did_receive_capabilities(DBusMessage* reply);
    void send_notify(PlatformNotification const&);
    void did_receive_notify_reply(u64 id, DBusMessage* reply);
    void did_receive_signal(DBusMessage*);
    void dispatch();

    static unsigned add_watch(DBusWatch*, void* data);
    static void remove_watch(DBusWatch*, void* data);
    static void toggle_watch(DBusWatch*, void* data);
    static unsigned add_timeout(DBusTimeout*, void* data);
    static void remove_timeout(DBusTimeout*, void* data);
    static void toggle_timeout(DBusTimeout*, void* data);

    DBusConnection* m_connection { nullptr };

    // The daemon names notifications by its own ids, handed out in the Notify reply.
    HashMap<u64, u32> m_daemon_id_for;
    HashMap<u32, u64> m_id_for_daemon_id;

    Optional<FreedesktopCapabilities> m_capabilities;
    bool m_capabilities_requested { false };
    Vector<PlatformNotification> m_waiting_for_capabilities;

    struct Watch {
        DBusWatch* watch { nullptr };
        RefPtr<Core::Notifier> notifier;
    };
    Vector<Watch> m_watches;

    struct Timeout {
        DBusTimeout* timeout { nullptr };
        RefPtr<Core::Timer> timer;
    };
    Vector<Timeout> m_timeouts;
};

}
