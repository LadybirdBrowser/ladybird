/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Notification.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::NotificationsAPI {

// https://notifications.spec.whatwg.org/#notifications
class WEB_API Notification final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Notification, notification)

public:
    // The id this notification is known by across the Page bridge, for tests that act on it from the UI side.
    u64 id_for_testing() const;

    // The document the notification was created in, if a window's.
    DOM::Document const* document() const;
    // The document is gone, so no event can reach this object again; the UI process keeps showing the notification.
    void abandon();

    // What the UI process reports back about the notification it shows for this object.
    void did_show();
    void did_fail_to_show();
    void did_click();
    void did_close(bool by_user);
};

}
