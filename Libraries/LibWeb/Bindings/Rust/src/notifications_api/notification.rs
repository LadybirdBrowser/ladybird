/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Notification, https://notifications.spec.whatwg.org/#notifications

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{ConsoleLogLevel, Gc, GcRefCell, Object, Realm, Trace, Utf16View, Value, Vm};
use liburl_rust::origin::Origin;
use liburl_rust::url::ExcludeFragment;
use liburl_rust::{BasicParseOptions, RustUrl, Url, basic_parse};

use crate::bindings::callback::CallbackFunction;
use crate::bindings::conversions::{UsvString, enumeration_value_to_js};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::environment_settings_object::{
    CppEnvironmentSettingsObject, current_settings_object, relevant_settings_object,
};
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::global_object::{CppWindow, is_service_worker_global_scope, window_from_global_object};
use crate::bindings::interface::{
    Ref, Wrappable, allocate, cell_from_abi, cell_into_abi, with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{TaskSource, WebIdlPromise, create_promise, queue_a_global_task, resolve_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::domurl::url_input;
use crate::html::structured_serialize::{structured_deserialize_for_storage, structured_serialize_for_storage};
use crate::notifications_api::notification_permission::get_the_notifications_permission_state;
use crate::permissions::permissions::PermissionName;
use crate::permissions_api::permission_status::PermissionState;
use crate::permissions_api::permissions::request_permission_to_use;
use crate::platform::timer::CppTimer;

unsafe extern "C" {
    fn web_notifications_api_register_notification(window: *mut c_void, notification: *mut c_void) -> u64;
    fn web_notifications_api_unregister_notification(window: *mut c_void, notification_id: u64);
    fn web_notifications_api_fetch_icon(
        notification: *mut c_void,
        settings_object: *mut c_void,
        url: &RustUrl,
    ) -> *mut c_void;
    fn web_notifications_api_stop_icon_fetch(icon: *mut c_void);
    fn web_notifications_api_show_notification(
        window: *mut c_void,
        notification_id: u64,
        data: &NotificationDataAbi,
        icon: *mut c_void,
    );
    fn web_notifications_api_request_close_notification(window: *mut c_void, notification_id: u64);
    fn web_notifications_api_abandon_notification(window: *mut c_void, notification_id: u64);
    fn web_notifications_api_handle_notification_click(window: *mut c_void, notification_id: u64, into_focus: bool);
}

/// The typedef VibratePattern of NotificationsAPI/Notification.idl, whose union (unsigned long or
/// sequence<unsigned long>) other interfaces also name.
pub(crate) enum VibratePattern {
    UnsignedLong(u32),
    Sequence(Vec<u32>),
}

// https://notifications.spec.whatwg.org/#enumdef-notificationpermission
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum NotificationPermission {
    Default,
    Denied,
    Granted,
}

// https://notifications.spec.whatwg.org/#enumdef-notificationdirection
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum NotificationDirection {
    Auto,
    Ltr,
    Rtl,
}

// https://notifications.spec.whatwg.org/#dictdef-notificationaction
pub(crate) struct NotificationAction {
    pub(crate) action: Utf16String,
    pub(crate) title: Utf16String,
    pub(crate) navigate: Option<UsvString>,
    pub(crate) icon: Option<UsvString>,
}

// https://notifications.spec.whatwg.org/#dictdef-notificationoptions
pub(crate) struct NotificationOptions {
    pub(crate) dir: NotificationDirection,
    pub(crate) lang: Utf16String,
    pub(crate) body: Utf16String,
    pub(crate) navigate: Option<UsvString>,
    pub(crate) tag: Utf16String,
    pub(crate) image: Option<UsvString>,
    pub(crate) icon: Option<UsvString>,
    pub(crate) badge: Option<UsvString>,
    pub(crate) vibrate: Option<VibratePattern>,
    pub(crate) timestamp: Option<u64>,
    pub(crate) renotify: bool,
    pub(crate) silent: Option<bool>,
    pub(crate) require_interaction: bool,
    pub(crate) data: Value,
    pub(crate) actions: Vec<NotificationAction>,
}

struct CreateNotificationOptions {
    direction: NotificationDirection,
    language: Utf16String,
    body: Utf16String,
    navigate: Option<UsvString>,
    tag: Utf16String,
    image: Option<UsvString>,
    icon: Option<UsvString>,
    badge: Option<UsvString>,
    // FIXME: Validate and normalize the pattern (create step 15); only its presence matters until then.
    has_vibrate: bool,
    timestamp: Option<u64>,
    renotify: bool,
    silent: Option<bool>,
    require_interaction: bool,
    data: Value,
    actions: Vec<NotificationAction>,
}

// https://notifications.spec.whatwg.org/#action
struct ConceptNotificationAction {
    name: Utf16String,
    title: Utf16String,
    navigation_url: Option<Url>,
    icon_url: Option<Url>,
    // FIXME: icon resource
}

// https://notifications.spec.whatwg.org/#concept-notification
// This is the notification described as "notification" in the spec. Do not confuse it with "notification" as in the IDL which is just the JS wrapper.
// "A notification is an abstract representation of something that happened, such as the delivery of a message."
struct ConceptNotification {
    // FIXME: A notification has an associated service worker registration (null or a service worker registration). It is initially null.
    title: Utf16String,
    direction: NotificationDirection,
    language: Utf16String,
    body: Utf16String,
    navigation_url: Option<Url>,
    tag: Utf16String,
    data: Vec<u8>,
    timestamp: u64,
    #[allow(dead_code, reason = "the spec keeps the origin, which no step reads yet")]
    origin: Origin,
    renotify_preference: bool,
    silent_preference: Option<bool>,
    require_interaction_preference: bool,
    image_url: Option<Url>,
    icon_url: Option<Url>,
    badge_url: Option<Url>,

    // FIXME: add the resources from m_image_url, m_icon_url and m_badge_url

    // FIXME: A notification has an associated vibration pattern (a list). It is initially « ».
    actions: Vec<ConceptNotificationAction>,
}

/// Web::NotificationsAPI::NotificationData.
#[repr(C)]
struct NotificationDataAbi {
    title: usize,
    body: usize,
    tag: usize,
    language: usize,
    direction: u8,
    has_silent: bool,
    silent: bool,
    require_interaction: bool,
    renotify: bool,
}

// Where the notification is on its way to the platform. From Pending on it's in the page's list of notifications,
// which keeps it alive; Closed means it has left the list for good.
#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Idle,
    Pending,
    Showing,
    Closing,
    Closed,
}

// https://notifications.spec.whatwg.org/#notifications
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Notification {
    event_target: CppEventTarget,

    #[gc(untraced)]
    notification: ConceptNotification,

    // The global the constructor ran in: Where the events fire, and whose window the notification belongs to.
    realm: Gc<Realm>,
    window: Option<CppWindow>,

    #[gc(untraced)]
    state: Cell<State>,
    #[gc(untraced)]
    id: Cell<u64>,

    // https://notifications.spec.whatwg.org/#icon-resource
    icon: GcRefCell<Option<CppCell>>,
    // Bounds the wait for the icon: Past it, the notification shows without one.
    fetch_timeout: GcRefCell<Option<CppTimer>>,
}

impl Wrappable for Notification {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for Notification {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

// The longest a notification waits for its icon before it shows without one.
const ICON_FETCH_TIMEOUT_MS: u64 = 5_000;

fn parse_url(url: &UsvString, base_url: &Url) -> Option<Url> {
    basic_parse(url_input(url.units()), BasicParseOptions::new().base_url(base_url))
}

fn serialize_url_for_bindings(url: Option<&Url>) -> UsvString {
    url.map_or_else(
        || UsvString::from_ascii(b""),
        |url| UsvString::from_utf16_string(&Utf16String::from_utf8(url.serialize(ExcludeFragment::No))),
    )
}

fn notification_permission_from(permission_state: PermissionState) -> NotificationPermission {
    // https://notifications.spec.whatwg.org/#permissions-integration
    // 2. If permissionState is "prompt", then return "default".
    // 3. Return permissionState.
    match permission_state {
        PermissionState::Prompt => NotificationPermission::Default,
        PermissionState::Granted => NotificationPermission::Granted,
        PermissionState::Denied => NotificationPermission::Denied,
    }
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct PermissionRequest {
    realm: Gc<Realm>,
    global: Gc<Object>,
    promise: WebIdlPromise,
    deprecated_callback: Option<CallbackFunction>,
    settings: CppEnvironmentSettingsObject,
}

impl Notification {
    // https://notifications.spec.whatwg.org/#dom-notification-notification
    pub(crate) fn construct(
        cx: &mut CallContext,
        title: Utf16String,
        options: NotificationOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let options = CreateNotificationOptions {
            direction: options.dir,
            language: options.lang,
            body: options.body,
            navigate: options.navigate,
            tag: options.tag,
            image: options.image,
            icon: options.icon,
            badge: options.badge,
            has_vibrate: options.vibrate.is_some(),
            timestamp: options.timestamp,
            renotify: options.renotify,
            silent: options.silent,
            require_interaction: options.require_interaction,
            data: options.data,
            actions: options.actions,
        };
        Self::create_with_global_scope(cx, title, options)
    }

    // https://notifications.spec.whatwg.org/#constructors
    fn create_with_global_scope(
        cx: &mut CallContext,
        title: Utf16String,
        options: CreateNotificationOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();
        let realm = cx.realm();
        let relevant_global_object = realm.global_object();
        let relevant_settings_object = relevant_settings_object(relevant_global_object);

        // 1. If this’s relevant global object is a ServiceWorkerGlobalScope object, then throw a TypeError.
        if is_service_worker_global_scope(relevant_global_object) {
            return Err(Exception::type_error(
                "This’s relevant global object is a ServiceWorkerGlobalScope object",
            ));
        }

        // 2. If options["actions"] is not empty, then throw a TypeError.
        if !options.actions.is_empty() {
            return Err(Exception::type_error("Options `action` is not empty"));
        }

        // 3. Let notification be the result of creating a notification with a settings object given title, options, and this’s relevant settings object.
        let notification =
            Self::create_a_notification_with_a_settings_object(cx, title, options, &relevant_settings_object)?;

        // 4. Associate this with notification.
        let this_notification = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            notification,
            realm,
            window: window_from_global_object(relevant_global_object),
            state: Cell::new(State::Idle),
            id: Cell::new(0),
            icon: GcRefCell::new(None),
            fetch_timeout: GcRefCell::new(None),
        });
        this_notification.event_target.set_owner(this_notification);

        // 5. Run these steps in parallel:
        // AD-HOC: The steps run from a task on the relevant global instead of in parallel: Showing crosses to the UI
        //         process asynchronously anyway, and a task keeps the permission read and the event firing on the main
        //         thread, after the constructor's caller has had its turn to attach event handlers.
        queue_a_global_task(
            vm,
            relevant_global_object,
            TaskSource::Unspecified,
            this_notification,
            |vm, this_notification| Self::run_steps_in_parallel_after_construction(this_notification, vm),
        );

        Ok(this_notification)
    }

    // https://notifications.spec.whatwg.org/#create-a-notification-with-a-settings-object
    fn create_a_notification_with_a_settings_object(
        cx: &mut CallContext,
        title: Utf16String,
        options: CreateNotificationOptions,
        settings: &CppEnvironmentSettingsObject,
    ) -> WebIdlResult<ConceptNotification> {
        // 1. Let origin be settings’s origin.
        let origin = settings.origin();

        // 2. Let baseURL be settings’s API base URL.
        let base_url = settings.api_base_url();

        // 3. Let fallbackTimestamp be the number of milliseconds from the Unix epoch to settings’s current wall time,
        // rounded to the nearest integer.
        #[allow(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "the conversion saturates, as AK::round_to() does"
        )]
        let fallback_timestamp = settings.current_wall_time().round_ties_even() as u64;

        // 4. Return the result of creating a notification given title, options, origin, baseURL, and fallbackTimestamp.
        Self::create_a_notification(cx, title, options, origin, &base_url, fallback_timestamp)
    }

    // https://notifications.spec.whatwg.org/#create-a-notification
    fn create_a_notification(
        cx: &mut CallContext,
        title: Utf16String,
        options: CreateNotificationOptions,
        origin: Origin,
        base_url: &Url,
        fallback_timestamp: u64,
    ) -> WebIdlResult<ConceptNotification> {
        // 1. Let notification be a new notification.

        // 2. If options["silent"] is true and options["vibrate"] exists, then throw a TypeError.
        if options.silent == Some(true) && options.has_vibrate {
            return Err(Exception::type_error(
                "options[\"vibrate\"] cannot be set when options[\"silent\"] is true.",
            ));
        }

        // 3. If options["renotify"] is true and options["tag"] is the empty string, then throw a TypeError.
        if options.renotify && options.tag.is_empty() {
            return Err(Exception::type_error(
                "options[\"tag\"] cannot be the empty string when options[\"renotify\"] is set to true.",
            ));
        }

        let mut notification = ConceptNotification {
            // 4. Set notification’s data to StructuredSerializeForStorage(options["data"]).
            data: structured_serialize_for_storage(cx, options.data)?,

            // 5. Set notification’s title to title.
            title,

            // 6. Set notification’s direction to options["dir"].
            direction: options.direction,

            // 7. Set notification’s language to options["lang"].
            language: options.language,

            // 8. Set notification’s origin to origin.
            origin,

            // 9. Set notification’s body to options["body"].
            body: options.body,

            // 10. If options["navigate"] exists, then parse it using baseURL, and if that does not return failure,
            // set notification’s navigation URL to the return value. (Otherwise notification’s navigation URL remains null.)
            navigation_url: options
                .navigate
                .as_ref()
                .and_then(|navigate| parse_url(navigate, base_url)),

            // 11. Set notification’s tag to options["tag"].
            tag: options.tag,

            // 12. If options["image"] exists, then parse it using baseURL, and if that does not return failure,
            // set notification’s image URL to the return value. (Otherwise notification’s image URL is not set.)
            image_url: options.image.as_ref().and_then(|image| parse_url(image, base_url)),

            // 13. If options["icon"] exists, then parse it using baseURL, and if that does not return failure,
            // set notification’s icon URL to the return value. (Otherwise notification’s icon URL is not set.)
            icon_url: options.icon.as_ref().and_then(|icon| parse_url(icon, base_url)),

            // 14. If options["badge"] exists, then parse it using baseURL, and if that does not return failure,
            // set notification’s badge URL to the return value. (Otherwise notification’s badge URL is not set.)
            badge_url: options.badge.as_ref().and_then(|badge| parse_url(badge, base_url)),

            // FIXME: 15. If options["vibrate"] exists, then validate and normalize it and
            // set notification’s vibration pattern to the return value.

            // 16. If options["timestamp"] exists, then set notification’s timestamp to the value.
            // Otherwise, set notification’s timestamp to fallbackTimestamp.
            timestamp: options.timestamp.unwrap_or(fallback_timestamp),

            // 17. Set notification’s renotify preference to options["renotify"].
            renotify_preference: options.renotify,

            // 18. Set notification’s silent preference to options["silent"].
            silent_preference: options.silent,

            // 19. Set notification’s require interaction preference to options["requireInteraction"].
            require_interaction_preference: options.require_interaction,

            // 20. Set notification’s actions to « ».
            actions: Vec::new(),
        };

        // 21. For each entry in options["actions"], up to the maximum number of actions supported (skip any excess entries):
        for entry in options.actions {
            if notification.actions.len() >= Self::MAX_ACTIONS as usize {
                break;
            }

            // 1. Let action be a new notification action.
            let action = ConceptNotificationAction {
                // 2. Set action’s name to entry["action"].
                name: entry.action,

                // 3. Set action’s title to entry["title"].
                title: entry.title,

                // 4. If entry["navigate"] exists, then parse it using baseURL, and if that does not return failure,
                // set action’s navigation URL to the return value. (Otherwise action’s navigation URL remains null.)
                navigation_url: entry
                    .navigate
                    .as_ref()
                    .and_then(|navigate| parse_url(navigate, base_url)),

                // 5. If entry["icon"] exists, then parse it using baseURL, and if that does not return failure,
                // set action’s icon URL to the return value. (Otherwise action’s icon URL remains null.)
                icon_url: entry.icon.as_ref().and_then(|icon| parse_url(icon, base_url)),
            };

            // 6. Append action to notification’s actions.
            notification.actions.push(action);
        }

        // 22. Return notification.
        Ok(notification)
    }

    // Step 5 of https://notifications.spec.whatwg.org/#constructors
    fn run_steps_in_parallel_after_construction(this: Ref<Self>, vm: &Vm) {
        let settings = relevant_settings_object(this.realm.global_object());

        // 1. If the result of getting the notifications permission state is not "granted", then queue a task to fire an
        //    event named error on this, and abort these steps.
        if get_the_notifications_permission_state(&settings) != PermissionState::Granted {
            Self::queue_task_to_fire_event(this, vm, "error");
            return;
        }

        // AD-HOC: The spec lets any document or worker show a notification. We refuse two cases until they're supported:
        //         A worker has no page to route the notification's events through, and a document whose origin differs
        //         from its top-level origin would have its notification attributed to the top-level site. Both get an
        //         error event, as if permission had been denied.
        if this.window.is_none() {
            Self::queue_task_to_fire_event(this, vm, "error");
            return;
        }
        if !settings
            .top_level_origin()
            .is_some_and(|top_level_origin| top_level_origin.is_same_origin(&settings.origin()))
        {
            Self::queue_task_to_fire_event(this, vm, "error");
            return;
        }

        // 2. Run the notification show steps for notification.
        Self::show_steps(this, vm);
    }

    fn window(&self) -> &CppWindow {
        self.window.as_ref().expect("a notification that is shown has a window")
    }

    // https://notifications.spec.whatwg.org/#notification-show-steps
    fn show_steps(this: Ref<Self>, vm: &Vm) {
        // The notification joins the page's list now, rather than once it's displayed: The list is what keeps it alive
        // while its icon is on its way, and what lets the page withdraw it if the document goes away before then.
        // SAFETY: The window and the cell are live.
        this.id
            .set(unsafe { web_notifications_api_register_notification(this.window().as_ptr(), cell_into_abi(this)) });
        this.state.set(State::Pending);

        // 1. Run the fetch steps given notification and settings.
        // 2. Wait for any fetches to complete and notification's image resource, icon resource, and badge resource to
        //    be set (if any), as well as the icon resources for the notification's actions (if any).
        // AD-HOC: The wait for the fetch is bounded: An icon that hasn't arrived after a few seconds isn't worth holding
        //         the notification back for, so it then shows without one.
        let fetch_timeout = CppTimer::create_single_shot(vm, ICON_FETCH_TIMEOUT_MS, this, |vm, this| {
            if let Some(icon) = &*this.icon.borrow() {
                // SAFETY: The icon is live.
                unsafe { web_notifications_api_stop_icon_fetch(icon.as_ptr()) };
            }
            Self::display(this, vm);
        });
        *this.fetch_timeout.borrow_mut(vm) = Some(fetch_timeout);
        Self::run_the_fetch_steps(this, vm);
    }

    fn fetch_steps_done(this: Ref<Self>, vm: &Vm) {
        let Some(fetch_timeout) = this.fetch_timeout.borrow().clone() else {
            return;
        };
        fetch_timeout.stop();
        Self::display(this, vm);
    }

    // https://notifications.spec.whatwg.org/#fetch-steps
    fn run_the_fetch_steps(this: Ref<Self>, vm: &Vm) {
        // 1. If the notification platform supports images and notification's image URL is set, then fetch a notification
        //    resource given notification, notification's image URL, settings, and the following step given a resource
        //    resource: set notification's image resource to resource.
        // NB: No platform we show on takes an image.

        // 2. If the notification platform supports icons and notification's icon URL is set, then fetch a notification
        //    resource given notification, notification's icon URL, settings, and the following step given a resource
        //    resource: set notification's icon resource to resource.
        let Some(icon_url) = &this.notification.icon_url else {
            Self::fetch_steps_done(this, vm);
            return;
        };
        let settings = relevant_settings_object(this.realm.global_object());
        // SAFETY: The cell and the settings object are live, the URL is lent for the call, and LibWeb returns a live
        //         icon.
        let icon = unsafe {
            CppCell::from_raw(web_notifications_api_fetch_icon(
                cell_into_abi(this),
                settings.as_ptr(),
                &RustUrl::from_url(icon_url),
            ))
        };
        *this.icon.borrow_mut(vm) = Some(icon);

        // 3. If the notification platform supports badges and notification's badge URL is set, then fetch a notification
        //    resource given notification, notification's badge URL, settings, and the following step given a resource
        //    resource: set notification's badge resource to resource.
        // NB: No platform we show on takes a badge.

        // 4. If the notification platform supports actions and action icons, then for each entry of notification's
        //    actions: if entry's icon URL is non-null, then fetch a notification resource given notification, entry's
        //    icon URL, settings, and the following step given a resource resource: set entry's icon resource to resource.
        // NB: A non-persistent notification has no actions.
    }

    // Steps 3 to 8 of https://notifications.spec.whatwg.org/#notification-show-steps
    fn display(this: Ref<Self>, vm: &Vm) {
        *this.fetch_timeout.borrow_mut(vm) = None;
        if this.state.get() != State::Pending {
            return;
        }

        // A document that went away while the icon was on its way gets no notification: The page may show another site
        // by now, and the notification would be attributed to that one.
        if !this.window().associated_document().is_fully_active() {
            this.abandon(vm);
            return;
        }

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
        this.state.set(State::Showing);
        let notification = &this.notification;
        let data = NotificationDataAbi {
            title: notification.title.raw_identity(),
            body: notification.body.raw_identity(),
            tag: notification.tag.raw_identity(),
            language: notification.language.raw_identity(),
            direction: notification.direction as u8,
            has_silent: notification.silent_preference.is_some(),
            silent: notification.silent_preference.unwrap_or(false),
            require_interaction: notification.require_interaction_preference,
            renotify: notification.renotify_preference,
        };
        let icon = this
            .icon
            .borrow()
            .as_ref()
            .map_or(core::ptr::null_mut(), CppCell::as_ptr);
        // SAFETY: The window and the icon are live, and the strings are lent for the call.
        unsafe { web_notifications_api_show_notification(this.window().as_ptr(), this.id.get(), &data, icon) };

        // 8. If notification is a non-persistent notification, then queue a task to fire an event named show on the
        //    Notification object representing notification.
        // NB: Fired from did_show(), once the platform has shown the notification: Until then the display can still fail,
        //     and a failure fires error instead.
    }

    fn document_ptr(&self) -> *mut c_void {
        self.window
            .as_ref()
            .map_or(core::ptr::null_mut(), |window| window.associated_document().as_ptr())
    }

    fn abandon(&self, vm: &Vm) {
        // Still on its way to the platform: Nothing has been shown, so there's nothing to tell the UI process.
        if self.state.get() == State::Pending {
            self.state.set(State::Closed);
            if let Some(fetch_timeout) = self.fetch_timeout.borrow_mut(vm).take() {
                fetch_timeout.stop();
            }
            if let Some(icon) = &*self.icon.borrow() {
                // SAFETY: The icon is live.
                unsafe { web_notifications_api_stop_icon_fetch(icon.as_ptr()) };
            }
            // SAFETY: The window is live.
            unsafe { web_notifications_api_unregister_notification(self.window().as_ptr(), self.id.get()) };
            return;
        }
        if self.state.get() != State::Showing && self.state.get() != State::Closing {
            return;
        }
        self.state.set(State::Closed);
        // SAFETY: The window is live.
        unsafe {
            web_notifications_api_unregister_notification(self.window().as_ptr(), self.id.get());
            web_notifications_api_abandon_notification(self.window().as_ptr(), self.id.get());
        }
    }

    fn did_show(this: Ref<Self>, vm: &Vm) {
        if this.state.get() != State::Showing {
            return;
        }
        Self::queue_task_to_fire_event(this, vm, "show");
    }

    fn did_fail_to_show(this: Ref<Self>, vm: &Vm) {
        // NB: The UI process forgets a notification it failed to show, so the close it was carrying out ends here.
        if this.state.get() == State::Closing {
            Self::did_close(this, vm);
            return;
        }
        if this.state.get() != State::Showing {
            return;
        }
        this.state.set(State::Closed);
        // SAFETY: The window is live.
        unsafe { web_notifications_api_unregister_notification(this.window().as_ptr(), this.id.get()) };
        Self::queue_task_to_fire_event(this, vm, "error");
    }

    // Step 2 of the non-persistent branch of https://notifications.spec.whatwg.org/#activating-a-notification
    fn did_click(this: Ref<Self>, vm: &Vm) {
        if this.state.get() != State::Showing && this.state.get() != State::Closing {
            return;
        }

        // Otherwise, queue a task to run these steps:
        queue_a_global_task(
            vm,
            this.realm.global_object(),
            TaskSource::UserInteraction,
            this,
            |_, this| {
                // 1. Let intoFocus be the result of firing an event named click on the Notification object representing
                //    notification, with its cancelable attribute initialized to true.
                // NB: The user activated the notification, so the window gets transient activation first: That's what lets
                //     focus() work from within the event listener, as the spec encourages.
                if let Some(window) = &this.window {
                    window.set_last_activation_timestamp_to_now();
                }
                let event = CppEvent::create(this.realm, "click", false, true, false);
                let into_focus = this.event_target.dispatch_event(&event);

                // 2. If intoFocus is true, then the user agent should bring the notification's related browsing context's
                //    viewport into focus.
                if let Some(window) = &this.window {
                    // SAFETY: The window is live.
                    unsafe {
                        web_notifications_api_handle_notification_click(window.as_ptr(), this.id.get(), into_focus);
                    }
                }
            },
        );
    }

    // https://notifications.spec.whatwg.org/#dom-notification-close
    pub(crate) fn close(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The close() method steps are to run the close steps for this's notification.
        self.close_steps();
        Ok(())
    }

    // https://notifications.spec.whatwg.org/#close-steps
    fn close_steps(&self) {
        // 1. If the list of notifications does not contain notification, then abort these steps.
        // NB: A notification that's already on its way out (Closing) is left to the close the UI process is carrying out.
        if self.state.get() != State::Showing {
            return;
        }

        // 2. Handle close events with notification.
        // 3. Remove notification from the list of notifications.
        // NB: Both happen in did_close(), once the UI process has withdrawn the notification from the platform.
        self.state.set(State::Closing);
        // SAFETY: The window is live.
        unsafe { web_notifications_api_request_close_notification(self.window().as_ptr(), self.id.get()) };
    }

    // Steps 2 and 3 of https://notifications.spec.whatwg.org/#close-steps
    fn did_close(this: Ref<Self>, vm: &Vm) {
        if this.state.get() != State::Showing && this.state.get() != State::Closing {
            return;
        }

        // 2. Handle close events with notification.
        // https://notifications.spec.whatwg.org/#handle-close-events
        // 2. If notification is a non-persistent notification, then queue a task to fire an event named close on the
        //    Notification object representing notification.
        Self::queue_task_to_fire_event(this, vm, "close");

        // 3. Remove notification from the list of notifications.
        this.state.set(State::Closed);
        // SAFETY: The window is live.
        unsafe { web_notifications_api_unregister_notification(this.window().as_ptr(), this.id.get()) };
    }

    fn queue_task_to_fire_event(this: Ref<Self>, vm: &Vm, event_name: &'static str) {
        queue_a_global_task(
            vm,
            this.realm.global_object(),
            TaskSource::Unspecified,
            (this, event_name),
            |_, (this, event_name)| {
                let event = CppEvent::create(this.realm, event_name, false, false, false);
                this.event_target.dispatch_event(&event);
            },
        );
    }

    // https://notifications.spec.whatwg.org/#dom-notification-permission
    pub(crate) fn permission(cx: &mut CallContext) -> WebIdlResult<NotificationPermission> {
        // The static permission getter steps are to return the result of getting the notifications permission state.
        // https://notifications.spec.whatwg.org/#permissions-integration
        // 1. Let permissionState be the result of getting the current permission state with "notifications".
        Ok(notification_permission_from(get_the_notifications_permission_state(
            &current_settings_object(cx.vm()),
        )))
    }

    // https://notifications.spec.whatwg.org/#dom-notification-requestpermission
    pub(crate) fn request_permission(
        cx: &mut CallContext,
        deprecated_callback: Option<CallbackFunction>,
    ) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();

        // 1. Let global be the current global object.
        let settings = current_settings_object(vm);
        let realm = settings.realm();
        let global = settings.global_object();

        // 2. Let promise be a new promise in this's relevant Realm.
        // NB: A static method has no "this"; the current realm is the one the call was made from.
        let promise = create_promise(vm, realm);

        // 3. Run these steps in parallel:
        //    1. Let permissionState be the result of requesting permission to use "notifications".
        //    2. Queue a global task on the DOM manipulation task source given global to run these steps:
        let request = PermissionRequest {
            realm,
            global,
            promise,
            deprecated_callback,
            settings: settings.clone(),
        };

        // AD-HOC: "Request permission to use" asks the user whenever the state is "prompt". We ask only from a document
        //         that is its own top-level site, and only while it has transient activation, which the request consumes.
        //         Any other request resolves with the state as it is, and nothing is stored, so the page may ask again
        //         from a click. (A document that isn't a secure context already reads "denied" from the permission
        //         state algorithm, so it never gets this far.) Each refusal is explained on the console.
        let current_state = get_the_notifications_permission_state(&settings);
        if current_state == PermissionState::Prompt {
            let window = window_from_global_object(global).expect("requestPermission() is exposed only to windows");
            let refuse = |message: &str| {
                realm.intrinsics().console_object(vm).console().output_debug_message(
                    ConsoleLogLevel::Error,
                    Utf16View::of_string(&Utf16String::from_utf8(message)),
                );
                Self::settle_permission_request(vm, request.clone());
                Ok(promise)
            };
            let is_its_own_top_level_site = settings.top_level_origin().is_some_and(|top_level_origin| {
                window.associated_document().origin().is_same_origin(&top_level_origin)
            });
            if !is_its_own_top_level_site {
                return refuse(
                    "Notification permission can only be requested from a document whose origin is the top-level origin.",
                );
            }
            if !window.has_transient_activation() {
                return refuse("Notification permission can only be requested while handling a user gesture.");
            }
            window.consume_user_activation();
        }
        request_permission_to_use(
            vm,
            PermissionName::Notifications,
            &settings,
            request,
            |vm, request, _| {
                Self::settle_permission_request(vm, request);
            },
        );

        // 4. Return promise.
        Ok(promise)
    }

    fn settle_permission_request(vm: &Vm, request: PermissionRequest) {
        // AD-HOC: "Request permission to use" reports "denied" for a prompt the user dismissed, though it stores nothing
        //         for it, and the page would then take a shrug for a refusal. The state the store holds once the
        //         answer is in is reported instead: "default" after a dismissal, so the page may ask again later.
        let permission_state = get_the_notifications_permission_state(&request.settings);
        let global = request.global;
        queue_a_global_task(
            vm,
            global,
            TaskSource::DOMManipulation,
            (request, permission_state as u8),
            |vm, (request, permission_state)| {
                let permission_state = match permission_state {
                    0 => PermissionState::Granted,
                    1 => PermissionState::Denied,
                    _ => PermissionState::Prompt,
                };
                with_temporary_execution_context_and_callbacks(request.realm, || {
                    let cx = CallContext::new(vm, request.realm);
                    let permission = enumeration_value_to_js(
                        &cx,
                        match notification_permission_from(permission_state) {
                            NotificationPermission::Default => "default",
                            NotificationPermission::Denied => "denied",
                            NotificationPermission::Granted => "granted",
                        },
                    );

                    // 1. If deprecatedCallback is given, then invoke deprecatedCallback with « permissionState » and "report".
                    if let Some(deprecated_callback) = &request.deprecated_callback {
                        deprecated_callback.invoke_and_report(&[permission]);
                    }

                    // 2. Resolve promise with permissionState.
                    resolve_promise(vm, request.promise, permission);
                });
            },
        );
    }

    // https://notifications.spec.whatwg.org/#maximum-number-of-actions-supported
    // Two is what both desktop notification platforms show room for; persistent notifications, the only kind that
    // carries actions, aren't supported yet.
    const MAX_ACTIONS: u32 = 2;

    // https://notifications.spec.whatwg.org/#dom-notification-maxactions
    pub(crate) fn max_actions(_: &mut CallContext) -> WebIdlResult<u32> {
        Ok(Self::MAX_ACTIONS)
    }

    pub(crate) fn title(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.notification.title.clone())
    }

    pub(crate) fn dir(&self, _: &mut CallContext) -> WebIdlResult<NotificationDirection> {
        Ok(self.notification.direction)
    }

    pub(crate) fn lang(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.notification.language.clone())
    }

    pub(crate) fn body(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.notification.body.clone())
    }

    pub(crate) fn navigate(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(serialize_url_for_bindings(self.notification.navigation_url.as_ref()))
    }

    pub(crate) fn tag(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.notification.tag.clone())
    }

    pub(crate) fn image(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(serialize_url_for_bindings(self.notification.image_url.as_ref()))
    }

    pub(crate) fn icon(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(serialize_url_for_bindings(self.notification.icon_url.as_ref()))
    }

    pub(crate) fn badge(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(serialize_url_for_bindings(self.notification.badge_url.as_ref()))
    }

    pub(crate) fn timestamp(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        Ok(self.notification.timestamp)
    }

    pub(crate) fn renotify(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.notification.renotify_preference)
    }

    pub(crate) fn silent(&self, _: &mut CallContext) -> WebIdlResult<Option<bool>> {
        Ok(self.notification.silent_preference)
    }

    pub(crate) fn require_interaction(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.notification.require_interaction_preference)
    }

    // https://notifications.spec.whatwg.org/#dom-notification-data
    pub(crate) fn data(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The data getter steps are to return StructuredDeserialize(this's notification's data, this's relevant Realm).
        // If this throws an exception, then return null.
        Ok(structured_deserialize_for_storage(cx, &self.notification.data).unwrap_or(Value::NULL))
    }

    // https://notifications.spec.whatwg.org/#dom-notification-actions
    pub(crate) fn actions(&self, _: &mut CallContext) -> WebIdlResult<Vec<NotificationAction>> {
        // 1. Let frozenActions be an empty list of type NotificationAction.
        let mut frozen_actions = Vec::with_capacity(self.notification.actions.len());

        // 2. For each entry of this’s notification’s actions:
        for entry in &self.notification.actions {
            // 1. Let action be a new NotificationAction.
            let action = NotificationAction {
                // 2. Set action["action"] to entry’s name.
                action: entry.name.clone(),

                // 3. Set action["title"] to entry’s title.
                title: entry.title.clone(),

                // 4. If entry’s navigation URL is non-null, then set action["navigate"] to entry’s navigation URL, serialized.
                navigate: entry
                    .navigation_url
                    .as_ref()
                    .map(|url| serialize_url_for_bindings(Some(url))),

                // 5. If entry’s icon URL is non-null, then set action["icon"] to entry’s icon URL, serialized.
                icon: entry.icon_url.as_ref().map(|url| serialize_url_for_bindings(Some(url))),
            };

            // FIXME: 6. Call Object.freeze on action, to prevent accidental mutation by scripts.

            // 7. Append action to frozenActions.
            frozen_actions.push(action);
        }

        // 3. Return the result of create a frozen array from frozenActions.
        // NB: The bindings freeze the array they build from this sequence.
        Ok(frozen_actions)
    }
}

