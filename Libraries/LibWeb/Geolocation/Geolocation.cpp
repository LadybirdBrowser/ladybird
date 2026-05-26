/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <LibGC/Weak.h>
#include <LibWeb/Bindings/Geolocation.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/Geolocation/GeolocationPosition.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Geolocation {

static constexpr u32 VISIBILITY_STATE_TIMEOUT_MS = 5'000;

static WebIDL::UnsignedLong s_next_watch_id = 0;

class GeolocationRequestState : public RefCounted<GeolocationRequestState> {
public:
    u64 request_id { 0 };
    bool completed { false };
    bool timeout_stopped { false };
};

GC_DEFINE_ALLOCATOR(Geolocation);

Geolocation::Geolocation(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void Geolocation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Geolocation);
    Base::initialize(realm);
}

void Geolocation::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& watch_position_data : m_watch_position_data) {
        visitor.visit(watch_position_data.value.success_callback);
        visitor.visit(watch_position_data.value.error_callback);
    }
    visitor.visit(m_cached_position);
    visitor.visit(m_timeout_timers);
}

// https://w3c.github.io/geolocation/#dom-geolocation-getcurrentposition
void Geolocation::get_current_position(GC::Ref<WebIDL::CallbackType> success_callback,
    GC::Ptr<WebIDL::CallbackType> error_callback, Bindings::PositionOptions const& options)
{
    // 1. If this's relevant global object's associated Document is not fully active:
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));
    if (!window.associated_document().is_fully_active()) {
        // 1. Call back with error errorCallback and POSITION_UNAVAILABLE.
        call_back_with_error(error_callback, GeolocationPositionError::ErrorCode::PositionUnavailable);

        // 2. Terminate this algorithm.
        return;
    }

    // 2. Request a position passing this, successCallback, errorCallback, and options.
    request_a_position(success_callback, error_callback, options);
}

// https://w3c.github.io/geolocation/#watchposition-method
WebIDL::Long Geolocation::watch_position(GC::Ref<WebIDL::CallbackType> success_callback,
    GC::Ptr<WebIDL::CallbackType> error_callback, Bindings::PositionOptions const& options)
{
    // 1. If this's relevant global object's associated Document is not fully active:
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));
    if (!window.associated_document().is_fully_active()) {
        // 1. Call back with error passing errorCallback and POSITION_UNAVAILABLE.
        call_back_with_error(error_callback, GeolocationPositionError::ErrorCode::PositionUnavailable);

        // 2. Return 0.
        return 0;
    }

    // 2. Let watchId be an implementation-defined unsigned long that is greater than zero.
    auto watch_id = ++s_next_watch_id;

    // 3. Append watchId to this's [[watchIDs]].
    m_watch_ids.set(watch_id);
    m_watch_position_data.set(watch_id, { success_callback, error_callback, options });

    // 4. Request a position passing this, successCallback, errorCallback, options, and watchId.
    request_a_position(success_callback, error_callback, options, watch_id);

    // 5. Return watchId.
    return watch_id;
}

// https://w3c.github.io/geolocation/#clearwatch-method
void Geolocation::clear_watch(WebIDL::Long watch_id)
{
    // 1. Remove watchId from this's [[watchIDs]].
    remove_watch_id(watch_id);

    if (auto request_id = m_pending_watch_position_request_ids.take(watch_id); request_id.has_value()) {
        auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
        document.page().cancel_geolocation_position_request(*request_id);
    }
}

