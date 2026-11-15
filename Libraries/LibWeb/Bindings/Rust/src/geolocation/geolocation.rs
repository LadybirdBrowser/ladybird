/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Geolocation, https://w3c.github.io/geolocation/#geolocation_interface

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::{HashMap, HashSet};
use std::time::{SystemTime, UNIX_EPOCH};

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Trace, Value, Vm};

use crate::bindings::callback::CallbackFunction;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::environment_settings_object::{CppEnvironmentSettingsObject, relevant_settings_object};
use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{
    Ref, Wrappable, allocate, cell_from_abi, cell_into_abi, with_temporary_execution_context, wrap,
};
use crate::bindings::promise::{TaskSource, deferred_invoke, queue_a_task_without_execution_context};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::document::{CppDocument, WeakCppDocument};
use crate::geolocation::geolocation_coordinates::{CoordinatesData, GeolocationCoordinates};
use crate::geolocation::geolocation_position::GeolocationPosition;
use crate::geolocation::geolocation_position_error::GeolocationPositionError;
use crate::geolocation::geolocation_position_error_code::GeolocationPositionErrorCode;
use crate::html::visibility_state::VisibilityState;
use crate::permissions::permissions::PermissionName;
use crate::permissions_api::permission_status::PermissionState;
use crate::permissions_api::permissions::{permission_state, request_permission_to_use};
use crate::permissions_policy::permissions_policy::PolicyControlledFeature;
use crate::platform::timer::CppTimer;

unsafe extern "C" {
    fn web_geolocation_request_position(window: *mut c_void, request: *mut c_void, is_watch: bool) -> u64;
    fn web_geolocation_cancel_position_request(window: *mut c_void, request_id: u64);
    fn web_geolocation_emulated_position_data(window: *mut c_void, data: &mut EmulatedPositionDataAbi);
    fn web_geolocation_register_emulated_position_data_observer(
        window: *mut c_void,
        geolocation: *mut c_void,
        watch_id: u32,
    ) -> u64;
    fn web_geolocation_unregister_emulated_position_data_observer(window: *mut c_void, observer_id: u64);
    fn web_geolocation_observe_document_visibility(document: *mut c_void, wait: *mut c_void) -> *mut c_void;
    fn web_geolocation_stop_observing_document_visibility(observer: *mut c_void);
}

thread_local! {
    static NEXT_WATCH_ID: Cell<u32> = const { Cell::new(0) };
}

// https://w3c.github.io/geolocation/#dom-positionoptions
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PositionOptions {
    pub(crate) enable_high_accuracy: bool,
    pub(crate) timeout: u32,
    pub(crate) maximum_age: u32,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct PositionRequestArguments {
    success_callback: CallbackFunction,
    error_callback: Option<CallbackFunction>,
    options: PositionOptions,
    watch_id: Option<u32>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct WatchPositionData {
    success_callback: CallbackFunction,
    error_callback: Option<CallbackFunction>,
    options: PositionOptions,
    emulated_position_data_observer_id: Option<u64>,
    observed_document: Option<WeakCppDocument>,
}

/// Web::Geolocation::EmulatedPositionData.
#[repr(C)]
struct EmulatedPositionDataAbi {
    kind: u8,
    coordinates: *mut c_void,
    error_code: u16,
}

/// https://w3c.github.io/geolocation/#dfn-emulated-position-data
enum EmulatedPositionData {
    None,
    Coordinates(Ref<GeolocationCoordinates>),
    Error(GeolocationPositionErrorCode),
}

/// Page::GeolocationPositionResult.
#[repr(C)]
pub(crate) struct GeolocationPositionResultAbi {
    is_error: bool,
    error_code: u16,
    coordinates: CoordinatesData,
}

// https://w3c.github.io/geolocation/#geolocation_interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Geolocation {
    window: CppWindow,

    // https://w3c.github.io/geolocation/#dfn-watchids
    #[gc(untraced)]
    watch_ids: RefCell<HashSet<u32>>,
    watch_position_data: GcRefCell<HashMap<u32, WatchPositionData>>,
    pending_watch_position_requests: GcRefCell<HashMap<u32, PositionRequestRef>>,

    // https://w3c.github.io/geolocation/#dfn-cachedposition
    cached_position: GcRefCell<Option<Ref<GeolocationPosition>>>,

    // The token, and the watch whose clearing stops the timer.
    timeout_timers: GcRefCell<Vec<(u64, Option<u32>, CppTimer)>>,
    // The document doesn't keep its observers alive.
    visibility_waits: GcRefCell<Vec<VisibilityWaitRef>>,
    #[gc(untraced)]
    next_timeout_timer: Cell<u64>,
}

impl Wrappable for Geolocation {}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct PositionRequest {
    geolocation: Ref<Geolocation>,
    arguments: PositionRequestArguments,
    weak_document: WeakCppDocument,
    request_id: Cell<u64>,
    completed: Cell<bool>,
    // A watch's request outlives its acquisitions, and each later position the page sends is a significant change.
    acquiring: Cell<bool>,
    acquisition_time: Cell<u64>,
    timeout_timer: Cell<Option<u64>>,
}

static POSITION_REQUEST_CLASS: EmbedderClass<PositionRequest> = EmbedderClass::new("GeolocationPositionRequest");

impl EmbedderValue for PositionRequest {
    const CLASS: &'static EmbedderClass<Self> = &POSITION_REQUEST_CLASS;
}

type PositionRequestRef = Gc<EmbedderCell<PositionRequest>>;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct VisibilityWait {
    geolocation: Ref<Geolocation>,
    arguments: PositionRequestArguments,
    observer: GcRefCell<Option<CppCell>>,
}

static VISIBILITY_WAIT_CLASS: EmbedderClass<VisibilityWait> = EmbedderClass::new("GeolocationVisibilityWait");

impl EmbedderValue for VisibilityWait {
    const CLASS: &'static EmbedderClass<Self> = &VISIBILITY_WAIT_CLASS;
}

type VisibilityWaitRef = Gc<EmbedderCell<VisibilityWait>>;

fn now_milliseconds_since_epoch() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| u64::try_from(duration.as_millis()).unwrap_or(u64::MAX))
}

