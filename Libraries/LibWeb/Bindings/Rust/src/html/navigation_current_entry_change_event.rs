/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! NavigationCurrentEntryChangeEvent,
//! https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-navigationcurrententrychangeevent-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::dom::event::EventInit;
use crate::html::navigation_type::NavigationType;

/// The dictionary NavigationCurrentEntryChangeEventInit of HTML/NavigationCurrentEntryChangeEvent.idl.
pub(crate) struct NavigationCurrentEntryChangeEventInit {
    pub(crate) parent: EventInit,
    pub(crate) navigation_type: Option<NavigationType>,
    pub(crate) from: CppInterface,
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationcurrententrychangeevent
#[derive(Trace)]
pub(crate) struct NavigationCurrentEntryChangeEvent {
    event: CppEvent,
    #[gc(untraced)]
    navigation_type: Option<NavigationType>,
    from: CppCell,
}

impl Wrappable for NavigationCurrentEntryChangeEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl NavigationCurrentEntryChangeEvent {
    fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        navigation_type: Option<NavigationType>,
        from: CppCell,
    ) -> Ref<NavigationCurrentEntryChangeEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| NavigationCurrentEntryChangeEvent {
            event,
            navigation_type,
            from,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: NavigationCurrentEntryChangeEventInit,
    ) -> WebIdlResult<Ref<NavigationCurrentEntryChangeEvent>> {
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            &event_init_dict.parent,
            event_init_dict.navigation_type,
            event_init_dict.from.cell(),
        ))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationcurrententrychangeevent-navigationtype
    pub(crate) fn navigation_type(&self, _: &mut CallContext) -> WebIdlResult<Option<NavigationType>> {
        Ok(self.navigation_type)
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationcurrententrychangeevent-from
    pub(crate) fn from(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(wrap_cpp_cell(cx, &self.from))
    }
}

/// The Event of a new NavigationCurrentEntryChangeEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_navigation_current_entry_change_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    has_navigation_type: bool,
    navigation_type: u8,
    from: &LentCppCell<cpp_class::Cell>,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles: false,
        cancelable: false,
        composed: false,
    };
    let navigation_type = has_navigation_type.then(|| match navigation_type {
        0 => NavigationType::Push,
        1 => NavigationType::Replace,
        2 => NavigationType::Reload,
        3 => NavigationType::Traverse,
        _ => unreachable!("LibWeb passes a NavigationType"),
    });
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let from = CppCell::from_lent(from);
    let event = NavigationCurrentEntryChangeEvent::create(vm, realm, &r#type, &event_init, navigation_type, from);
    event.event().as_ptr()
}