// https://w3c.github.io/geolocation/#dfn-acquire-a-position
void Geolocation::acquire_a_position(GC::Ref<WebIDL::CallbackType> success_callback,
    GC::Ptr<WebIDL::CallbackType> error_callback, Bindings::PositionOptions const& options, Optional<WebIDL::UnsignedLong> watch_id)
{
    // 1. If watchId was passed and this's [[watchIDs]] does not contain watchId, terminate this algorithm.
    if (watch_id.has_value() && !m_watch_ids.contains(watch_id.value()))
        return;

    // 2. Let acquisitionTime be a new EpochTimeStamp that represents now.
    HighResolutionTime::EpochTimeStamp const acquisition_time = AK::UnixDateTime::now().milliseconds_since_epoch();

    // 3. Let timeoutTime be the sum of acquisitionTime and options.timeout.
    [[maybe_unused]] HighResolutionTime::EpochTimeStamp const timeout_time = acquisition_time + options.timeout;

    // 4. Let cachedPosition be this's [[cachedPosition]].
    auto cached_position = m_cached_position;

    // 5. Create an implementation-specific timeout task that elapses at timeoutTime, during which it tries to acquire
    //    the device's position by running the following steps:
    {
        // 1. Let permission be get the current permission state of "geolocation".
        auto permission = Web::PermissionsAPI::permission_state(Bindings::PermissionDescriptor { "geolocation"_utf16 });

        // 2. If permission is "denied":
        if (permission == Bindings::PermissionState::Denied) {
            // 1. Stop timeout.
            // NB: No timeout is running yet because we start the timer immediately before requesting native position
            //     data below.

            // 2. Do the user or system denied permission failure case step.
            call_back_with_error(error_callback, GeolocationPositionError::ErrorCode::PermissionDenied);
            return;
        }

        // 3. If permission is "granted":
        if (permission == Bindings::PermissionState::Granted) {
            // 1. Check if an emulated position should be used by running the following steps:
            {
                // 1. Let emulatedPositionData be get emulated position data passing this.
                auto emulated_position_data = get_emulated_position_data();

                // 2. If emulatedPositionData is not null:
                if (!emulated_position_data.has<Empty>()) {
                    // 1. If emulatedPositionData is a GeolocationPositionError:
                    if (emulated_position_data.has<GeolocationPositionError::ErrorCode>()) {
                        // 1. Call back with error passing errorCallback and emulatedPositionData.
                        // FIXME: We pass along the code instead of the entire error object. Spec issue:
                        //        https://github.com/w3c/geolocation/issues/186
                        call_back_with_error(error_callback, emulated_position_data.get<GeolocationPositionError::ErrorCode>());

                        // 2. Terminate this algorithm.
                        return;
                    }

                    // 2. Let position be a new GeolocationPosition passing emulatedPositionData, acquisitionTime and
                    //    options.enableHighAccuracy.
                    auto position = realm().create<GeolocationPosition>(realm(),
                        emulated_position_data.get<GC::Ref<GeolocationCoordinates>>(),
                        acquisition_time,
                        options.enable_high_accuracy);

                    // 3. Queue a task on the geolocation task source with a step that invokes successCallback with
                    //    « position » and "report".
                    HTML::queue_a_task(HTML::Task::Source::Geolocation, nullptr, nullptr, GC::create_function(heap(), [success_callback, position] {
                        (void)WebIDL::invoke_callback(success_callback, {}, WebIDL::ExceptionBehavior::Report, { { position } });
                    }));

                    // 4. Terminate this algorithm.
                    return;
                }
            }

            // 2. Let position be null.
            // NB: position is created inside the async callback in step 6 below.

            // 3. If cachedPosition is not null, and options.maximumAge is greater than 0:
            if (cached_position && options.maximum_age > 0) {
                // 1. Let cacheTime be acquisitionTime minus the value of the options.maximumAge member.
                HighResolutionTime::EpochTimeStamp const cache_time = acquisition_time - options.maximum_age;

                // 2. If cachedPosition's timestamp's value is greater than cacheTime, and
                //    cachedPosition.[[isHighAccuracy]] equals options.enableHighAccuracy:
                if (cached_position->timestamp() > cache_time
                    && cached_position->is_high_accuracy() == options.enable_high_accuracy) {
                    // 1. Queue a task on the geolocation task source with a step that invokes successCallback with
                    //    « cachedPosition » and "report".
                    HTML::queue_a_task(HTML::Task::Source::Geolocation, nullptr, nullptr, GC::create_function(heap(), [success_callback, cached_position] {
                        (void)WebIDL::invoke_callback(success_callback, {}, WebIDL::ExceptionBehavior::Report, { { cached_position } });
                    }));

                    // 2. Terminate this algorithm.
                    return;
                }
            }

            // 4. Otherwise, if position is not cachedPosition, try to acquire position data from the underlying system,
            //    optionally taking into consideration the value of options.enableHighAccuracy during acquisition.
            acquire_position_from_page(success_callback, error_callback, options, watch_id, acquisition_time);
        }
    }
}