fn timeout_milliseconds(timeout: u32) -> u64 {
    u64::from(timeout.min(i32::MAX.unsigned_abs()))
}

fn invoke_position_callback(vm: &Vm, callback: &CallbackFunction, position: Ref<GeolocationPosition>) {
    let callback_realm = callback.callback_realm();
    let position = wrap(&CallContext::new(vm, callback_realm), position);
    callback.invoke_and_report(&[Value::from_object(position)]);
}

fn invoke_position_error_callback(vm: &Vm, callback: &CallbackFunction, error: Ref<GeolocationPositionError>) {
    let callback_realm = callback.callback_realm();
    let error = wrap(&CallContext::new(vm, callback_realm), error);
    callback.invoke_and_report(&[Value::from_object(error)]);
}

impl Geolocation {
    fn window_settings_object(&self) -> CppEnvironmentSettingsObject {
        relevant_settings_object(self.window.realm().global_object())
    }

    fn queue_success_callback(&self, vm: &Vm, callback: CallbackFunction, position: Ref<GeolocationPosition>) {
        queue_a_task_without_execution_context(
            vm,
            TaskSource::Geolocation,
            (callback, position),
            |vm, (callback, position)| invoke_position_callback(vm, &callback, position),
        );
    }

    fn next_timeout_timer_token(&self) -> u64 {
        let token = self.next_timeout_timer.get() + 1;
        self.next_timeout_timer.set(token);
        token
    }

    fn add_timeout_timer(&self, vm: &Vm, token: u64, watch_id: Option<u32>, timer: CppTimer) {
        self.timeout_timers.borrow_mut(vm).push((token, watch_id, timer));
    }

    fn take_timeout_timer(&self, vm: &Vm, token: u64) -> Option<CppTimer> {
        let mut timers = self.timeout_timers.borrow_mut(vm);
        let index = timers.iter().position(|(candidate, _, _)| *candidate == token)?;
        Some(timers.remove(index).2)
    }

    fn stop_timeout_timers_of_watch(&self, vm: &Vm, watch_id: u32) {
        self.timeout_timers.borrow_mut(vm).retain(|(_, timer_watch_id, timer)| {
            if *timer_watch_id != Some(watch_id) {
                return true;
            }
            timer.stop();
            false
        });
    }

    // https://w3c.github.io/geolocation/#dom-geolocation-getcurrentposition
    pub(crate) fn get_current_position(
        this: &Ref<Self>,
        cx: &mut CallContext,
        success_callback: CallbackFunction,
        error_callback: Option<CallbackFunction>,
        options: PositionOptions,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. If this's relevant global object's associated Document is not fully active:
        if !this.window.associated_document().is_fully_active() {
            // 1. Call back with error errorCallback and POSITION_UNAVAILABLE.
            this.call_back_with_error(vm, error_callback, GeolocationPositionErrorCode::PositionUnavailable);

            // 2. Terminate this algorithm.
            return Ok(());
        }

        // 2. Request a position passing this, successCallback, errorCallback, and options.
        Self::request_a_position(
            *this,
            vm,
            PositionRequestArguments {
                success_callback,
                error_callback,
                options,
                watch_id: None,
            },
        );
        Ok(())
    }

