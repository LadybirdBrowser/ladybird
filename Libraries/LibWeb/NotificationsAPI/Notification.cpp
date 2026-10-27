/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NotificationsAPI/Notification.h>
#include <LibWeb/NotificationsAPI/NotificationPermission.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>

namespace Web::NotificationsAPI {

GC_DEFINE_ALLOCATOR(Notification);

Notification::Notification()
    : DOM::EventTarget()
{
}

void Notification::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_relevant_global);
    visitor.visit(m_page);
    visitor.visit(m_icon_fetch_controller);
    visitor.visit(m_fetch_timeout);
}

WebIDL::ExceptionOr<NotificationOptions> Notification::options_from_bindings(JS::VM& vm, Bindings::NotificationOptions const& options)
{
    Vector<NotificationAction> actions;
    actions.ensure_capacity(options.actions.size());
    for (auto const& entry : options.actions) {
        actions.append(NotificationAction {
            .action = entry.action,
            .title = entry.title,
            .navigate = entry.navigate,
            .icon = entry.icon,
        });
    }

    return NotificationOptions {
        .direction = options.dir,
        .language = options.lang,
        .body = options.body,
        .navigate = options.navigate,
        .tag = options.tag,
        .image = options.image,
        .icon = options.icon,
        .badge = options.badge,
        .vibrate = options.vibrate,
        .timestamp = options.timestamp,
        .renotify = options.renotify,
        .silent = options.silent,
        .require_interaction = options.require_interaction,
        .data = TRY(HTML::structured_serialize_for_storage(vm, options.data)),
        .actions = move(actions),
    };
}

WebIDL::ExceptionOr<GC::Ref<Notification>> Notification::create_for_constructor(JS::Object& relevant_global_object, Utf16String const& title, Bindings::NotificationOptions const& options)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(relevant_global_object);
    VERIFY(global_scope);

    // The constructor rejects actions before attempting to serialize data.
    if (!options.actions.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Options `action` is not empty"_utf16 };

    auto title_utf8 = title.to_utf8();
    return create_with_global_scope(relevant_global_object, *global_scope, title_utf8, TRY(options_from_bindings(relevant_global_object.shape().realm().vm(), options)));
}

// https://notifications.spec.whatwg.org/#create-a-notification
WebIDL::ExceptionOr<ConceptNotification> Notification::create_a_notification(
    String const& title,
    NotificationOptions options,
    URL::Origin origin,
    URL::URL base_url,
    HighResolutionTime::EpochTimeStamp fallback_timestamp)
{
    // 1. Let notification be a new notification.
    ConceptNotification notification;

    // 2. If options["silent"] is true and options["vibrate"] exists, then throw a TypeError.
    if (options.silent == true && options.vibrate.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "options[\"vibrate\"] cannot be set when options[\"silent\"] is true."_utf16 };

    // 3. If options["renotify"] is true and options["tag"] is the empty string, then throw a TypeError.
    if (options.renotify && options.tag.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "options[\"tag\"] cannot be the empty string when options[\"renotify\"] is set to true."_utf16 };

    // 4. Set notification’s data to StructuredSerializeForStorage(options["data"]).
    notification.data = move(options.data);

    // 5. Set notification’s title to title.
    notification.title = title;

    // 6. Set notification’s direction to options["dir"].
    notification.direction = options.direction;

    // 7. Set notification’s language to options["lang"].
    notification.language = move(options.language);

    // 8. Set notification’s origin to origin.
    notification.origin = move(origin);

    // 9. Set notification’s body to options["body"].
    notification.body = move(options.body);

    // 10. If options["navigate"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s navigation URL to the return value. (Otherwise notification’s navigation URL remains null.)
    if (options.navigate.has_value()) {
        auto navigate = options.navigate->to_utf8();
        notification.navigation_url = base_url.complete_url(navigate);
    }

    // 11. Set notification’s tag to options["tag"].
    notification.tag = move(options.tag);

    // 12. If options["image"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s image URL to the return value. (Otherwise notification’s image URL is not set.)
    if (options.image.has_value()) {
        auto image = options.image->to_utf8();
        notification.image_url = base_url.complete_url(image);
    }

    // 13. If options["icon"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s icon URL to the return value. (Otherwise notification’s icon URL is not set.)
    if (options.icon.has_value()) {
        auto icon = options.icon->to_utf8();
        notification.icon_url = base_url.complete_url(icon);
    }

    // 14. If options["badge"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s badge URL to the return value. (Otherwise notification’s badge URL is not set.)
    if (options.badge.has_value()) {
        auto badge = options.badge->to_utf8();
        notification.badge_url = base_url.complete_url(badge);
    }

    // FIXME: 15. If options["vibrate"] exists, then validate and normalize it and
    // set notification’s vibration pattern to the return value.

    // 16. If options["timestamp"] exists, then set notification’s timestamp to the value.
    // Otherwise, set notification’s timestamp to fallbackTimestamp.
    if (options.timestamp.has_value())
        notification.timestamp = options.timestamp.value();
    else
        notification.timestamp = fallback_timestamp;

    // 17. Set notification’s renotify preference to options["renotify"].
    notification.renotify_preference = options.renotify;

    // 18. Set notification’s silent preference to options["silent"].
    notification.silent_preference = options.silent;

    // 19. Set notification’s require interaction preference to options["requireInteraction"].
    notification.require_interaction_preference = options.require_interaction;

    // 20. Set notification’s actions to « ».
    notification.actions = {};

    // 21. For each entry in options["actions"], up to the maximum number of actions supported (skip any excess entries):
    for (auto const& entry : options.actions) {
        if (notification.actions.size() >= max_actions())
            break;

        // 1. Let action be a new notification action.
        ConceptNotification::Action action;

        // 2. Set action’s name to entry["action"].
        action.name = entry.action;

        // 3. Set action’s title to entry["title"].
        action.title = entry.title;

        // 4. If entry["navigate"] exists, then parse it using baseURL, and if that does not return failure,
        // set action’s navigation URL to the return value. (Otherwise action’s navigation URL remains null.)
        if (entry.navigate.has_value()) {
            auto navigate = entry.navigate->to_utf8();
            action.navigation_url = base_url.complete_url(navigate);
        }

        // 5. If entry["icon"] exists, then parse it using baseURL, and if that does not return failure,
        // set action’s icon URL to the return value. (Otherwise action’s icon URL remains null.)
        if (entry.icon.has_value()) {
            auto icon = entry.icon->to_utf8();
            action.icon_url = base_url.complete_url(icon);
        }

        // 6. Append action to notification’s actions.
        notification.actions.append(action);
    }

    // 22. Return notification.
    return notification;
}