// https://w3c.github.io/geolocation/#dfn-acquire-a-position
void Geolocation::acquire_position_from_page(GC::Ref<WebIDL::CallbackType> success_callback,
    GC::Ptr<WebIDL::CallbackType> error_callback, Bindings::PositionOptions const& options,
    Optional<WebIDL::UnsignedLong> watch_id, HighResolutionTime::EpochTimeStamp acquisition_time)
{
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    auto& page = document.page();

    auto request_state = make_ref_counted<GeolocationRequestState>();
    auto weak_document = GC::Weak<DOM::Document> { document };
    auto weak_page = GC::Weak<Page> { page };
    auto timeout_timer = Platform::Timer::create_single_shot(heap(), min(options.timeout, NumericLimits<int>::max()), {});
    m_timeout_timers.append(timeout_timer);

    auto stop_timeout_timer = [this, timeout_timer, request_state] {
        if (request_state->timeout_stopped)
            return;
        request_state->timeout_stopped = true;
        timeout_timer->stop();
        timeout_timer->on_timeout = nullptr;
        m_timeout_timers.remove_first_matching([&](auto timer) { return timer == timeout_timer; });
    };

    auto position_callback = GC::create_function(heap(),
        [this, success_callback, error_callback, options, watch_id, acquisition_time, request_state, weak_document, stop_timeout_timer](Page::GeolocationPositionResult result) {
            if (request_state->completed)
                return;

            if (watch_id.has_value() && !m_watch_ids.contains(*watch_id)) {
                // NB: The position response arrived after the watch was cleared; stop the timeout before ignoring it.
                stop_timeout_timer();
                return;
            }
            if (!watch_id.has_value())
                request_state->completed = true;

            auto document = weak_document.ptr();
            if (!document || !document->is_fully_active()) {
                // NB: The position response is no longer observable; stop the timeout before ignoring it.
                stop_timeout_timer();
                return;
            }

            // 5. If the timeout elapses during acquisition, or acquiring the device's position results in failure:
            if (result.has<GeolocationPositionError::ErrorCode>()) {
                auto error_code = result.get<GeolocationPositionError::ErrorCode>();

                // 1. Stop the timeout.
                stop_timeout_timer();

                if (watch_id.has_value() && error_code == GeolocationPositionError::ErrorCode::PermissionDenied) {
                    request_state->completed = true;
                    m_pending_watch_position_request_ids.remove(*watch_id);
                }

                // 2. Go to dealing with failures.
                call_back_with_error(error_callback, error_code);

                // 3. Terminate this algorithm.
                return;
            }

            // 6. If acquiring the position data from the system succeeds:
            auto const& coordinates_data = result.get<CoordinatesData>();
            auto position_acquisition_time = watch_id.has_value()
                ? UnixDateTime::now().milliseconds_since_epoch()
                : acquisition_time;

            // 1. Let positionData be a map with the following name/value pairs based on the acquired position data:
            //    * longitude:
            //        A double that represents the longitude coordinates on the Earth's surface in degrees, using
            //        the [WGS84] coordinate system. Longitude measures how far east or west a point is from the
            //        Prime Meridian.
            //    * altitude:
            //        A double? that represents the altitude in meters above the [WGS84] ellipsoid, or null if not
            //        available. Altitude measures the height above sea level.
            //    * accuracy:
            //        A non-negative double that represents the accuracy value indicating the 95% confidence level
            //        in meters. Accuracy measures how close the measured coordinates are to the true position.
            //    * altitudeAccuracy:
            //        A non-negative double? that represents the altitude accuracy, or null if not available,
            //        indicating the 95% confidence level in meters. Altitude accuracy measures how close the
            //        measured altitude is to the true altitude.
            //    * speed:
            //        A non-negative double? that represents the speed in meters per second, or null if not
            //        available. Speed measures how fast the device is moving.
            //    * heading:
            //        A double? that represents the heading in degrees, or null if not available or the device is
            //        stationary. Heading measures the direction in which the device is moving relative to true
            //        north.
            GC::Ref<GeolocationCoordinates> position_data = realm().create<GeolocationCoordinates>(realm(), coordinates_data);

            // 2. Set position to a new GeolocationPosition passing positionData, acquisitionTime and
            //    options.enableHighAccuracy.
            auto position = realm().create<GeolocationPosition>(realm(), position_data, position_acquisition_time, options.enable_high_accuracy);

            // 3. Set this's [[cachedPosition]] to position.
            m_cached_position = *position;

            // NB: Reaching this point means acquiring position data succeeded, as the failure path in step 5
            //     terminates the algorithm.

            // 7. Stop the timeout.
            stop_timeout_timer();

            // 8. Queue a task on the geolocation task source with a step that invokes successCallback with
            //    « position » and "report".
            HTML::queue_a_task(HTML::Task::Source::Geolocation, nullptr, nullptr, GC::create_function(heap(), [success_callback, position] {
                (void)WebIDL::invoke_callback(success_callback, {}, WebIDL::ExceptionBehavior::Report, { { position } });
            }));
        });

    request_state->request_id = page.request_geolocation_position(position_callback, watch_id.has_value() ? Page::GeolocationRequestType::Watch : Page::GeolocationRequestType::OneShot);
    if (watch_id.has_value())
        m_pending_watch_position_request_ids.set(*watch_id, request_state->request_id);

    // 5. If the timeout elapses during acquisition, or acquiring the device's position results in failure:
    timeout_timer->on_timeout = GC::create_function(heap(), [this, error_callback, watch_id, request_state, weak_document, weak_page, stop_timeout_timer] {
        if (request_state->completed)
            return;
        request_state->completed = true;

        // 1. Stop the timeout.
        stop_timeout_timer();

        if (watch_id.has_value() && !m_pending_watch_position_request_ids.remove(*watch_id))
            return;

        if (auto page = weak_page.ptr())
            page->cancel_geolocation_position_request(request_state->request_id);

        auto document = weak_document.ptr();
        if (!document || !document->is_fully_active())
            return;

        // 2. Go to dealing with failures.
        call_back_with_error(error_callback, GeolocationPositionError::ErrorCode::Timeout);

        // 3. Terminate this algorithm.
        // NB: This is dealt with by setting request_state->completed above.
    });
    timeout_timer->start();
}