    // https://w3c.github.io/geolocation/#watchposition-method
    pub(crate) fn watch_position(
        this: &Ref<Self>,
        cx: &mut CallContext,
        success_callback: CallbackFunction,
        error_callback: Option<CallbackFunction>,
        options: PositionOptions,
    ) -> WebIdlResult<i32> {
        let vm = cx.vm();

        // 1. If this's relevant global object's associated Document is not fully active:
        if !this.window.associated_document().is_fully_active() {
            // 1. Call back with error passing errorCallback and POSITION_UNAVAILABLE.
            this.call_back_with_error(vm, error_callback, GeolocationPositionErrorCode::PositionUnavailable);

            // 2. Return 0.
            return Ok(0);
        }

        // 2. Let watchId be an implementation-defined unsigned long that is greater than zero.
        let watch_id = NEXT_WATCH_ID.with(|next_watch_id| {
            let watch_id = next_watch_id.get().wrapping_add(1);
            next_watch_id.set(watch_id);
            watch_id
        });

        // 3. Append watchId to this's [[watchIDs]].
        this.watch_ids.borrow_mut().insert(watch_id);
        this.watch_position_data.borrow_mut(vm).insert(
            watch_id,
            WatchPositionData {
                success_callback: success_callback.clone(),
                error_callback: error_callback.clone(),
                options,
                emulated_position_data_observer_id: None,
                observed_document: None,
            },
        );

        // 4. Request a position passing this, successCallback, errorCallback, options, and watchId.
        Self::request_a_position(
            *this,
            vm,
            PositionRequestArguments {
                success_callback,
                error_callback,
                options,
                watch_id: Some(watch_id),
            },
        );

        // 5. Return watchId.
        Ok(watch_id.cast_signed())
    }

    // https://w3c.github.io/geolocation/#clearwatch-method
    pub(crate) fn clear_watch(&self, cx: &mut CallContext, watch_id: i32) -> WebIdlResult<()> {
        let watch_id = watch_id.cast_unsigned();

        // 1. Remove watchId from this's [[watchIDs]].
        self.end_watch(cx.vm(), watch_id);
        Ok(())
    }

    fn end_watch(&self, vm: &Vm, watch_id: u32) {
        self.remove_watch_id(vm, watch_id);
        self.stop_timeout_timers_of_watch(vm, watch_id);
        let request = self.pending_watch_position_requests.borrow_mut(vm).remove(&watch_id);
        if let Some(request) = request {
            request.stop_timeout_timer(vm);
            // SAFETY: The window is live.
            unsafe { web_geolocation_cancel_position_request(self.window.as_ptr(), request.request_id.get()) };
        }
    }

    // https://w3c.github.io/geolocation/#dfn-get-emulated-position-data
    fn get_emulated_position_data(&self) -> EmulatedPositionData {
        // 1. Let navigable be geolocation's relevant global object's associated Document's node navigable.
        // 2. If navigable is null, return null.
        // 3. Return navigable's top-level traversable's emulated position data.
        // NB: The page holds the top-level traversable's, which another process may host.
        let mut data = EmulatedPositionDataAbi {
            kind: 0,
            coordinates: core::ptr::null_mut(),
            error_code: 0,
        };
        // SAFETY: The window is live, and the data outlives the call.
        unsafe { web_geolocation_emulated_position_data(self.window.as_ptr(), &mut data) };
        match data.kind {
            0 => EmulatedPositionData::None,
            // SAFETY: LibWeb lends the live cell of a GeolocationCoordinates, which the page keeps alive.
            1 => EmulatedPositionData::Coordinates(unsafe { cell_from_abi(data.coordinates) }),
            2 => EmulatedPositionData::Error(GeolocationPositionErrorCode::from_u16(data.error_code)),
            _ => unreachable!("LibWeb lends emulated position data"),
        }
    }

    fn remove_watch_id(&self, vm: &Vm, watch_id: u32) {
        self.watch_ids.borrow_mut().remove(&watch_id);
        self.unregister_watch_position_observer(vm, watch_id);
        self.watch_position_data.borrow_mut(vm).remove(&watch_id);

        let visibility_waits = MarkedVec::new(vm);
        for wait in self.visibility_waits.borrow().iter() {
            if wait.arguments.watch_id == Some(watch_id) {
                visibility_waits.push(*wait);
            }
        }
        while let Some(wait) = visibility_waits.pop() {
            VisibilityWait::stop_observing(wait, vm);
        }
    }

    fn unregister_watch_position_observer(&self, vm: &Vm, watch_id: u32) {
        let observer_id = {
            let mut watch_position_data = self.watch_position_data.borrow_mut(vm);
            let Some(watch_position_data) = watch_position_data.get_mut(&watch_id) else {
                return;
            };
            let Some(observer_id) = watch_position_data.emulated_position_data_observer_id.take() else {
                return;
            };
            observer_id
        };

        // SAFETY: The window is live.
        unsafe { web_geolocation_unregister_emulated_position_data_observer(self.window.as_ptr(), observer_id) };
    }

    // https://w3c.github.io/geolocation/#dfn-request-a-position
    fn request_a_position(this: Ref<Self>, vm: &Vm, arguments: PositionRequestArguments) {
        // 1. Let watchIDs be geolocation's [[watchIDs]].

        // 2. Let document be the geolocation's relevant global object's associated Document.
        let document = this.window.associated_document();

        // 3. If document is not allowed to use the "geolocation" feature:
        if !document.is_allowed_to_use_feature(PolicyControlledFeature::Geolocation) {
            // 1. If watchId was passed, remove watchId from watchIDs.
            if let Some(watch_id) = arguments.watch_id {
                this.remove_watch_id(vm, watch_id);
            }

            // 2. Call back with error passing errorCallback and PERMISSION_DENIED.
            this.call_back_with_error(
                vm,
                arguments.error_callback,
                GeolocationPositionErrorCode::PermissionDenied,
            );

            // 3. Terminate this algorithm.
            return;
        }

        // 4. If geolocation's environment settings object is a non-secure context:
        if !this.window_settings_object().is_secure_context() {
            // 1. If watchId was passed, remove watchId from watchIDs.
            if let Some(watch_id) = arguments.watch_id {
                this.remove_watch_id(vm, watch_id);
            }

            // 2. Call back with error passing errorCallback and PERMISSION_DENIED.
            this.call_back_with_error(
                vm,
                arguments.error_callback,
                GeolocationPositionErrorCode::PermissionDenied,
            );

            // 3. Terminate this algorithm.
            return;
        }

        // 5. If document's visibility state is "hidden", wait for the following page visibility change steps to run:
        Self::run_in_parallel_when_document_is_visible(this, vm, &document, arguments);
    }

