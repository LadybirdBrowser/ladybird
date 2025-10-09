/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>

namespace Web::NotificationsAPI {

struct NotificationAction {
    String action;
    String title;
    Optional<String> navigate;
    Optional<String> icon;
};

struct NotificationOptions {
    Bindings::NotificationDirection dir = Bindings::NotificationDirection::Auto;
    String lang = ""_string;
    String body = ""_string;
    Optional<String> navigate;
    String tag = ""_string;
    Optional<String> image;
    Optional<String> icon;
    Optional<String> badge;
    // VibratePattern vibrate;  // FIXME: properly implement vibrate pattern
    Optional<HighResolutionTime::EpochTimeStamp> timestamp;
    bool renotify = false;
    Optional<bool> silent;
    bool require_interaction = false;
    JS::Value data;
    Vector<NotificationAction> actions;
};

// https://notifications.spec.whatwg.org/#notifications
class WEB_API Notification final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Notification, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Notification);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<Notification>> construct_impl(
        JS::Realm& realm,
        String const& title,
        Optional<NotificationOptions> options);

private:
    Notification(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