// https://w3c.github.io/geolocation/#dfn-call-back-with-error
void Geolocation::call_back_with_error(GC::Ptr<WebIDL::CallbackType> callback, GeolocationPositionError::ErrorCode code) const
{
    // 1. If callback is null, return.
    if (!callback)
        return;

    // 2. Let error be a newly created GeolocationPositionError instance whose code attribute is initialized to code.
    auto error = realm().create<GeolocationPositionError>(realm(), code);

    // 3. Queue a task on the geolocation task source with a step that invokes callback with « error » and "report".
    HTML::queue_a_task(HTML::Task::Source::Geolocation, nullptr, nullptr, GC::create_function(heap(), [callback, error] {
        (void)WebIDL::invoke_callback(*callback, {}, WebIDL::ExceptionBehavior::Report, { { error } });
    }));
}

// https://w3c.github.io/geolocation/#dfn-get-emulated-position-data
EmulatedPositionData Geolocation::get_emulated_position_data() const
{
    // 1. Let navigable be geolocation's relevant global object's associated Document's node navigable.
    auto navigable = as<HTML::Window>(HTML::relevant_global_object(*this)).navigable();

    // 2. If navigable is null, return null.
    if (!navigable)
        return Empty {};

    // 3. Let traversable be navigable’s top-level traversable.
    auto traversable = navigable->top_level_traversable();

    // 4. If traversable is null, return null.
    // NB: This cannot be null.
    // 5. Return traversable's associated emulated position data.
    return as<HTML::LocalTraversableNavigable>(*traversable).emulated_position_data();
}