    fn request_a_position_when_document_is_visible(this: Ref<Self>, vm: &Vm, arguments: PositionRequestArguments) {
        // 1. Assert: document's visibility state is "visible".
        // 2. Continue to the next steps below.
        // AD-HOC: This is implemented by run_in_parallel_when_document_is_visible().

        // 6. Let descriptor be a new PermissionDescriptor whose name is "geolocation".
        let descriptor = PermissionName::Geolocation;

        // 7. In parallel:
        // NB: The round trip to the UI process that asks the user for permission stands in for the parallel steps: the
        //     request goes out as an async IPC, and the rest of the steps run on the main thread once it answers.
        {
            // 1. Set permission to request permission to use descriptor.
            let settings = this.window_settings_object();
            request_permission_to_use(
                vm,
                descriptor,
                &settings,
                (this, arguments),
                |vm, (this, arguments), permission| {
                    // 2. If permission is "denied", then:
                    if permission == PermissionState::Denied {
                        // 1. If watchId was passed, remove watchId from watchIDs.
                        if let Some(watch_id) = arguments.watch_id {
                            this.remove_watch_id(vm, watch_id);
                        }

                        // 2. Call back with error passing errorCallback and PERMISSION_DENIED.
                        this.call_back_with_error(
                            vm,
                            arguments.error_callback,
                            GeolocationPositionErrorCode::PermissionDenied,
                        );

                        // 3. Terminate this algorithm.
                        return;
                    }

                    // FIXME: 3. Wait to acquire a position passing successCallback, errorCallback, options, and watchId.
                    Self::acquire_a_position(this, vm, arguments.clone(), None);

                    // 4. If watchId was not passed, terminate this algorithm.
                    let Some(watch_id) = arguments.watch_id else {
                        return;
                    };

                    if !this.watch_ids.borrow().contains(&watch_id) {
                        return;
                    }

                    // 5. While watchIDs contains watchId:
                    // AD-HOC: The spec expresses this as a loop; register callbacks for significant position changes and
                    // re-enter acquire a position from those callbacks.
                    {
                        let watch_position_data = this.watch_position_data.borrow();
                        match watch_position_data.get(&watch_id) {
                            Some(data) if data.emulated_position_data_observer_id.is_none() => {}
                            _ => return,
                        }
                    }

                    let weak_document = this.window.associated_document().downgrade(vm);
                    // SAFETY: The window and the cell are live.
                    let observer_id = unsafe {
                        web_geolocation_register_emulated_position_data_observer(
                            this.window.as_ptr(),
                            cell_into_abi(this),
                            watch_id,
                        )
                    };
                    if let Some(data) = this.watch_position_data.borrow_mut(vm).get_mut(&watch_id) {
                        data.emulated_position_data_observer_id = Some(observer_id);
                        data.observed_document = Some(weak_document);
                    }
                },
            );
        }
    }

    // https://w3c.github.io/geolocation/#dfn-request-a-position
    // 7.5. While watchIDs contains watchId:
    //      1. Wait for a significant change of geographic position. What constitutes a significant change of
    //         geographic position is left to the implementation. User agents MAY impose a rate limit on how frequently
    //         position changes are reported. User agents MUST consider invoking set emulated position data as a
    //         significant change.
    fn emulated_position_data_changed(this: Ref<Self>, vm: &Vm, watch_id: u32) {
        if !this.watch_ids.borrow().contains(&watch_id) {
            return;
        }

        let arguments = {
            let watch_position_data = this.watch_position_data.borrow();
            let Some(data) = watch_position_data.get(&watch_id) else {
                return;
            };

            //      2. If document is not fully active or visibility state is not "visible", go back to the previous step
            //         and again wait for a significant change of geographic position.
            let document = data.observed_document.as_ref().and_then(WeakCppDocument::get);
            if !document.is_some_and(|document| {
                document.is_fully_active() && document.visibility_state() == VisibilityState::Visible
            }) {
                return;
            }

            PositionRequestArguments {
                success_callback: data.success_callback.clone(),
                error_callback: data.error_callback.clone(),
                options: data.options,
                watch_id: Some(watch_id),
            }
        };

        let request = this.pending_watch_position_requests.borrow().get(&watch_id).copied();
        if let Some(request) = request {
            request.acquiring.set(false);
            request.stop_timeout_timer(vm);
        }

        //      3. Wait to acquire a position passing successCallback, errorCallback, options, and watchId.
        Self::acquire_a_position(this, vm, arguments, None);
    }