/// # Safety
///
/// `notification` must be the live cell of a Notification.
unsafe fn notification_from_abi(notification: *mut c_void) -> Ref<Notification> {
    // SAFETY: The caller passes the live cell of a Notification.
    unsafe { cell_from_abi(notification) }
}

/// # Safety
///
/// `notification` must be the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_id_for_testing(notification: *mut c_void) -> u64 {
    // SAFETY: The caller passes a live cell.
    unsafe { notification_from_abi(notification) }.id.get()
}

/// # Safety
///
/// `notification` must be the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_document(notification: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell.
    unsafe { notification_from_abi(notification) }.document_ptr()
}

/// Whether the notification still waits for its icon.
///
/// # Safety
///
/// `notification` must be the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_is_pending(notification: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { notification_from_abi(notification) }.state.get() == State::Pending
}

/// # Safety
///
/// `notification` must be the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_icon_bytes_arrived(notification: *mut c_void) {
    // SAFETY: The caller passes a live cell.
    let notification = unsafe { notification_from_abi(notification) };
    if let Some(fetch_timeout) = &*notification.fetch_timeout.borrow() {
        fetch_timeout.stop();
    }
}

/// The icon resource is set, or there is none.
///
/// # Safety
///
/// `vm` must be the live VM, and `notification` the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_icon_fetched(vm: *mut c_void, notification: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, notification) = unsafe { (&*vm.cast::<Vm>(), notification_from_abi(notification)) };
    Notification::fetch_steps_done(notification, vm);
}