void Geolocation::remove_watch_id(WebIDL::UnsignedLong watch_id)
{
    m_watch_ids.remove(watch_id);

    // NB: watchPosition() runs while [[watchIDs]] contains watchId. We use an emulated position data observer to
    //     implement the spec's significant-change wait, so stop observing when the watch id leaves [[watchIDs]].
    unregister_watch_position_observer(watch_id);
    m_watch_position_data.remove(watch_id);
}

GC::Ptr<HTML::LocalTraversableNavigable> Geolocation::top_level_traversable() const
{
    auto navigable = as<HTML::Window>(HTML::relevant_global_object(*this)).navigable();
    if (!navigable)
        return nullptr;
    return &as<HTML::LocalTraversableNavigable>(*navigable->top_level_traversable());
}

void Geolocation::unregister_watch_position_observer(WebIDL::UnsignedLong watch_id)
{
    auto watch_position_data = m_watch_position_data.get(watch_id);
    if (!watch_position_data.has_value() || !watch_position_data->emulated_position_data_observer_id.has_value())
        return;

    if (auto traversable = top_level_traversable())
        traversable->unregister_emulated_position_data_observer(*watch_position_data->emulated_position_data_observer_id);
    watch_position_data->emulated_position_data_observer_id = {};
}

// https://w3c.github.io/geolocation/#dfn-request-a-position
void Geolocation::request_a_position(GC::Ref<WebIDL::CallbackType> success_callback,
    GC::Ptr<WebIDL::CallbackType> error_callback, Bindings::PositionOptions const& options, Optional<WebIDL::UnsignedLong> watch_id)
{
    // 1. Let watchIDs be geolocation's [[watchIDs]].

    // 2. Let document be the geolocation's relevant global object's associated Document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // FIXME: 3. If document is not allowed to use the "geolocation" feature:
    if (false) {
        // 1. If watchId was passed, remove watchId from watchIDs.
        if (watch_id.has_value())
            remove_watch_id(watch_id.value());

        // 2. Call back with error passing errorCallback and PERMISSION_DENIED.
        call_back_with_error(error_callback, GeolocationPositionError::ErrorCode::PermissionDenied);

        // 3. Terminate this algorithm.
        return;
    }

    // 4. If geolocation's environment settings object is a non-secure context:
    if (is_non_secure_context(HTML::relevant_settings_object(*this))) {
        // 1. If watchId was passed, remove watchId from watchIDs.
        if (watch_id.has_value())
            remove_watch_id(watch_id.value());

        // 2. Call back with error passing errorCallback and PERMISSION_DENIED.
        call_back_with_error(error_callback, GeolocationPositionError::ErrorCode::PermissionDenied);

        // 3. Terminate this algorithm.
        return;
    }

    // 5. If document's visibility state is "hidden", wait for the following page visibility change steps to run:
    run_in_parallel_when_document_is_visible(document, GC::create_function(heap(), [this, watch_id, success_callback, error_callback, options] {
        // 1. Assert: document's visibility state is "visible".
        // 2. Continue to the next steps below.
        // AD-HOC: This is implemented by run_in_parallel_when_document_is_visible().

        // 6. Let descriptor be a new PermissionDescriptor whose name is "geolocation".
        auto descriptor = Bindings::PermissionDescriptor { "geolocation"_utf16 };

        // 7. In parallel:
        // AD-HOC: run_in_parallel_when_document_is_visible() already runs this in parallel.
        {
            // 1. Set permission to request permission to use descriptor.
            auto permission = Web::PermissionsAPI::request_permission(descriptor);

            // 2. If permission is "denied", then:
            if (permission == Bindings::PermissionState::Denied) {
                // 1. If watchId was passed, remove watchId from watchIDs.
                if (watch_id.has_value())
                    remove_watch_id(watch_id.value());

                // 2. Call back with error passing errorCallback and PERMISSION_DENIED.
                call_back_with_error(error_callback, GeolocationPositionError::ErrorCode::PermissionDenied);

                // 3. Terminate this algorithm.
                return;
            }

            // 3. Wait to acquire a position passing successCallback, errorCallback, options, and watchId.
            acquire_a_position(success_callback, error_callback, options, watch_id);

            // 4. If watchId was not passed, terminate this algorithm.
            if (!watch_id.has_value())
                return;

            if (!m_watch_ids.contains(*watch_id))
                return;

            // 5. While watchIDs contains watchId:
            // AD-HOC: The spec expresses this as a loop; we register callbacks for significant position changes and
            //         re-enter request a position from those callbacks.
            // 1. Wait for a significant change of geographic position. What constitutes a significant change of
            //    geographic position is left to the implementation. User agents MAY impose a rate limit on how
            //    frequently position changes are reported. User agents MUST consider invoking set emulated position
            //    data as a significant change.
            auto watch_position_data = m_watch_position_data.get(*watch_id);
            if (!watch_position_data.has_value() || watch_position_data->emulated_position_data_observer_id.has_value())
                return;

            auto traversable = top_level_traversable();
            if (!traversable)
                return;

            auto weak_document = GC::Weak<DOM::Document> { as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document() };
            auto observer_id = traversable->register_emulated_position_data_observer(GC::create_function(heap(), [this, watch_id = *watch_id, weak_document] {
                if (!m_watch_ids.contains(watch_id))
                    return;

                auto document = weak_document.ptr();
                if (!document || !document->is_fully_active())
                    return;

                if (m_pending_watch_position_request_ids.contains(watch_id))
                    return;

                auto watch_position_data = m_watch_position_data.get(watch_id);
                if (!watch_position_data.has_value())
                    return;

                // FIXME: 2. If document is not fully active or visibility state is not "visible", go back to the
                //    previous step and again wait for a significant change of geographic position.

                // 3. Wait to acquire a position passing successCallback, errorCallback, options, and watchId.
                request_a_position(watch_position_data->success_callback, watch_position_data->error_callback, watch_position_data->options, watch_id);
            }));
            watch_position_data->emulated_position_data_observer_id = observer_id;
        }
    }));
}

