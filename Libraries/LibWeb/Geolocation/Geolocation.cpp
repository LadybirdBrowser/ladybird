/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/GeolocationPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/Geolocation/GeolocationPosition.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>
#include <LibWeb/Geolocation/GeolocationUpdateState.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(Geolocation);

GC::Ref<Geolocation> Geolocation::create(JS::Realm& realm)
{
    return realm.create<Geolocation>(realm);
}

Geolocation::Geolocation(JS::Realm& realm)
    : PlatformObject(realm)
{
}

Geolocation::~Geolocation() = default;

void Geolocation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Geolocation);
    Base::initialize(realm);
}

void Geolocation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_cached_position.has_value())
        visitor.visit(*m_cached_position);
}

// https://w3c.github.io/geolocation/#getcurrentposition-method
void Geolocation::get_current_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback, Optional<PositionOptions> options)
{
    // 1. If this's relevant global object's associated Document is not fully active:
    if (!as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document().is_fully_active()) {
        // 1.1. Call back with error errorCallback and POSITION_UNAVAILABLE.
        call_back_with_error(error_callback, GeolocationPositionError::POSITION_UNAVAILABLE);

        // 1.2. Terminate this algorithm.
        return;
    }

    // 2. Request a position passing this, successCallback, errorCallback, and options.
    request_position(success_callback, error_callback, options);
}

// https://w3c.github.io/geolocation/#watchposition-method
WebIDL::Long Geolocation::watch_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback, Optional<PositionOptions> options)
{
    // 1. If this's relevant global object's associated Document is not fully active:
    if (!as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document().is_fully_active()) {
        // 1.1. Call back with error errorCallback and POSITION_UNAVAILABLE.
        call_back_with_error(error_callback, GeolocationPositionError::POSITION_UNAVAILABLE);

        // 1.2. Terminate this algorithm.
        return 0;
    }

    // 2. Let watchId be an implementation-defined unsigned long that is greater than zero.
    auto watch_id = ++m_next_watch_id;

    // 3. Append watchId to this's [[watchIDs]].
    m_watch_ids.set(watch_id);

    // 4. Request a position passing this, successCallback, errorCallback, options, and watchId.
    request_position(success_callback, error_callback, options, watch_id);

    // 5. Return watchId.
    return watch_id;
}

// https://w3c.github.io/geolocation/#clearwatch-method
void Geolocation::clear_watch(WebIDL::Long watch_id)
{
    // 1. Remove watchId from this's [[watchIDs]].
    m_watch_ids.remove(watch_id);

    // Stop geolocation watch.
    if (auto request_id = m_watch_request_ids.take(watch_id); request_id.has_value()) {
        auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));
        window.page().stop_geolocation_watch(*request_id);
    }
}

// https://w3c.github.io/geolocation/#request-a-position
void Geolocation::request_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback, Optional<PositionOptions> options, Optional<WebIDL::Long> watch_id)
{
    auto const& realm = HTML::relevant_realm(*this);
    auto const& window = as<HTML::Window>(HTML::relevant_global_object(*this));

    // 1. Let watchIDs be geolocation's [[watchIDs]].
    // -

    // 2. Let document be the geolocation's relevant global object's associated Document.
    auto const& document = window.associated_document();

    // 3. If document is not allowed to use the "geolocation" feature:
    // 3.1. If watchId was passed, remove watchId from watchIDs.
    // 3.2. Call back with error passing errorCallback and PERMISSION_DENIED.
    // 3.3. Terminate this algorithm.
    // FIXME

    // 4. If geolocation's environment settings object is a non-secure context:
    if (!window.is_secure_context()) {
        // 4.1. If watchId was passed, remove watchId from watchIDs.
        if (watch_id.has_value())
            m_watch_ids.remove(*watch_id);

        // 4.2. Call back with error passing errorCallback and PERMISSION_DENIED.
        call_back_with_error(error_callback, GeolocationPositionError::PERMISSION_DENIED);

        // 4.3. Terminate this algorithm.
        return;
    }

    // 5. If document's visibility state is "hidden", wait for the following page visibility change steps to run:
    if (document.visibility_state() == HTML::VisibilityState::Hidden) {
        // 5.1. Assert: document's visibility state is "visible".
        // 5.2. Continue to the next steps below.
        // FIXME
    }

    // 6. Let descriptor be a new PermissionDescriptor whose name is "geolocation".
    // FIXME

    // 7. In parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &success_callback, error_callback, options, watch_id]() {
        // 7.1. Set permission to request permission to use descriptor.
        // FIXME

        // 7.2. If permission is "denied", then:
        // 7.2.1. If watchId was passed, remove watchId from watchIDs.
        // 7.2.2. Call back with error passing errorCallback and PERMISSION_DENIED.
        // 7.2.3. Terminate this algorithm.
        // FIXME

        // 7.3. Wait to acquire a position passing successCallback, errorCallback, options, and watchId.
        acquire_position(success_callback, error_callback, options, watch_id);

        // 7.4. If watchId was not passed, terminate this algorithm.
        if (!watch_id.has_value())
            return;

        // 7.5. While watchIDs contains watchId:
        // 7.5.1. Wait for a significant change of geographic position. What constitutes a significant change of geographic position is left to the implementation. User agents MAY impose a rate limit on how frequently position changes are reported. User agents MUST consider invoking set emulated position data as a significant change.
        // 7.5.2. If document is not fully active or visibility state is not "visible", go back to the previous step and again wait for a significant change of geographic position.
        // 7.5.3. Wait to acquire a position passing successCallback, errorCallback, options, and watchId.
        // FIXME
    }));
}