    // https://w3c.github.io/geolocation/#dfn-acquire-a-position
    fn acquire_a_position(
        this: Ref<Self>,
        vm: &Vm,
        arguments: PositionRequestArguments,
        acquired: Option<&GeolocationPositionResultAbi>,
    ) {
        // 1. If watchId was passed and this's [[watchIDs]] does not contain watchId, terminate this algorithm.
        if let Some(watch_id) = arguments.watch_id
            && !this.watch_ids.borrow().contains(&watch_id)
        {
            return;
        }

        // 2. Let acquisitionTime be a new EpochTimeStamp that represents now.
        let acquisition_time = now_milliseconds_since_epoch();

        // 3. Let timeoutTime be the sum of acquisitionTime and options.timeout.
        let _timeout_time = acquisition_time.wrapping_add(u64::from(arguments.options.timeout));

        // 4. Let cachedPosition be this's [[cachedPosition]].
        let cached_position = *this.cached_position.borrow();

        // 5. Create an implementation-specific timeout task that elapses at timeoutTime, during which it tries to acquire
        //    the device's position by running the following steps:
        {
            // 1. Let permission be get the current permission state of "geolocation".
            // NB: This can run outside any script (when the user's answer arrives, e.g.), so the settings object is passed.
            let permission = permission_state(PermissionName::Geolocation, &this.window_settings_object());

            // 2. If permission is "denied":
            if permission == PermissionState::Denied {
                // 1. Stop timeout.
                // NB: No timeout is running yet because we start the timer immediately before requesting native position
                //     data below.

                // 2. Do the user or system denied permission failure case step.
                this.call_back_with_error(
                    vm,
                    arguments.error_callback,
                    GeolocationPositionErrorCode::PermissionDenied,
                );

                // AD-HOC: A watch ends once its permission is denied. Reporting the denial again on each significant
                //         change would tell the page when the user moves.
                if let Some(watch_id) = arguments.watch_id {
                    this.end_watch(vm, watch_id);
                }
                return;
            }

            // 3. If permission is "granted":
            if permission == PermissionState::Granted {
                // 1. Check if an emulated position should be used by running the following steps:
                {
                    // 1. Let emulatedPositionData be get emulated position data passing this.
                    let emulated_position_data = this.get_emulated_position_data();

                    // 2. If emulatedPositionData is not null:
                    match emulated_position_data {
                        EmulatedPositionData::None => {}
                        // 1. If emulatedPositionData is a GeolocationPositionError:
                        EmulatedPositionData::Error(code) => {
                            // 1. Call back with error passing errorCallback and emulatedPositionData.
                            // FIXME: We pass along the code instead of the entire error object. Spec issue:
                            //        https://github.com/w3c/geolocation/issues/186
                            this.call_back_with_error(vm, arguments.error_callback, code);

                            // 2. Terminate this algorithm.
                            return;
                        }
                        EmulatedPositionData::Coordinates(coordinates) => {
                            // 2. Let position be a new GeolocationPosition passing emulatedPositionData, acquisitionTime and
                            //    options.enableHighAccuracy.
                            let position = GeolocationPosition::create(
                                vm,
                                coordinates,
                                acquisition_time,
                                arguments.options.enable_high_accuracy,
                            );

                            // 3. Queue a task on the geolocation task source with a step that invokes successCallback with
                            //    « position » and "report".
                            this.queue_success_callback(vm, arguments.success_callback, position);

                            // 4. Terminate this algorithm.
                            return;
                        }
                    }
                }

                // 2. Let position be null.
                // NB: position is created inside the async callback in step 6 below.

                // 3. If cachedPosition is not null, and options.maximumAge is greater than 0:
                // AD-HOC: A position the system just sent a watch is newer than cachedPosition, so it's reported instead.
                if let Some(cached_position) = cached_position
                    && arguments.options.maximum_age > 0
                    && acquired.is_none()
                {
                    // 1. Let cacheTime be acquisitionTime minus the value of the options.maximumAge member.
                    let cache_time = acquisition_time.wrapping_sub(u64::from(arguments.options.maximum_age));

                    // 2. If cachedPosition's timestamp's value is greater than cacheTime, and
                    //    cachedPosition.[[isHighAccuracy]] equals options.enableHighAccuracy:
                    if cached_position.timestamp_value() > cache_time
                        && cached_position.is_high_accuracy() == arguments.options.enable_high_accuracy
                    {
                        // 1. Queue a task on the geolocation task source with a step that invokes successCallback with
                        //    « cachedPosition » and "report".
                        this.queue_success_callback(vm, arguments.success_callback, cached_position);

                        // 2. Terminate this algorithm.
                        return;
                    }
                }

                // 4. Otherwise, if position is not cachedPosition, try to acquire position data from the underlying system,
                //    optionally taking into consideration the value of options.enableHighAccuracy during acquisition.
                Self::acquire_position_from_page(this, vm, arguments, acquisition_time, acquired);
                return;
            }

            // NB: While the permission is "prompt" (a one-time grant just ended, e.g.), neither step above applies, and the
            //     timeout task elapses without a position.
            Self::call_back_with_timeout_error(
                this,
                vm,
                arguments.error_callback,
                arguments.options.timeout,
                arguments.watch_id,
            );
        }
    }