void Geolocation::run_in_parallel_when_document_is_visible(DOM::Document& document, GC::Ref<GC::Function<void()>> callback)
{
    // Run callback in parallel if the document is already visible.
    if (document.visibility_state_value() == HTML::VisibilityState::Visible) {
        auto callback_with_context = GC::create_function(heap(), [this, callback] {
            HTML::TemporaryExecutionContext execution_context { realm() };
            callback->function()();
        });
        Platform::EventLoopPlugin::the().deferred_invoke(callback_with_context);
        return;
    }

    // Run the callback as soon as the document becomes visible. If we time out, do not run the callback at all.
    auto document_observer = realm().create<DOM::DocumentObserver>(realm(), document);
    auto timeout_timer = Platform::Timer::create_single_shot(heap(), VISIBILITY_STATE_TIMEOUT_MS, {});
    m_timeout_timers.append(timeout_timer);
    auto clear_observer_and_timer = [this, document_observer, timeout_timer] {
        document_observer->set_document_visibility_state_observer({});
        timeout_timer->stop();
        m_timeout_timers.remove_first_matching([&](auto timer) { return timer == timeout_timer; });
    };
    timeout_timer->on_timeout = GC::create_function(heap(), [clear_observer_and_timer] {
        dbgln("Geolocation: Waiting for visibility state update timed out");
        clear_observer_and_timer();
    });

    document_observer->set_document_visibility_state_observer([this, clear_observer_and_timer, callback](HTML::VisibilityState state) {
        if (state == HTML::VisibilityState::Visible) {
            clear_observer_and_timer();
            HTML::TemporaryExecutionContext execution_context { realm() };
            callback->function()();
        }
    });
    timeout_timer->start();
}

}
