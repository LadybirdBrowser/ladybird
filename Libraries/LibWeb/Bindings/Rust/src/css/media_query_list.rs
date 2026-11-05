/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MediaQueryList, https://drafts.csswg.org/cssom-view/#the-mediaquerylist-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};
use libweb_rust::NativeMediaList;

use crate::bindings::callback::CallbackInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::media_query_list_event::{MediaQueryListEvent, MediaQueryListEventInit};
use crate::dom::event::EventInit;

unsafe extern "C" {
    fn web_css_media_query_list_evaluate(document: *mut c_void, media: *const c_void) -> bool;
    fn web_css_media_query_list_update_container_layout(document: *mut c_void) -> bool;
}

const CHANGE: &str = "change";

// https://drafts.csswg.org/cssom-view/#the-mediaquerylist-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MediaQueryList {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // A MediaQueryList object has an associated media query list and an associated document set on creation.
    document: CppCell,
    #[gc(untraced)]
    media: NativeMediaList,

    has_changed_state: Cell<bool>,
}

impl Wrappable for MediaQueryList {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for MediaQueryList {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl MediaQueryList {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, document: CppCell, media: NativeMediaList) -> Ref<Self> {
        let object = allocate(vm, |_| MediaQueryList {
            event_target: CppEventTarget::new(),
            realm,
            document,
            media,
            has_changed_state: Cell::new(false),
        });
        object.event_target.set_owner(object);
        object.evaluate();
        object
    }

    fn evaluate(&self) -> bool {
        // SAFETY: The document is live, and the media query list is lent for the call.
        unsafe { web_css_media_query_list_evaluate(self.document.as_ptr(), (&raw const self.media).cast()) }
    }

    fn current_matches(&self) -> bool {
        let did_match = self.media.matches();

        // NB: If our document is inside a frame, we need to update layout since that may cause our frame (and thus
        //     viewport) to resize.
        // SAFETY: The document is live.
        if unsafe { web_css_media_query_list_update_container_layout(self.document.as_ptr()) } {
            self.evaluate();
        }

        let now_matches = self.media.matches();
        if did_match != now_matches {
            self.has_changed_state.set(true);
        }
        now_matches
    }

    /// Whether the list has to outlive its last script reference, to receive change events.
    pub(crate) fn has_change_listener(&self) -> bool {
        self.event_target.has_event_listener(CHANGE)
    }

    // https://drafts.csswg.org/cssom-view/#evaluate-media-queries-and-report-changes
    pub(crate) fn evaluate_and_report_change(&self, vm: &Vm) {
        let did_match = self.current_matches();
        let now_matches = self.evaluate();
        let did_change_internally = self.has_changed_state.replace(false);

        // 1. If target’s matches state has changed since the last time these steps were run, fire an event at target
        //    using the MediaQueryListEvent constructor, with its type attribute initialized to change, its isTrusted
        //    attribute initialized to true, its media attribute initialized to target’s media, and its matches
        //    attribute initialized to target’s matches state.
        if did_change_internally || did_match != now_matches {
            let event_init = MediaQueryListEventInit {
                parent: EventInit {
                    bubbles: false,
                    cancelable: false,
                    composed: false,
                },
                matches: now_matches,
                media: self.media.media_text(),
            };
            let event =
                MediaQueryListEvent::create(vm, self.realm, &Utf16String::from_ascii(CHANGE.as_bytes()), event_init);
            event.event().set_is_trusted(true);
            self.event_target.dispatch_event(event.event());
        }
    }

    // https://drafts.csswg.org/cssom-view/#dom-mediaquerylist-media
    pub(crate) fn media(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.media.media_text())
    }

    // https://drafts.csswg.org/cssom-view/#dom-mediaquerylist-matches
    pub(crate) fn matches(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.current_matches())
    }

    // https://www.w3.org/TR/cssom-view/#dom-mediaquerylist-addlistener
    pub(crate) fn add_listener(&self, _: &mut CallContext, callback: Option<CallbackInterface>) -> WebIdlResult<()> {
        // 1. If listener is null, terminate these steps.
        let Some(callback) = callback else {
            return Ok(());
        };

        // 2. Append an event listener to the associated list of event listeners with type set to change, callback set
        //    to listener, and capture set to false, unless there already is an event listener in that list with the
        //    same type, callback, and capture.
        //    (NOTE: capture is set to false by default)
        self.event_target.add_event_listener_without_options(CHANGE, &callback);
        Ok(())
    }

    // https://www.w3.org/TR/cssom-view/#dom-mediaquerylist-removelistener
    pub(crate) fn remove_listener(&self, _: &mut CallContext, callback: Option<CallbackInterface>) -> WebIdlResult<()> {
        // 1. Remove an event listener from the associated list of event listeners, whose type is change, callback is
        //    listener, and capture is false.
        // NB: While the spec doesn't technically use remove_event_listener and instead manipulates the list directly,
        //     every major engine uses remove_event_listener. This means if an event listener removes another event
        //     listener that comes after it, the removed event listener will not be invoked.
        if let Some(callback) = callback {
            self.event_target
                .remove_event_listener_without_options(CHANGE, &callback);
        }
        Ok(())
    }
}
