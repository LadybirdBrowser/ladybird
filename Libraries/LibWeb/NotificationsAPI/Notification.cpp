/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NotificationPrototype.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/NotificationsAPI/Notification.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>

namespace Web::NotificationsAPI {

GC_DEFINE_ALLOCATOR(Notification);

Notification::Notification(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
    this->body();
}

// https://notifications.spec.whatwg.org/#create-a-notification
WebIDL::ExceptionOr<ConceptNotification> Notification::create_a_notification(
    JS::Realm& realm,
    String title,
    Optional<NotificationOptions> options,
    URL::Origin origin,
    URL::URL base_url,
    HighResolutionTime::EpochTimeStamp fallback_timestamp)
{
    // 1. Let notification be a new notification.
    ConceptNotification notification;

    // FIXME: 2. If options["silent"] is true and options["vibrate"] exists, then throw a TypeError.

    // 3. If options["renotify"] is true and options["tag"] is the empty string, then throw a TypeError.
    if (options->renotify && options->tag.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "options[\"tag\"] cannot be the empty string when options[\"renotify\"] is set to true."sv };

    // 4. Set notification’s data to StructuredSerializeForStorage(options["data"]).
    notification.data = HTML::structured_serialize_for_storage(realm.vm(), options->data).release_value();

    // 5. Set notification’s title to title.
    notification.title = move(title);

    // 6. Set notification’s direction to options["dir"].
    notification.direction = options->dir;

    // 7. Set notification’s language to options["lang"].
    notification.language = options->lang;

    // 8. Set notification’s origin to origin.
    notification.origin = move(origin);

    // 9. Set notification’s body to options["body"].
    notification.body = options->body;

    auto try_set_url = [&base_url](String const& url) -> Optional<URL::URL> {
        auto maybe_url = base_url.complete_url(url);
        if (maybe_url.has_value())
            return maybe_url.release_value();
        return {};
    };

    // 10. If options["navigate"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s navigation URL to the return value. (Otherwise notification’s navigation URL remains null.)
    if (options->navigate.has_value()) {
        notification.navigation_url = try_set_url(options->navigate.value());
    }

    // 11. Set notification’s tag to options["tag"].
    notification.tag = options->tag;

    // 12. If options["image"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s image URL to the return value. (Otherwise notification’s image URL is not set.)
    if (options->image.has_value()) {
        notification.image_url = try_set_url(options->image.value());
    }

    // 13. If options["icon"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s icon URL to the return value. (Otherwise notification’s icon URL is not set.)
    if (options->icon.has_value()) {
        notification.icon_url = try_set_url(options->icon.value());
    }

    // 14. If options["badge"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s badge URL to the return value. (Otherwise notification’s badge URL is not set.)
    if (options->badge.has_value()) {
        notification.badge_url = try_set_url(options->badge.value());
    }

    // FIXME: 15. If options["vibrate"] exists, then validate and normalize it and
    // set notification’s vibration pattern to the return value.

    // 16. If options["timestamp"] exists, then set notification’s timestamp to the value.
    // Otherwise, set notification’s timestamp to fallbackTimestamp.
    if (options->timestamp.has_value())
        notification.timestamp = options->timestamp.value();
    else
        notification.timestamp = fallback_timestamp;

    // 17. Set notification’s renotify preference to options["renotify"].
    notification.renotify_preference = options->renotify;

    // 18. Set notification’s silent preference to options["silent"].
    notification.silent_preference = options->silent;

    // 19. Set notification’s require interaction preference to options["requireInteraction"].
    notification.require_interaction_preference = options->require_interaction;

    // 20. Set notification’s actions to « ».
    notification.actions = {};

    // 21. For each entry in options["actions"], up to the maximum number of actions supported (skip any excess entries):
    for (auto const& entry : options->actions) {
        // FIXME: stop the loop at the max number of actions supported

        // 1. Let action be a new notification action.
        ConceptNotification::Action action;

        // 2. Set action’s name to entry["action"].
        action.name = entry.action;

        // 3. Set action’s title to entry["title"].
        action.title = entry.title;

        // 4. If entry["navigate"] exists, then parse it using baseURL, and if that does not return failure,
        // set action’s navigation URL to the return value. (Otherwise action’s navigation URL remains null.)
        if (entry.navigate.has_value())
            action.navigation_url = try_set_url(entry.navigate.value());

        // 5. If entry["icon"] exists, then parse it using baseURL, and if that does not return failure,
        // set action’s icon URL to the return value. (Otherwise action’s icon URL remains null.)
        if (entry.icon.has_value())
            action.icon_url = try_set_url(entry.icon.value());

        // 6. Append action to notification’s actions.
        notification.actions.append(action);
    }

    // 22. Return notification.
    return notification;
}

// https://notifications.spec.whatwg.org/#create-a-notification-with-a-settings-object
WebIDL::ExceptionOr<ConceptNotification> Notification::create_a_notification_with_a_settings_object(
    JS::Realm& realm,
    String title,
    Optional<NotificationOptions> options,
    GC::Ptr<HTML::EnvironmentSettingsObject> settings)
{
    // 1. Let origin be settings’s origin.
    URL::Origin origin = settings->origin();

    // 2. Let baseURL be settings’s API base URL.
    URL::URL base_url = settings->api_base_url();

    // 3. Let fallbackTimestamp be the number of milliseconds from the Unix epoch to settings’s current wall time,
    // rounded to the nearest integer.
    auto fallback_timestamp = round_to<HighResolutionTime::EpochTimeStamp>(settings->current_wall_time());

    // 4. Return the result of creating a notification given title, options, origin, baseURL, and fallbackTimestamp.
    return create_a_notification(realm, move(title), options, origin, base_url, fallback_timestamp);
}

// https://notifications.spec.whatwg.org/#constructors
WebIDL::ExceptionOr<GC::Ref<Notification>> Notification::construct_impl(
    JS::Realm& realm,
    String title,
    Optional<NotificationOptions> options)
{
    auto this_notification = realm.create<Notification>(realm);
    auto& relevant_settings_object = HTML::relevant_settings_object(this_notification);
    auto& relevant_global_object = HTML::relevant_global_object(this_notification);

    // 1. If this’s relevant global object is a ServiceWorkerGlobalScope object, then throw a TypeError.
    if (is<ServiceWorker::ServiceWorkerGlobalScope>(relevant_global_object))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "This’s relevant global object is a ServiceWorkerGlobalScope object"sv };

    // 2. If options["actions"] is not empty, then throw a TypeError.
    if (!options->actions.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Options `action` is not empty"sv };

    // 3. Let notification be the result of creating a notification with a settings object given title, options, and this’s relevant settings object.
    ConceptNotification notification = TRY(create_a_notification_with_a_settings_object(realm, move(title), options, relevant_settings_object));

    // 4. Associate this with notification.
    this_notification->m_notification = notification;

    // FIXME: 5. Run these steps in parallel:

    // FIXME: 1. If the result of getting the notifications permission state is not "granted",
    // then queue a task to fire an event named error on this, and abort these steps.

    // FIXME: 2. Run the notification show steps for notification.

    return this_notification;
}

void Notification::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Notification);
    Base::initialize(realm);
}

}
