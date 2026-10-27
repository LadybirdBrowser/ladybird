/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Notification.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>
#include <LibWebCommon/NotificationsAPI/NotificationData.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::NotificationsAPI {

using NotificationDirection = Bindings::NotificationDirection;

struct NotificationAction {
    Utf16String action;
    Utf16String title;
    Optional<Utf16String> navigate;
    Optional<Utf16String> icon;
};

struct NotificationOptions {
    NotificationDirection direction { NotificationDirection::Auto };
    Utf16String language;
    Utf16String body;
    Optional<Utf16String> navigate;
    Utf16String tag;
    Optional<Utf16String> image;
    Optional<Utf16String> icon;
    Optional<Utf16String> badge;
    // FIXME: Validate and normalize the pattern (create step 15); only its presence matters until then.
    Optional<Variant<WebIDL::UnsignedLong, Vector<WebIDL::UnsignedLong>>> vibrate;
    Optional<HighResolutionTime::EpochTimeStamp> timestamp;
    bool renotify { false };
    Optional<bool> silent;
    bool require_interaction { false };
    HTML::StorageSerializationRecord data;
    Vector<NotificationAction> actions;
};

// https://notifications.spec.whatwg.org/#concept-notification
// This is the notification described as "notification" in the spec. Do not confuse it with "notification" as in the IDL which is just the JS wrapper.
// "A notification is an abstract representation of something that happened, such as the delivery of a message."
struct ConceptNotification {
    // FIXME: A notification has an associated service worker registration (null or a service worker registration). It is initially null.
    String title;
    NotificationDirection direction;
    Utf16String language;
    Utf16String body;
    Optional<URL::URL> navigation_url;
    Utf16String tag;
    HTML::StorageSerializationRecord data;
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
        Utf16String name;
        Utf16String title;
        Optional<URL::URL> navigation_url;
        Optional<URL::URL> icon_url;
        // FIXME: icon resource
    };
    Vector<Action> actions;
};

// https://notifications.spec.whatwg.org/#notifications
class WEB_API Notification final : public DOM::EventTarget {
    WEB_WRAPPABLE(Notification, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Notification);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<Notification>> create_with_global_scope(
        JS::Object& relevant_global_object,
        HTML::WindowOrWorkerGlobalScopeMixin&,
        String const& title,
        NotificationOptions options);
    [[nodiscard]] static WebIDL::ExceptionOr<NotificationOptions> options_from_bindings(JS::VM&, Bindings::NotificationOptions const&);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<Notification>> create_for_constructor(JS::Object&, Utf16String const& title, Bindings::NotificationOptions const&);

    // https://notifications.spec.whatwg.org/#create-a-notification-with-a-settings-object
    static WebIDL::ExceptionOr<ConceptNotification> create_a_notification_with_a_settings_object(
        String const& title,
        NotificationOptions options,
        GC::Ref<HTML::EnvironmentSettingsObject> settings);

    // https://notifications.spec.whatwg.org/#create-a-notification
    static WebIDL::ExceptionOr<ConceptNotification> create_a_notification(
        String const& title,
        NotificationOptions options,
        URL::Origin origin,
        URL::URL base_url,
        HighResolutionTime::EpochTimeStamp fallback_timestamp);

    static Bindings::NotificationPermission permission();

    void close();

    void set_onclick(WebIDL::CallbackType*);
    WebIDL::CallbackType* onclick();
    void set_onshow(WebIDL::CallbackType*);
    WebIDL::CallbackType* onshow();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();
    void set_onclose(WebIDL::CallbackType*);
    WebIDL::CallbackType* onclose();

    // What the UI process reports back about the notification it shows for this object.
    void did_show();
    void did_fail_to_show();
    void did_click();
    void did_close(bool by_user);

    // https://notifications.spec.whatwg.org/#maximum-number-of-actions-supported
    // Two is what both desktop notification platforms show room for; persistent notifications, the only kind that
    // carries actions, aren't supported yet.
    static unsigned long max_actions() { return 2; }

    String const& title() const { return m_notification.title; }
    NotificationDirection direction() const { return m_notification.direction; }
    NotificationDirection notification_direction() const { return direction(); }
    Utf16String const& lang() const { return m_notification.language; }
    Utf16String const& body() const { return m_notification.body; }
    String navigate() const { return m_notification.navigation_url.has_value() ? m_notification.navigation_url->serialize() : ""_string; }
    Utf16String const& tag() const { return m_notification.tag; }
    String image() const { return m_notification.image_url.has_value() ? m_notification.image_url->serialize() : ""_string; }
    String icon() const { return m_notification.icon_url.has_value() ? m_notification.icon_url->serialize() : ""_string; }
    String badge() const { return m_notification.badge_url.has_value() ? m_notification.badge_url->serialize() : ""_string; }
    HighResolutionTime::EpochTimeStamp timestamp() const { return m_notification.timestamp; }
    bool renotify() const { return m_notification.renotify_preference; }
    Optional<bool> silent() const { return m_notification.silent_preference; }
    bool require_interaction() const { return m_notification.require_interaction_preference; }
    Vector<NotificationAction> actions() const;
    HTML::StorageSerializationRecord const& serialized_data() const { return m_notification.data; }
    JS::Value data(JS::Object const& relevant_global_object) const;

private:
    Notification();

    virtual void visit_edges(Cell::Visitor&) override;

    static Utf16String serialize_url_for_bindings(Optional<URL::URL> const&);

    void run_steps_in_parallel_after_construction();
    void show_steps();
    void close_steps();
    void queue_task_to_fire_event(Utf16FlyString const& event_name);
    NotificationData data() const;

    ConceptNotification m_notification;

    // The global the constructor ran in: Where the events fire, and whose window the notification belongs to.
    GC::Ptr<JS::Object> m_relevant_global;
    GC::Ptr<Page> m_page;

    // Where the notification is on its way through the UI process. Showing and Closing mean it's in the page's list of
    // notifications; Closed means it has left it for good.
    enum class State {
        Idle,
        Showing,
        Closing,
        Closed,
    };
    State m_state { State::Idle };
    u64 m_id { 0 };
};

}
