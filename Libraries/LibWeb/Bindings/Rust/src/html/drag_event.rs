/*
 * Copyright (c) 2024, Maciej <sppmacd@pm.me>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! DragEvent, https://html.spec.whatwg.org/multipage/dnd.html#the-dragevent-interface

use core::ffi::c_void;
use core::ptr::{NonNull, null_mut};

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Trace, Vm};

use super::data_transfer::DataTransfer;
use super::drag_data_store::{LentDragDataStore, Mode};
use crate::bindings::cpp_cell::{LentCppCell, cpp_class};
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::ui_events::mouse_event::MouseEventInit;

/// The dictionary DragEventInit of HTML/DragEvent.idl.
pub(crate) struct DragEventInit {
    pub(crate) parent: MouseEventInit,
    pub(crate) data_transfer: Option<Ref<DataTransfer>>,
}

#[repr(C)]
struct MouseEventConstructorOptions {
    view: *mut c_void,
    related_target: *mut c_void,
    screen_x: f64,
    screen_y: f64,
    client_x: f64,
    client_y: f64,
    movement_x: f64,
    movement_y: f64,
    detail: i32,
    button: i16,
    buttons: u16,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    ctrl_key: bool,
    shift_key: bool,
    alt_key: bool,
    meta_key: bool,
    modifier_alt_graph: bool,
    modifier_caps_lock: bool,
    modifier_fn: bool,
    modifier_fn_lock: bool,
    modifier_hyper: bool,
    modifier_num_lock: bool,
    modifier_scroll_lock: bool,
    modifier_super: bool,
    modifier_symbol: bool,
    modifier_symbol_lock: bool,
}

unsafe extern "C" {
    fn web_drag_event_create_mouse_event(
        global_object: *mut c_void,
        name: usize,
        options: &MouseEventConstructorOptions,
    ) -> NonNull<LentCppCell<cpp_class::Event>>;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DragEvent {
    event: CppEvent,
    data_transfer: Option<Ref<DataTransfer>>,
    data_drag_store_was_changed: bool,
}

impl Wrappable for DragEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl DragEvent {
    fn create(
        vm: &Vm,
        event: CppEvent,
        data_transfer: Option<Ref<DataTransfer>>,
        data_drag_store_was_changed: bool,
    ) -> Ref<Self> {
        let owner = allocate(vm, |_| Self {
            event,
            data_transfer,
            data_drag_store_was_changed,
        });
        // SAFETY: Both the JS constructor and the native factory provide a RustMouseEvent.
        unsafe { owner.event.set_mouse_event_owner(owner) };
        owner
    }

    pub(crate) fn construct(cx: &mut CallContext, name: Utf16String, init: DragEventInit) -> WebIdlResult<Ref<Self>> {
        let mouse = &init.parent;
        let modifiers = &mouse.parent;
        let ui = &modifiers.parent;
        let event = &ui.parent;
        let options = MouseEventConstructorOptions {
            view: ui.view.map_or(null_mut(), |view| view.wrapper().as_ptr().cast()),
            related_target: mouse
                .related_target
                .map_or(null_mut(), |target| target.wrapper().as_ptr().cast()),
            screen_x: mouse.screen_x,
            screen_y: mouse.screen_y,
            client_x: mouse.client_x,
            client_y: mouse.client_y,
            movement_x: mouse.movement_x,
            movement_y: mouse.movement_y,
            detail: ui.detail,
            button: mouse.button,
            buttons: mouse.buttons,
            bubbles: event.bubbles,
            cancelable: event.cancelable,
            composed: event.composed,
            ctrl_key: modifiers.ctrl_key,
            shift_key: modifiers.shift_key,
            alt_key: modifiers.alt_key,
            meta_key: modifiers.meta_key,
            modifier_alt_graph: modifiers.modifier_alt_graph,
            modifier_caps_lock: modifiers.modifier_caps_lock,
            modifier_fn: modifiers.modifier_fn,
            modifier_fn_lock: modifiers.modifier_fn_lock,
            modifier_hyper: modifiers.modifier_hyper,
            modifier_num_lock: modifiers.modifier_num_lock,
            modifier_scroll_lock: modifiers.modifier_scroll_lock,
            modifier_super: modifiers.modifier_super,
            modifier_symbol: modifiers.modifier_symbol,
            modifier_symbol_lock: modifiers.modifier_symbol_lock,
        };
        let name = Utf16FlyString::from_utf16_string(&name);
        // SAFETY: The global, event name and options are live and LibWeb returns the new MouseEvent.
        let event = unsafe {
            CppEvent::from_lent(
                web_drag_event_create_mouse_event(
                    cx.realm().global_object().as_ptr().cast(),
                    name.raw_identity(),
                    &options,
                )
                .as_ref(),
            )
        };
        Ok(Self::create(cx.vm(), event, init.data_transfer, false))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn data_transfer(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<DataTransfer>>> {
        Ok(self.data_transfer)
    }

    fn native_data_transfer(&self) -> Ref<DataTransfer> {
        self.data_transfer.expect("native drag events have a data transfer")
    }
}

/// https://html.spec.whatwg.org/multipage/dnd.html#fire-a-dnd-event
// DragAndDropEventHandler.cpp handles the steps that create and dispatch the event.
#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_rust_initialize_drag_event(
    vm: &Vm,
    event: &LentCppCell<cpp_class::Event>,
    store: Option<LentDragDataStore<'_>>,
    current_drag_operation: LentUtf16FlyString<'_>,
) {
    let event = CppEvent::from_lent(event);
    // 1. Let dataDragStoreWasChanged be false.
    let mut data_drag_store_was_changed = false;
    let data_transfer = store.map(|store| {
        let store = store.to_rc();
        let name = event.r#type();
        let is_drag_start = name == "dragstart";
        let is_drop = name == "drop";

        // 4. If e is dragstart, then set the drag data store mode to the read/write mode and set dataDragStoreWasChanged to true.
        if is_drag_start {
            store.set_mode(Mode::ReadWrite);
            data_drag_store_was_changed = true;

        // 5. If e is drop, set the drag data store mode to the read-only mode.
        } else if is_drop {
            store.set_mode(Mode::ReadOnly);
        }

        // 6. Let dataTransfer be a newly created DataTransfer object associated with the given drag data store.
        let data_transfer = DataTransfer::create(vm, store.clone());

        // 7. Set the effectAllowed attribute to the drag data store's drag data store allowed effects state.
        data_transfer.set_effect_allowed_internal(store.allowed_effects_state());

        // 8. Set the dropEffect attribute to "none" if e is dragstart, drag, or dragleave; to the value corresponding to
        //    the current drag operation if e is drop or dragend; and to a value based on the effectAllowed attribute's
        //    value and the drag-and-drop source, as given by the following table, otherwise (i.e. if e is dragenter or dragover):
        let effect = if is_drag_start || name == "drag" || name == "dragleave" {
            Utf16FlyString::from_utf8("none")
        } else if is_drop || name == "dragend" {
            current_drag_operation.to_utf16_fly_string()
        } else {
            // effectAllowed                                                                     | dropEffect
            // ---------------------------------------------------------------------------------------------------------------------------------------
            // "none"                                                                            | "none"
            // "copy"                                                                            | "copy"
            // "copyLink"                                                                        | "copy", or, if appropriate, "link"
            // "copyMove"                                                                        | "copy", or, if appropriate, "move"
            // "all"                                                                             | "copy", or, if appropriate, either "link" or "move"
            // "link"                                                                            | "link"
            // "linkMove"                                                                        | "link", or, if appropriate, "move"
            // "move"                                                                            | "move"
            // "uninitialized" when what is being dragged is a selection from a text control     | "move", or, if appropriate, either "copy" or "link"
            // "uninitialized" when what is being dragged is a selection                         | "copy", or, if appropriate, either "link" or "move"
            // "uninitialized" when what is being dragged is an a element with an href attribute | "link", or, if appropriate, either "copy" or "move"
            // Any other case                                                                    | "copy", or, if appropriate, either "link" or "move"
            let allowed = Utf16String::from(data_transfer.effect_allowed_value());
            let effect = if allowed == "none" {
                "none"
            } else if allowed == "link" || allowed == "linkMove" {
                "link"
            } else if allowed == "move" {
                "move"
            } else {
                // FIXME: Handle "uninitialized" when element drag operations are supported.
                "copy"
            };
            Utf16FlyString::from_utf8(effect)
        };
        data_transfer.set_drop_effect_value(effect);
        data_transfer
    });
    DragEvent::create(vm, event, data_transfer, data_drag_store_was_changed);
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_rust_finish_drag_event(event: &InterfaceCell) {
    let event = event.object::<DragEvent>();
    let data_transfer = event.native_data_transfer();

    let store = data_transfer.store().expect("the drag event still has its data store");

    // 13. Set the drag data store allowed effects state to the current value of dataTransfer's effectAllowed attribute.
    //     (It can only have changed value if e is dragstart.)
    store.set_allowed_effects_state(data_transfer.effect_allowed_value());

    // 14. If dataDragStoreWasChanged is true, then set the drag data store mode back to the protected mode.
    if event.data_drag_store_was_changed {
        store.set_mode(Mode::Protected);
    }

    drop(store);

    // 15. Break the association between dataTransfer and the drag data store.
    data_transfer.disassociate_from_store();
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_rust_drag_event_drop_effect(event: &InterfaceCell) -> usize {
    event
        .object::<DragEvent>()
        .native_data_transfer()
        .drop_effect_value()
        .into_raw()
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_rust_drag_event_operation(event: &InterfaceCell) -> usize {
    let transfer = event.object::<DragEvent>().native_data_transfer();
    let allowed = Utf16String::from(transfer.effect_allowed_value());
    let drop_effect = transfer.drop_effect_value();
    let effect = Utf16String::from(&drop_effect);

    // effectAllowed                                             | dropEffect | Drag operation
    // ---------------------------------------------------------------------------------------
    // "uninitialized", "copy", "copyLink", "copyMove", or "all" | "copy"     | "copy"
    // "uninitialized", "link", "copyLink", "linkMove", or "all" | "link"     | "link"
    // "uninitialized", "move", "copyMove", "linkMove", or "all" | "move"     | "move"
    // Any other case                                            |            | "none"
    let permitted = if effect == "copy" {
        ["uninitialized", "copy", "copyLink", "copyMove", "all"]
            .iter()
            .any(|value| allowed == *value)
    } else if effect == "link" {
        ["uninitialized", "link", "copyLink", "linkMove", "all"]
            .iter()
            .any(|value| allowed == *value)
    } else if effect == "move" {
        ["uninitialized", "move", "copyMove", "linkMove", "all"]
            .iter()
            .any(|value| allowed == *value)
    } else {
        false
    };

    if permitted {
        drop_effect
    } else {
        Utf16FlyString::from_utf8("none")
    }
    .into_raw()
}