// https://w3c.github.io/geolocation/#acquire-a-position
void Geolocation::acquire_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback, Optional<PositionOptions> options, Optional<WebIDL::Long> watch_id)
{
    auto& realm = HTML::relevant_realm(*this);
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));

    // 1. If watchId was passed and this's [[watchIDs]] does not contain watchId, terminate this algorithm.
    if (watch_id.has_value() && !m_watch_ids.contains(*watch_id))
        return;

    // 2. Let acquisitionTime be a new EpochTimeStamp that represents now.
    // HighResolutionTime::EpochTimeStamp acquisition_time = UnixDateTime::now().milliseconds_since_epoch();

    // 3. Let timeoutTime be the sum of acquisitionTime and options.timeout.
    // -

    // 4. Let cachedPosition be this's [[cachedPosition]].
    // auto cached_position = m_cached_position;

    // 5. Create an implementation-specific timeout task that elapses at timeoutTime, during which it tries to acquire the device's position by running the following steps:
    // FIXME

    // 5.1. Let permission be get the current permission state of "geolocation".
    // FIXME

    // 5.2. If permission is "denied":
    // 5.2.1. Stop timeout.
    // 5.2.2. Do the user or system denied permission failure case step.
    // FIXME

    // 5.3. If permission is "granted":
    if (true) { // FIXME
        // 5.3.1. Check if an emulated position should be used by running the following steps:
        // 5.3.1.1. Let emulatedPositionData be get emulated position data passing this.
        // 5.3.1.2. If emulatedPositionData is not null:
        // 5.3.1.2.1. If emulatedPositionData is a GeolocationPositionError:
        // 5.3.1.2.1.1. Call back with error passing errorCallback and emulatedPositionData.
        // 5.3.1.2.1.2. Terminate this algorithm.
        // 5.3.1.2.2. Let position be a new GeolocationPosition passing emulatedPositionData, acquisitionTime and options.enableHighAccuracy.
        // 5.3.1.2.3. Queue a task on the geolocation task source with a step that invokes successCallback with « position » and "report".
        // 5.3.1.2.4. Terminate this algorithm.
        // FIXME

        // 5.3.2. Let position be null.
        // -

        // 5.3.3. If cachedPosition is not null, and options.maximumAge is greater than 0:
        // if (cached_position.has_value() && options->maximum_age > 0) {
        //     auto const& cached_position_ptr = *cached_position.ptr();

        //     // 5.3.3.1. Let cacheTime be acquisitionTime minus the value of the options.maximumAge member.
        //     auto cache_time = acquisition_time - options->maximum_age;

        //     // 5.3.3.2. If cachedPosition's timestamp's value is greater than cacheTime, and cachedPosition.[[isHighAccuracy]] equals options.enableHighAccuracy:
        //     if (cached_position_ptr->timestamp() > cache_time && cached_position_ptr->is_high_accuracy() == options->enable_high_accuracy) {
        //         // 5.3.3.2.1. Queue a task on the geolocation task source with a step that invokes successCallback with « cachedPosition » and "report".
        //         HTML::queue_global_task(HTML::Task::Source::Geolocation, *this, GC::create_function(realm.heap(), [&success_callback, cached_position_ptr]() mutable {
        //             (void)WebIDL::invoke_callback(success_callback, JS::js_undefined(), WebIDL::ExceptionBehavior::Report, { { { cached_position_ptr } } });
        //         }));

        //         // 5.3.3.2.2. Terminate this algorithm.
        //         return;
        //     }
        // }

        // 5.3.4. Otherwise, if position is not cachedPosition, try to acquire position data from the underlying system, optionally taking into consideration the value of options.enableHighAccuracy during acquisition.
        auto request_id = window.page().request_geolocation(options->enable_high_accuracy, watch_id.has_value(), GC::create_function(realm.heap(), [this, &realm, &success_callback, error_callback, options](GeolocationUpdateState update_state) {
            if (update_state.has<GeolocationUpdatePosition>()) {
                auto const& update_position = update_state.get<GeolocationUpdatePosition>();
                auto coords = GeolocationCoordinates::create(realm, update_position.accuracy, update_position.latitude, update_position.longitude, update_position.altitude, update_position.altitude_accuracy, update_position.heading, update_position.speed);
                auto position = GeolocationPosition::create(realm, coords, update_position.timestamp.milliseconds_since_epoch(), options->enable_high_accuracy);
                dbgln_if(GEOLOCATION_DEBUG, "Geolocation success callback: {},{}", update_position.latitude, update_position.longitude);
                HTML::queue_global_task(HTML::Task::Source::Geolocation, *this, GC::create_function(realm.heap(), [&success_callback, position]() mutable {
                    (void)WebIDL::invoke_callback(success_callback, JS::js_undefined(), WebIDL::ExceptionBehavior::Report, { { { position } } });
                }));
            }

            if (update_state.has<GeolocationUpdateError>()) {
                auto update_error = update_state.get<GeolocationUpdateError>();
                if (update_error == GeolocationUpdateError::PermissionDenied) {
                    call_back_with_error(error_callback, GeolocationPositionError::PERMISSION_DENIED);
                } else if (update_error == GeolocationUpdateError::PositionUnavailable) {
                    call_back_with_error(error_callback, GeolocationPositionError::POSITION_UNAVAILABLE);
                } else if (update_error == GeolocationUpdateError::Timeout) {
                    call_back_with_error(error_callback, GeolocationPositionError::TIMEOUT);
                }
            }
        }));
        if (watch_id.has_value())
            m_watch_request_ids.set(*watch_id, request_id);

        // 5.3.5. If the timeout elapses during acquisition, or acquiring the device's position results in failure:
        // 5.3.5.1. Stop timeout.
        // 5.3.5.2. Go to dealing with failures.
        // 5.3.5.3. Terminate this algorithm.
        // FIXME

        // 5.3.6. If acquiring the position data from the system succeeds:
        // FIXME

        // 5.3.7. Stop the timeout.
        // FIXME

        // 5.3.8. Queue a task on the geolocation task source with a step that invokes successCallback with « position » and "report".
        // FIXME
    }
}

// https://w3c.github.io/geolocation/#dfn-call-back-with-error
void Geolocation::call_back_with_error(WebIDL::CallbackType* error_callback, WebIDL::UnsignedShort code)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. If callback is null, return.
    if (!error_callback)
        return;

    // 2. Let error be a newly created GeolocationPositionError instance whose code attribute is initialized to code.
    auto error = GeolocationPositionError::create(realm, code);

    // 3. Queue a task on the geolocation task source with a step that invokes callback with « error » and "report".
    dbgln_if(GEOLOCATION_DEBUG, "Geolocation error callback: {}", code);
    HTML::queue_global_task(HTML::Task::Source::Geolocation, *this, GC::create_function(realm.heap(), [error_callback, error]() mutable {
        (void)WebIDL::invoke_callback(*error_callback, JS::js_undefined(), WebIDL::ExceptionBehavior::Report, { { { error } } });
    }));
}

}
