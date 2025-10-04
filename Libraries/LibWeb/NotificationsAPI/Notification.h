/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/StructuredSerialize.h>
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

// https://notifications.spec.whatwg.org/#concept-notification
// This is the notification described as "notification" in the spec. Do not confuse it with "notification" as in the IDL which is just the JS wrapper.
// "A notification is an abstract representation of something that happened, such as the delivery of a message."
struct ConceptNotification {
    // FIXME: A notification has an associated service worker registration (null or a service worker registration). It is initially null.
    String title;
    Bindings::NotificationDirection direction;
    String language;
    String body;
    Optional<URL::URL> navigation_url;
    String tag;
    HTML::SerializationRecord data;
    HighResolutionTime::EpochTimeStamp timestamp;
    URL::Origin origin = URL::Origin({}); // FIXME: Is this a hack ? There is no default constructor to URL::Origin and the value for `origin` is set in `create-a-notification-with-a-settings-object`
    bool renotify_preference;
    Optional<bool> silent_preference;
    bool require_interaction_preference;
    Optional<URL::URL> image_url;
    Optional<URL::URL> icon_url;
    Optional<URL::URL> badge_url;

    // FIXME: add the resources from m_image_url, m_icon_url and m_badge_url

    // FIXME: A notification has an associated vibration pattern (a list). It is initially « ».

    // https://notifications.spec.whatwg.org/#action
    struct Action {
        String name;
        String title;
        Optional<URL::URL> navigation_url;
        Optional<URL::URL> icon_url;
        // FIXME: icon resource
    };
    Vector<Action> actions;
};

// https://notifications.spec.whatwg.org/#notifications
class WEB_API Notification final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Notification, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Notification);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<Notification>> construct_impl(
        JS::Realm& realm,
        String const& title,
        NotificationOptions const& options);

    // https://notifications.spec.whatwg.org/#create-a-notification-with-a-settings-object
    static WebIDL::ExceptionOr<ConceptNotification> create_a_notification_with_a_settings_object(
        JS::Realm& realm,
        String const& title,
        NotificationOptions const& options,
        GC::Ref<HTML::EnvironmentSettingsObject> settings);

    // https://notifications.spec.whatwg.org/#create-a-notification
    static WebIDL::ExceptionOr<ConceptNotification> create_a_notification(
        JS::Realm& realm,
        String const& title,
        NotificationOptions const& options,
        URL::Origin origin,
        URL::URL base_url,
        HighResolutionTime::EpochTimeStamp fallback_timestamp);

    static unsigned long max_actions(JS::VM&)
    {
        // FIXME: Change the number of max_actions supported when actions will actually be supported
        // It seems like Chrome is 2, Firefox is undefined, Safari is undefined
        return 0;
    }

    String const& title() const { return m_notification.title; }
    Bindings::NotificationDirection dir() const { return m_notification.direction; }
    String const& lang() const { return m_notification.language; }
    String const& body() const { return m_notification.body; }
    String navigate() const { return m_notification.navigation_url.has_value() ? m_notification.navigation_url->serialize() : ""_string; }
    String const& tag() const { return m_notification.tag; }
    String image() const { return m_notification.image_url.has_value() ? m_notification.image_url->serialize() : ""_string; }
    String icon() const { return m_notification.icon_url.has_value() ? m_notification.icon_url->serialize() : ""_string; }
    String badge() const { return m_notification.badge_url.has_value() ? m_notification.badge_url->serialize() : ""_string; }
    HighResolutionTime::EpochTimeStamp timestamp() const { return m_notification.timestamp; }
    bool renotify() const { return m_notification.renotify_preference; }
    Optional<bool> silent() const { return m_notification.silent_preference; }
    bool require_interaction() const { return m_notification.require_interaction_preference; }
    Vector<NotificationAction> actions() const;
    JS::Value data() const;

private:
    Notification(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    ConceptNotification m_notification;
};

}