// https://notifications.spec.whatwg.org/#create-a-notification-with-a-settings-object
WebIDL::ExceptionOr<ConceptNotification> Notification::create_a_notification_with_a_settings_object(
    String const& title,
    NotificationOptions options,
    GC::Ref<HTML::EnvironmentSettingsObject> settings)
{
    // 1. Let origin be settings’s origin.
    URL::Origin origin = settings->origin();

    // 2. Let baseURL be settings’s API base URL.
    URL::URL base_url = settings->api_base_url();

    // 3. Let fallbackTimestamp be the number of milliseconds from the Unix epoch to settings’s current wall time,
    // rounded to the nearest integer.
    auto fallback_timestamp = round_to<HighResolutionTime::EpochTimeStamp>(settings->current_wall_time());

    // 4. Return the result of creating a notification given title, options, origin, baseURL, and fallbackTimestamp.
    return create_a_notification(title, move(options), origin, base_url, fallback_timestamp);
}

// https://notifications.spec.whatwg.org/#constructors
WebIDL::ExceptionOr<GC::Ref<Notification>> Notification::create_with_global_scope(
    JS::Object& relevant_global_object,
    HTML::WindowOrWorkerGlobalScopeMixin& global_scope,
    String const& title,
    NotificationOptions options)
{
    auto& relevant_settings_object = HTML::relevant_settings_object(global_scope);

    // 1. If this’s relevant global object is a ServiceWorkerGlobalScope object, then throw a TypeError.
    if (is<ServiceWorker::ServiceWorkerGlobalScope>(global_scope.this_impl()))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "This’s relevant global object is a ServiceWorkerGlobalScope object"_utf16 };

    // 2. If options["actions"] is not empty, then throw a TypeError.
    if (!options.actions.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Options `action` is not empty"_utf16 };

    // 3. Let notification be the result of creating a notification with a settings object given title, options, and this’s relevant settings object.
    ConceptNotification notification = TRY(create_a_notification_with_a_settings_object(title, move(options), relevant_settings_object));

    // 4. Associate this with notification.
    auto this_notification = GC::Heap::the().allocate<Notification>();
    this_notification->m_notification = notification;
    this_notification->m_relevant_global = relevant_global_object;
    if (auto* window = HTML::window_from_global_object(relevant_global_object))
        this_notification->m_page = window->page();

    // 5. Run these steps in parallel:
    // AD-HOC: The steps run from a task on the relevant global instead of in parallel: Showing crosses to the UI
    //         process asynchronously anyway, and a task keeps the permission read and the event firing on the main
    //         thread, after the constructor's caller has had its turn to attach event handlers.
    HTML::queue_global_task(HTML::Task::Source::Unspecified, relevant_global_object, GC::create_function(GC::Heap::the(), [this_notification] {
        this_notification->run_steps_in_parallel_after_construction();
    }));

    return this_notification;
}