    // A watch cleared, or a document left, before the timeout elapses hears nothing of it, as with a timeout that elapses
    // while a position is being acquired.
    fn call_back_with_timeout_error(
        this: Ref<Self>,
        vm: &Vm,
        error_callback: Option<CallbackFunction>,
        timeout: u32,
        watch_id: Option<u32>,
    ) {
        // NB: The acquisition replaces any earlier one of the watch that's still waiting, and so does its timeout.
        if let Some(watch_id) = watch_id {
            this.stop_timeout_timers_of_watch(vm, watch_id);
        }

        let weak_document = this.window.associated_document().downgrade(vm);
        let token = this.next_timeout_timer_token();
        let timer = CppTimer::create_single_shot(
            vm,
            timeout_milliseconds(timeout),
            (this, token, error_callback, watch_id, weak_document),
            |vm, (this, token, error_callback, watch_id, weak_document)| {
                this.take_timeout_timer(vm, token);
                if let Some(watch_id) = watch_id
                    && !this.watch_ids.borrow().contains(&watch_id)
                {
                    return;
                }
                if !weak_document.get().is_some_and(|document| document.is_fully_active()) {
                    return;
                }
                this.call_back_with_error(vm, error_callback, GeolocationPositionErrorCode::Timeout);
            },
        );
        this.add_timeout_timer(vm, token, watch_id, timer);
    }

    fn acquire_position_from_page(
        this: Ref<Self>,
        vm: &Vm,
        arguments: PositionRequestArguments,
        acquisition_time: u64,
        acquired: Option<&GeolocationPositionResultAbi>,
    ) {
        let watch_id = arguments.watch_id;
        let existing_request =
            watch_id.and_then(|watch_id| this.pending_watch_position_requests.borrow().get(&watch_id).copied());
        if let Some(request) = existing_request {
            PositionRequest::start_acquisition(request, vm, acquisition_time, acquired.is_none());
            if let Some(result) = acquired {
                PositionRequest::receive_position(request, vm, result);
            }
            return;
        }

        let document = this.window.associated_document();
        let request = POSITION_REQUEST_CLASS.allocate(vm, |_| PositionRequest {
            geolocation: this,
            arguments,
            weak_document: document.downgrade(vm),
            request_id: Cell::new(0),
            completed: Cell::new(false),
            acquiring: Cell::new(false),
            acquisition_time: Cell::new(0),
            timeout_timer: Cell::new(None),
        });

        // SAFETY: The window and the cell are live.
        let request_id = unsafe {
            web_geolocation_request_position(this.window.as_ptr(), request.as_ptr().cast(), watch_id.is_some())
        };
        request.request_id.set(request_id);
        if let Some(watch_id) = watch_id {
            this.pending_watch_position_requests
                .borrow_mut(vm)
                .insert(watch_id, request);
        }
        PositionRequest::start_acquisition(request, vm, acquisition_time, true);
    }

    // https://w3c.github.io/geolocation/#dfn-call-back-with-error
    fn call_back_with_error(&self, vm: &Vm, callback: Option<CallbackFunction>, code: GeolocationPositionErrorCode) {
        // 1. If callback is null, return.
        let Some(callback) = callback else {
            return;
        };

        // 2. Let error be a newly created GeolocationPositionError instance whose code attribute is initialized to code.
        let error = GeolocationPositionError::create(vm, code);

        // 3. Queue a task on the geolocation task source with a step that invokes callback with « error » and "report".
        queue_a_task_without_execution_context(
            vm,
            TaskSource::Geolocation,
            (callback, error),
            |vm, (callback, error)| invoke_position_error_callback(vm, &callback, error),
        );
    }

    fn run_in_parallel_when_document_is_visible(
        this: Ref<Self>,
        vm: &Vm,
        document: &CppDocument,
        arguments: PositionRequestArguments,
    ) {
        // Run callback in parallel if the document is already visible.
        if document.visibility_state() == VisibilityState::Visible {
            deferred_invoke(vm, (this, arguments), |vm, (this, arguments)| {
                with_temporary_execution_context(this.window.realm(), || {
                    Self::request_a_position_when_document_is_visible(this, vm, arguments);
                });
            });
            return;
        }

        let wait = VISIBILITY_WAIT_CLASS.allocate(vm, |_| VisibilityWait {
            geolocation: this,
            arguments,
            observer: GcRefCell::new(None),
        });
        // SAFETY: The Document and the cell are live, and LibWeb returns a live DocumentObserver.
        let observer = unsafe {
            CppCell::from_raw(web_geolocation_observe_document_visibility(
                document.as_ptr(),
                wait.as_ptr().cast(),
            ))
        };
        *wait.observer.borrow_mut(vm) = Some(observer);
        this.visibility_waits.borrow_mut(vm).push(wait);
    }
}