/// # Safety
///
/// `vm` must be the live VM, and `notification` the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_abandon(vm: *mut c_void, notification: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, notification) = unsafe { (&*vm.cast::<Vm>(), notification_from_abi(notification)) };
    notification.abandon(vm);
}

/// # Safety
///
/// `vm` must be the live VM, and `notification` the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_did_show(vm: *mut c_void, notification: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, notification) = unsafe { (&*vm.cast::<Vm>(), notification_from_abi(notification)) };
    Notification::did_show(notification, vm);
}

/// # Safety
///
/// `vm` must be the live VM, and `notification` the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_did_fail_to_show(vm: *mut c_void, notification: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, notification) = unsafe { (&*vm.cast::<Vm>(), notification_from_abi(notification)) };
    Notification::did_fail_to_show(notification, vm);
}

/// # Safety
///
/// `vm` must be the live VM, and `notification` the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_did_click(vm: *mut c_void, notification: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, notification) = unsafe { (&*vm.cast::<Vm>(), notification_from_abi(notification)) };
    Notification::did_click(notification, vm);
}

/// # Safety
///
/// `vm` must be the live VM, and `notification` the live cell of a Notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_notifications_api_did_close(vm: *mut c_void, notification: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, notification) = unsafe { (&*vm.cast::<Vm>(), notification_from_abi(notification)) };
    Notification::did_close(notification, vm);
}