// Step 5 of https://notifications.spec.whatwg.org/#constructors
void Notification::run_steps_in_parallel_after_construction()
{
    // 1. If the result of getting the notifications permission state is not "granted", then queue a task to fire an
    //    event named error on this, and abort these steps.
    if (get_the_notifications_permission_state(HTML::relevant_settings_object(*m_relevant_global)) != PermissionsAPI::PermissionState::Granted) {
        queue_task_to_fire_event(HTML::EventNames::error);
        return;
    }

    // AD-HOC: The spec lets any document or worker show a notification. We refuse two cases until they're supported:
    //         A worker has no page to route the notification's events through, and a document whose origin differs
    //         from its top-level origin would have its notification attributed to the top-level site. Both get an
    //         error event, as if permission had been denied.
    auto* window = HTML::window_from_global_object(*m_relevant_global);
    if (!window || !m_page) {
        queue_task_to_fire_event(HTML::EventNames::error);
        return;
    }
    auto& settings = HTML::relevant_settings_object(*m_relevant_global);
    if (!settings.top_level_origin.has_value() || !settings.top_level_origin->is_same_origin(settings.origin())) {
        queue_task_to_fire_event(HTML::EventNames::error);
        return;
    }

    // 2. Run the notification show steps for notification.
    show_steps();
}

// The longest a notification waits for its icon before it shows without one.
static constexpr auto icon_fetch_timeout = AK::Duration::from_seconds(5);
// The longest side an icon is sent to the platform at; both desktop platforms show icons smaller than this.
static constexpr int icon_maximum_side = 128;

// https://notifications.spec.whatwg.org/#notification-show-steps
void Notification::show_steps()
{
    // 1. Run the fetch steps given notification and settings.
    // 2. Wait for any fetches to complete and notification's image resource, icon resource, and badge resource to
    //    be set (if any), as well as the icon resources for the notification's actions (if any).
    // AD-HOC: The wait for the fetch is bounded: An icon that hasn't arrived after a few seconds isn't worth holding
    //         the notification back for, so it then shows without one.
    m_fetch_timeout = Platform::Timer::create_single_shot(heap(), icon_fetch_timeout, GC::create_function(heap(), [this] {
        if (m_icon_fetch_controller)
            m_icon_fetch_controller->stop_fetch();
        m_icon_fetch_controller = nullptr;
        display();
    }));
    m_fetch_timeout->start();
    run_the_fetch_steps(GC::create_function(heap(), [this] {
        if (!m_fetch_timeout)
            return;
        m_fetch_timeout->stop();
        display();
    }));
}

// https://notifications.spec.whatwg.org/#fetch-steps
void Notification::run_the_fetch_steps(GC::Ref<GC::Function<void()>> on_done)
{
    // 1. If the notification platform supports images and notification's image URL is set, then fetch a notification
    //    resource given notification, notification's image URL, settings, and the following step given a resource
    //    resource: set notification's image resource to resource.
    // NB: No platform we show on takes an image.

    // 2. If the notification platform supports icons and notification's icon URL is set, then fetch a notification
    //    resource given notification, notification's icon URL, settings, and the following step given a resource
    //    resource: set notification's icon resource to resource.
    if (!m_notification.icon_url.has_value()) {
        on_done->function()();
        return;
    }
    fetch_a_notification_resource(*m_notification.icon_url, GC::create_function(heap(), [this, on_done](RefPtr<Gfx::Bitmap const> resource) {
        m_icon_resource = move(resource);
        on_done->function()();
    }));

    // 3. If the notification platform supports badges and notification's badge URL is set, then fetch a notification
    //    resource given notification, notification's badge URL, settings, and the following step given a resource
    //    resource: set notification's badge resource to resource.
    // NB: No platform we show on takes a badge.

    // 4. If the notification platform supports actions and action icons, then for each entry of notification's
    //    actions: if entry's icon URL is non-null, then fetch a notification resource given notification, entry's
    //    icon URL, settings, and the following step given a resource resource: set entry's icon resource to resource.
    // NB: A non-persistent notification has no actions.
}