impl PositionRequest {
    fn start_acquisition(request: PositionRequestRef, vm: &Vm, acquisition_time: u64, start_timeout: bool) {
        request.stop_timeout_timer(vm);
        request.acquiring.set(true);
        request.acquisition_time.set(acquisition_time);
        if !start_timeout {
            return;
        }

        let geolocation = request.geolocation;
        let token = geolocation.next_timeout_timer_token();
        request.timeout_timer.set(Some(token));
        let timeout = timeout_milliseconds(request.arguments.options.timeout);
        let timer = CppTimer::create_single_shot(vm, timeout, request, |vm, request| {
            PositionRequest::timeout_elapsed(request, vm);
        });
        geolocation.add_timeout_timer(vm, token, None, timer);
    }

    fn stop_timeout_timer(&self, vm: &Vm) {
        if let Some(token) = self.timeout_timer.take()
            && let Some(timer) = self.geolocation.take_timeout_timer(vm, token)
        {
            timer.stop();
        }
    }

    fn receive_position(request: PositionRequestRef, vm: &Vm, result: &GeolocationPositionResultAbi) {
        let this = request.geolocation;
        let arguments = &request.arguments;

        if request.completed.get() {
            return;
        }

        if let Some(watch_id) = arguments.watch_id
            && !this.watch_ids.borrow().contains(&watch_id)
        {
            // NB: The position response arrived after the watch was cleared; stop the timeout before ignoring it.
            request.stop_timeout_timer(vm);
            return;
        }
        if arguments.watch_id.is_none() {
            request.completed.set(true);
        }

        let Some(document) = request.weak_document.get() else {
            request.stop_timeout_timer(vm);
            return;
        };

        // NB: The page drops a watch's request when the permission is denied.
        // AD-HOC: A watch ends once its permission is denied, as in acquire a position.
        let is_denial = result.is_error
            && GeolocationPositionErrorCode::from_u16(result.error_code)
                == GeolocationPositionErrorCode::PermissionDenied;
        if is_denial && let Some(watch_id) = arguments.watch_id {
            request.completed.set(true);
            this.pending_watch_position_requests.borrow_mut(vm).remove(&watch_id);
            this.end_watch(vm, watch_id);
        }

        if !request.acquiring.get() && !is_denial {
            // https://w3c.github.io/geolocation/#dfn-request-a-position
            // 7.5. While watchIDs contains watchId:
            //      2. If document is not fully active or visibility state is not "visible", go back to the previous step
            //         and again wait for a significant change of geographic position.
            if !document.is_fully_active() || document.visibility_state() != VisibilityState::Visible {
                return;
            }

            //      3. Wait to acquire a position passing successCallback, errorCallback, options, and watchId.
            Geolocation::acquire_a_position(this, vm, arguments.clone(), Some(result));
            return;
        }

        request.acquiring.set(false);
        if !document.is_fully_active() {
            // NB: The position response is no longer observable; stop the timeout before ignoring it.
            request.stop_timeout_timer(vm);
            return;
        }

        // 5. If the timeout elapses during acquisition, or acquiring the device's position results in failure:
        if result.is_error {
            let error_code = GeolocationPositionErrorCode::from_u16(result.error_code);

            // 1. Stop the timeout.
            request.stop_timeout_timer(vm);

            // 2. Go to dealing with failures.
            this.call_back_with_error(vm, arguments.error_callback.clone(), error_code);

            // 3. Terminate this algorithm.
            return;
        }

        // 6. If acquiring the position data from the system succeeds:
        // 6.1. Let positionData be a map with the following name/value pairs based on the acquired position data:
        //      * latitude:
        //          A double that represents the latitude coordinates on the Earth's surface in degrees, using the
        //          [WGS84] coordinate system. Latitude measures how far north or south a point is from the Equator.
        //      * longitude:
        //          A double that represents the longitude coordinates on the Earth's surface in degrees, using the
        //          [WGS84] coordinate system. Longitude measures how far east or west a point is from the Prime
        //          Meridian.
        //      * altitude:
        //          A double? that represents the altitude in meters above the [WGS84] ellipsoid, or null if not
        //          available. Altitude measures the height above sea level.
        //      * accuracy:
        //          A non-negative double that represents the accuracy value indicating the 95% confidence level in
        //          meters. Accuracy measures how close the measured coordinates are to the true position.
        //      * altitudeAccuracy:
        //          A non-negative double? that represents the altitude accuracy, or null if not available,
        //          indicating the 95% confidence level in meters. Altitude accuracy measures how close the measured
        //          altitude is to the true altitude.
        //      * speed:
        //          A non-negative double? that represents the speed in meters per second, or null if not available.
        //          Speed measures how fast the device is moving.
        //      * heading:
        //          A double? that represents the heading in degrees, or null if not available or the device is
        //          stationary. Heading measures the direction in which the device is moving relative to true north.
        let position_data = GeolocationCoordinates::create(vm, &result.coordinates);

        // 6.2. Set position to a new GeolocationPosition passing positionData, acquisitionTime and
        //      options.enableHighAccuracy.
        let position = GeolocationPosition::create(
            vm,
            position_data,
            request.acquisition_time.get(),
            arguments.options.enable_high_accuracy,
        );

        // 6.3. Set this's [[cachedPosition]] to position.
        *this.cached_position.borrow_mut(vm) = Some(position);

        // 7. Stop the timeout.
        request.stop_timeout_timer(vm);

        // 8. Queue a task on the geolocation task source with a step that invokes successCallback with « position »
        //    and "report".
        this.queue_success_callback(vm, arguments.success_callback.clone(), position);
    }