// https://notifications.spec.whatwg.org/#fetch-a-notification-resource
void Notification::fetch_a_notification_resource(URL::URL const& url, GC::Ref<GC::Function<void(RefPtr<Gfx::Bitmap const>)>> process_resource)
{
    auto& settings = HTML::relevant_settings_object(*m_relevant_global);

    // 1. Let request be a new request whose URL is url, client is settings, destination is "image", mode is
    //    "no-cors", and credentials mode is "include".
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(url);
    request->set_client(settings.address());
    request->set_destination(Fetch::Infrastructure::Request::Destination::Image);
    request->set_mode(Fetch::Infrastructure::Request::Mode::NoCORS);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);

    // 2. If settings is null, then set request's origin to notification's origin, request's referrer to
    //    "no-referrer", and request's service-workers mode to "none".
    // NB: A page's notification always has its settings object.

    // 3. Fetch request with useParallelQueue set to true and processResponseConsumeBody set to the following steps
    //    given a response response and null, failure, or a byte sequence bytes:
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };
    fetch_algorithms_input.process_response_consume_body = [this, process_resource](GC::Ref<Fetch::Infrastructure::ResponseView>, Fetch::Infrastructure::FetchAlgorithms::BodyBytes bytes) {
        m_icon_fetch_controller = nullptr;
        // The bytes are here, so the wait is no longer on the network: Decoding them finishes on its own, however
        // long a busy machine takes over it.
        if (m_fetch_timeout)
            m_fetch_timeout->stop();

        // 1. If bytes is null or failure, then return.
        // AD-HOC: The show steps wait on this fetch, so an absent resource is reported as such, rather than left
        //         for the timeout to notice.
        if (!bytes.has<Core::ImmutableBytes>()) {
            process_resource->function()(nullptr);
            return;
        }

        // 2. Attempt to decode bytes as an image.
        // 3. If the image format is supported, then run processResource given the decoded image.
        auto on_decoded = [this, process_resource](Platform::DecodedImage& decoded_image) -> ErrorOr<void> {
            RefPtr<Gfx::Bitmap const> bitmap;
            if (!decoded_image.frames.is_empty() && decoded_image.frames[0].bitmap) {
                bitmap = decoded_image.frames[0].bitmap;
                auto size = bitmap->size();
                if (size.width() > icon_maximum_side || size.height() > icon_maximum_side) {
                    auto scale = static_cast<float>(icon_maximum_side) / static_cast<float>(max(size.width(), size.height()));
                    Gfx::IntSize scaled_size { max(1, static_cast<int>(roundf(size.width() * scale))), max(1, static_cast<int>(roundf(size.height() * scale))) };
                    // Without a compositor to scale with, the platform gets the icon as it came.
                    if (m_page && m_page->has_compositor_host()) {
                        auto output = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, scaled_size);
                        Compositing::DisplayListResourceStorage resource_storage;
                        auto display_list = Painting::record_image_frame_display_list(Gfx::DecodedImageFrame { *bitmap }, Gfx::FloatRect { {}, scaled_size.to_type<float>() }, Gfx::ScalingMode::BilinearMipmap, resource_storage);
                        if (!output.is_error() && m_page->compositor_host().rasterize_display_list(display_list, resource_storage, output.value()))
                            bitmap = output.release_value();
                    }
                }
            }
            process_resource->function()(move(bitmap));
            return {};
        };
        auto on_decode_failed = [process_resource](Error&) {
            process_resource->function()(nullptr);
        };
        (void)Platform::ImageCodecPlugin::the().decode_image(bytes.get<Core::ImmutableBytes>().bytes(), move(on_decoded), move(on_decode_failed));
    };
    m_icon_fetch_controller = Fetch::Fetching::fetch(settings.realm(), request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)), Fetch::Fetching::UseParallelQueue::Yes);
}

// Steps 3 to 8 of https://notifications.spec.whatwg.org/#notification-show-steps
void Notification::display()
{
    m_fetch_timeout = nullptr;

    // 3. Let shown be false.
    // 4. Let oldNotification be the notification in the list of notifications whose tag is not the empty string and is
    //    notification's tag, and whose origin is same origin with notification's origin, if any, and null otherwise.
    // 5. If oldNotification is non-null:
    //    1. Handle close events with oldNotification.
    //    2. If the notification platform supports replacement:
    //       1. Replace oldNotification with notification, in the list of notifications.
    //       2. Set shown to true.
    //    3. Otherwise, remove oldNotification from the list of notifications.
    // 6. If shown is false:
    //    1. Append notification to the list of notifications.
    //    2. Display notification on the device (e.g., by calling the appropriate notification platform API).
    // 7. If shown is false or oldNotification is non-null, and notification's renotify preference is true, then
    //    optionally alert the end user in a user-agent-defined manner (e.g., by playing a sound).
    // NB: Steps 4, 5 and 7 are the UI process's: It sees every page's notifications, so it finds oldNotification
    //     across pages, replaces it on the platform, and reports its close back through notification_closed().
    //     Here, the notification joins this page's part of the list and is sent off for display.
    m_id = m_page->allocate_notification_id();
    m_page->register_showing_notification(m_id, *this);
    m_state = State::Showing;
    m_page->client().page_did_request_show_notification(m_id, data(), m_icon_resource);

    // 8. If notification is a non-persistent notification, then queue a task to fire an event named show on the
    //    Notification object representing notification.
    // NB: Fired from did_show(), once the platform has shown the notification: Until then the display can still fail,
    //     and a failure fires error instead.
}

DOM::Document const* Notification::document() const
{
    if (auto const* window = HTML::window_from_global_object(*m_relevant_global))
        return &window->associated_document();
    return nullptr;
}

void Notification::abandon()
{
    if (m_state != State::Showing && m_state != State::Closing)
        return;
    m_state = State::Closed;
    m_page->unregister_notification(m_id);
    m_page->client().page_did_abandon_notification(m_id);
}

void Notification::did_show()
{
    if (m_state != State::Showing)
        return;
    queue_task_to_fire_event(HTML::EventNames::show);
}

void Notification::did_fail_to_show()
{
    if (m_state != State::Showing)
        return;
    m_state = State::Closed;
    m_page->unregister_notification(m_id);
    queue_task_to_fire_event(HTML::EventNames::error);
}

// Step 2 of the non-persistent branch of https://notifications.spec.whatwg.org/#activating-a-notification
void Notification::did_click()
{
    if (m_state != State::Showing && m_state != State::Closing)
        return;

    // Otherwise, queue a task to run these steps:
    HTML::queue_global_task(HTML::Task::Source::UserInteraction, *m_relevant_global, GC::create_function(GC::Heap::the(), [this] {
        // 1. Let intoFocus be the result of firing an event named click on the Notification object representing
        //    notification, with its cancelable attribute initialized to true.
        // NB: The user activated the notification, so the window gets transient activation first: That's what lets
        //     focus() work from within the event listener, as the spec encourages.
        if (auto* window = HTML::window_from_global_object(*m_relevant_global))
            window->set_last_activation_timestamp(HighResolutionTime::current_high_resolution_time(*m_relevant_global));
        auto into_focus = dispatch_event(DOM::Event::create(*m_relevant_global, HTML::EventNames::click, { .cancelable = true }));

        // 2. If intoFocus is true, then the user agent should bring the notification's related browsing context's
        //    viewport into focus.
        if (m_page)
            m_page->client().page_did_handle_notification_click(m_id, into_focus);
    }));
}

// https://notifications.spec.whatwg.org/#dom-notification-close
void Notification::close()
{
    // The close() method steps are to run the close steps for this's notification.
    close_steps();
}

// https://notifications.spec.whatwg.org/#close-steps
void Notification::close_steps()
{
    // 1. If the list of notifications does not contain notification, then abort these steps.
    // NB: A notification that's already on its way out (Closing) is left to the close the UI process is carrying out.
    if (m_state != State::Showing)
        return;

    // 2. Handle close events with notification.
    // 3. Remove notification from the list of notifications.
    // NB: Both happen in did_close(), once the UI process has withdrawn the notification from the platform.
    m_state = State::Closing;
    m_page->client().page_did_request_close_notification(m_id);
}

// Steps 2 and 3 of https://notifications.spec.whatwg.org/#close-steps
void Notification::did_close(bool)
{
    if (m_state != State::Showing && m_state != State::Closing)
        return;

    // 2. Handle close events with notification.
    // https://notifications.spec.whatwg.org/#handle-close-events
    // 2. If notification is a non-persistent notification, then queue a task to fire an event named close on the
    //    Notification object representing notification.
    queue_task_to_fire_event(HTML::EventNames::close);

    // 3. Remove notification from the list of notifications.
    m_state = State::Closed;
    m_page->unregister_notification(m_id);
}