    fn timeout_elapsed(request: PositionRequestRef, vm: &Vm) {
        let this = request.geolocation;
        if request.completed.get() || !request.acquiring.get() {
            return;
        }
        request.acquiring.set(false);
        request.stop_timeout_timer(vm);
        match request.arguments.watch_id {
            Some(watch_id) => {
                if !this.watch_ids.borrow().contains(&watch_id) {
                    return;
                }
            }
            None => {
                request.completed.set(true);
                // SAFETY: The window is live.
                unsafe { web_geolocation_cancel_position_request(this.window.as_ptr(), request.request_id.get()) };
            }
        }
        if !request
            .weak_document
            .get()
            .is_some_and(|document| document.is_fully_active())
        {
            return;
        }
        this.call_back_with_error(
            vm,
            request.arguments.error_callback.clone(),
            GeolocationPositionErrorCode::Timeout,
        );
    }
}

impl VisibilityWait {
    fn stop_observing(wait: VisibilityWaitRef, vm: &Vm) {
        if let Some(observer) = wait.observer.borrow_mut(vm).take() {
            // SAFETY: The DocumentObserver is live.
            unsafe { web_geolocation_stop_observing_document_visibility(observer.as_ptr()) };
        }
        wait.geolocation
            .visibility_waits
            .borrow_mut(vm)
            .retain(|candidate| *candidate != wait);
    }

    fn document_visibility_changed(wait: VisibilityWaitRef, vm: &Vm, is_visible: bool) {
        if !is_visible {
            return;
        }
        Self::stop_observing(wait, vm);
        let geolocation = wait.geolocation;
        let arguments = wait.arguments.clone();
        with_temporary_execution_context(geolocation.window.realm(), || {
            Geolocation::request_a_position_when_document_is_visible(geolocation, vm, arguments);
        });
    }
}

/// A new Geolocation of the window.
///
/// # Safety
///
/// `vm` must be the live VM, and `window` a live Window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_create_geolocation(vm: *mut c_void, window: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live Window.
    let (vm, window) = unsafe { (&*vm.cast::<Vm>(), CppWindow::from_raw(window)) };
    cell_into_abi(allocate(vm, |_| Geolocation {
        window,
        watch_ids: RefCell::new(HashSet::new()),
        watch_position_data: GcRefCell::new(HashMap::new()),
        pending_watch_position_requests: GcRefCell::new(HashMap::new()),
        cached_position: GcRefCell::new(None),
        timeout_timers: GcRefCell::new(Vec::new()),
        visibility_waits: GcRefCell::new(Vec::new()),
        next_timeout_timer: Cell::new(0),
    }))
}

/// The page's answer to a request for a position.
///
/// # Safety
///
/// `vm` must be the live VM, `request` the live cell that the request was made with, and `result` lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_receive_position(
    vm: *mut c_void,
    request: *mut c_void,
    result: &GeolocationPositionResultAbi,
) {
    // SAFETY: The caller passes the live VM and the live cell of a request.
    let (vm, request) = unsafe {
        (
            &*vm.cast::<Vm>(),
            POSITION_REQUEST_CLASS
                .downcast(NonNull::new(request).expect("LibWeb passes the request"))
                .expect("LibWeb passes a GeolocationPositionRequest"),
        )
    };
    PositionRequest::receive_position(request, vm, result);
}

/// The page's emulated position data changed, which a watch observes.
///
/// # Safety
///
/// `vm` must be the live VM, and `geolocation` the live cell of a Geolocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_emulated_position_data_changed(
    vm: *mut c_void,
    geolocation: *mut c_void,
    watch_id: u32,
) {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, geolocation) = unsafe { (&*vm.cast::<Vm>(), cell_from_abi::<Geolocation>(geolocation)) };
    Geolocation::emulated_position_data_changed(geolocation, vm, watch_id);
}

/// The visibility state of the document that a request a position waits for changed.
///
/// # Safety
///
/// `vm` must be the live VM, and `wait` the live cell that the observer was made with.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_document_visibility_changed(
    vm: *mut c_void,
    wait: *mut c_void,
    is_visible: bool,
) {
    // SAFETY: The caller passes the live VM and the live cell of a wait.
    let (vm, wait) = unsafe {
        (
            &*vm.cast::<Vm>(),
            VISIBILITY_WAIT_CLASS
                .downcast(NonNull::new(wait).expect("LibWeb passes the wait"))
                .expect("LibWeb passes a GeolocationVisibilityWait"),
        )
    };
    VisibilityWait::document_visibility_changed(wait, vm, is_visible);
}