void Notification::queue_task_to_fire_event(Utf16FlyString const& event_name)
{
    HTML::queue_global_task(HTML::Task::Source::Unspecified, *m_relevant_global, GC::create_function(GC::Heap::the(), [this, event_name] {
        dispatch_event(DOM::Event::create(*m_relevant_global, event_name));
    }));
}

NotificationData Notification::data() const
{
    return NotificationData {
        .title = Utf16String::from_utf8(m_notification.title),
        .body = m_notification.body,
        .tag = m_notification.tag,
        .language = m_notification.language,
        .direction = m_notification.direction,
        .silent = m_notification.silent_preference,
        .require_interaction = m_notification.require_interaction_preference,
        .renotify = m_notification.renotify_preference,
    };
}

#define __ENUMERATE(attribute_name, event_name)                           \
    void Notification::set_##attribute_name(WebIDL::CallbackType* value)  \
    {                                                                     \
        set_event_handler_attribute(HTML::EventNames::event_name, value); \
    }                                                                     \
    WebIDL::CallbackType* Notification::attribute_name()                  \
    {                                                                     \
        return event_handler_attribute(HTML::EventNames::event_name);     \
    }
__ENUMERATE(onclick, click)
__ENUMERATE(onshow, show)
__ENUMERATE(onerror, error)
__ENUMERATE(onclose, close)
#undef __ENUMERATE

// https://notifications.spec.whatwg.org/#dom-notification-permission
Bindings::NotificationPermission Notification::permission()
{
    // The static permission getter steps are to return the result of getting the notifications permission state.
    // https://notifications.spec.whatwg.org/#permissions-integration
    // 1. Let permissionState be the result of getting the current permission state with "notifications".
    auto permission_state = get_the_notifications_permission_state(HTML::current_settings_object());

    // 2. If permissionState is "prompt", then return "default".
    // 3. Return permissionState.
    switch (permission_state) {
    case PermissionsAPI::PermissionState::Prompt:
        return Bindings::NotificationPermission::Default;
    case PermissionsAPI::PermissionState::Granted:
        return Bindings::NotificationPermission::Granted;
    case PermissionsAPI::PermissionState::Denied:
        return Bindings::NotificationPermission::Denied;
    }
    VERIFY_NOT_REACHED();
}

// https://notifications.spec.whatwg.org/#dom-notification-actions
Vector<NotificationAction> Notification::actions() const
{
    // 1. Let frozenActions be an empty list of type NotificationAction.
    Vector<NotificationAction> frozen_actions;
    frozen_actions.ensure_capacity(m_notification.actions.capacity());

    // 2. For each entry of this’s notification’s actions:
    for (auto const& entry : m_notification.actions) {
        // 1. Let action be a new NotificationAction.
        NotificationAction action;

        // 2. Set action["action"] to entry’s name.
        action.action = entry.name;

        // 3. Set action["title"] to entry’s title.
        action.title = entry.title;

        // 4. If entry’s navigation URL is non-null, then set action["navigate"] to entry’s navigation URL, serialized.
        if (entry.navigation_url.has_value())
            action.navigate = serialize_url_for_bindings(entry.navigation_url);

        // 5. If entry’s icon URL is non-null, then set action["icon"] to entry’s icon URL, serialized.
        if (entry.icon_url.has_value())
            action.icon = serialize_url_for_bindings(entry.icon_url);

        // FIXME: 6. Call Object.freeze on action, to prevent accidental mutation by scripts.

        // 7. Append action to frozenActions.
        frozen_actions.append(action);
    }

    // 3. Return the result of create a frozen array from frozenActions.
    // NB: The bindings freeze the array they build from this sequence.
    return frozen_actions;
}

// https://notifications.spec.whatwg.org/#dom-notification-data
Utf16String Notification::serialize_url_for_bindings(Optional<URL::URL> const& url)
{
    if (!url.has_value())
        return {};
    return Utf16String::from_utf8(url->serialize());
}

JS::Value Notification::data(JS::Object const& relevant_global_object) const
{
    // The data getter steps are to return StructuredDeserialize(this's notification's data, this's relevant Realm).
    // If this throws an exception, then return null.
    auto& relevant_realm = HTML::relevant_realm(relevant_global_object);
    auto deserialized_data = HTML::structured_deserialize(relevant_realm.vm(), m_notification.data, relevant_realm);
    if (!deserialized_data.is_exception())
        return deserialized_data.release_value();
    return JS::js_null();
}

}
