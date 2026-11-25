/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Navigator partial interface of the Gamepad API, https://w3c.github.io/gamepad/#extensions-to-the-navigator-interface

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Trace, Vm};

use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::Ref;
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::realm_data::realm_data;
use crate::dom::document::CppDocument;
use crate::dom::event::EventInit;
use crate::gamepad::gamepad::{Gamepad, GamepadDescription, GamepadState, current_high_resolution_time};
use crate::gamepad::gamepad_event::{GamepadEvent, GamepadEventInit};
use crate::permissions_policy::permissions_policy::PolicyControlledFeature;

unsafe extern "C" {
    fn web_gamepad_did_start_using_gamepads(window: *mut c_void);
}

/// https://w3c.github.io/gamepad/#internal-slots
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct NavigatorGamepad {
    // https://w3c.github.io/gamepad/#dfn-hasgamepadgesture
    // A flag indicating that a gamepad user gesture has been observed
    has_gamepad_gesture: Cell<bool>,

    // https://w3c.github.io/gamepad/#dfn-gamepads
    // Each Gamepad present at the index specified by its index attribute, or null for unassigned indices.
    gamepads: GcRefCell<Vec<Option<Ref<Gamepad>>>>,

    // Non-standard attribute to know which gamepads are available to the system. This is used to prevent duplicate
    // connections for the same gamepad ID (e.g. if the navigator object is initialized and checks for connected gamepads
    // and also receives a gamepad connected event)
    #[gc(untraced)]
    available_gamepads: RefCell<Vec<u32>>,
}

static NAVIGATOR_GAMEPAD_CLASS: EmbedderClass<NavigatorGamepad> = EmbedderClass::new("NavigatorGamepad");

impl EmbedderValue for NavigatorGamepad {
    const CLASS: &'static EmbedderClass<Self> = &NAVIGATOR_GAMEPAD_CLASS;
}

pub(crate) type NavigatorGamepadRef = Gc<EmbedderCell<NavigatorGamepad>>;

pub(crate) fn navigator_gamepad(vm: &Vm, window: &CppWindow) -> NavigatorGamepadRef {
    let data = realm_data(vm, window.realm());
    if let Some(navigator) = data.navigator_gamepad.get() {
        return navigator;
    }
    let navigator = NAVIGATOR_GAMEPAD_CLASS.allocate(vm, |_| NavigatorGamepad {
        has_gamepad_gesture: Cell::new(false),
        gamepads: GcRefCell::default(),
        available_gamepads: RefCell::default(),
    });
    data.navigator_gamepad.set(Some(navigator));
    navigator
}

pub(crate) fn fire_a_gamepad_event(vm: &Vm, window: &CppWindow, event_name: &str, gamepad: Ref<Gamepad>) {
    let event_init = GamepadEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        gamepad: Some(gamepad),
    };
    let event = GamepadEvent::create(vm, window.realm(), &Utf16String::from_utf8(event_name), event_init);
    window.as_event_target().dispatch_event(event.event());
}

pub(crate) struct NotAllowedToUseGamepads;

impl NavigatorGamepad {
    pub(crate) fn has_gamepad_gesture(&self) -> bool {
        self.has_gamepad_gesture.get()
    }

    pub(crate) fn set_has_gamepad_gesture(&self, has_gamepad_gesture: bool) {
        self.has_gamepad_gesture.set(has_gamepad_gesture);
    }

    pub(crate) fn gamepads<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Option<Ref<Gamepad>>> {
        let gamepads = MarkedVec::new(vm);
        gamepads.extend_from_slice(&self.gamepads.borrow());
        gamepads
    }
}

// https://w3c.github.io/gamepad/#dom-navigator-getgamepads
pub(crate) fn get_gamepads<'vm>(
    vm: &'vm Vm,
    navigator_window: &CppWindow,
    current_window: &CppWindow,
) -> Result<MarkedVec<'vm, Option<Ref<Gamepad>>>, NotAllowedToUseGamepads> {
    let navigator = navigator_gamepad(vm, navigator_window);

    // 1. Let doc be the current global object's associated Document.
    let document = current_window.associated_document();

    // 2. If doc is null or doc is not fully active, then return an empty list.
    let gamepads = MarkedVec::new(vm);
    if !document.is_fully_active() {
        return Ok(gamepads);
    }

    // 3. If doc is not allowed to use the "gamepad" permission, then throw a "SecurityError" DOMException and abort these steps.
    if !document.is_allowed_to_use_feature(PolicyControlledFeature::Gamepad) {
        return Err(NotAllowedToUseGamepads);
    }

    // AD-HOC: Tell the UI process that this process consumes gamepad input.
    // SAFETY: The Window is live.
    unsafe { web_gamepad_did_start_using_gamepads(current_window.as_ptr()) };

    // 4. If this.[[hasGamepadGesture]] is false, then return an empty list.
    if !navigator.has_gamepad_gesture.get() {
        return Ok(gamepads);
    }

    // 5. Let now be the current high resolution time given the current global object.
    let now = current_high_resolution_time(current_window);

    // 6. Let gamepads be an empty list.
    // NOTE: Already done.

    // 7. For each gamepad of this.[[gamepads]]:
    for gamepad in navigator.gamepads.borrow().iter().copied() {
        // 1. If gamepad is not null and gamepad.[[exposed]] is false:
        if let Some(gamepad) = gamepad
            && !gamepad.is_exposed()
        {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad.set_exposed(true);

            // 2. Set gamepad.[[timestamp]] to now.
            gamepad.set_timestamp(now);
        }

        // 2. Append gamepad to gamepads.
        gamepads.push(gamepad);
    }

    // 8. Return gamepads.
    Ok(gamepads)
}

// https://w3c.github.io/gamepad/#dfn-selecting-an-unused-gamepad-index
pub(crate) fn select_an_unused_gamepad_index(vm: &Vm, navigator: &NavigatorGamepadRef) -> usize {
    let mut gamepads = navigator.gamepads.borrow_mut(vm);

    // 2. Let maxGamepadIndex be the size of navigator.[[gamepads]] − 1.
    // 3. For each gamepadIndex of the range from 0 to maxGamepadIndex:
    for (gamepad_index, gamepad) in gamepads.iter().enumerate() {
        // 1. If navigator.[[gamepads]][gamepadIndex] is null, then return gamepadIndex.
        if gamepad.is_none() {
            return gamepad_index;
        }
    }

    // 4. Append null to navigator.[[gamepads]].
    gamepads.push(None);

    // 5. Return the size of navigator.[[gamepads]] − 1.
    gamepads.len() - 1
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct GamepadConnection {
    window: CppWindow,
    document: CppDocument,
    #[gc(untraced)]
    description: GamepadDescription,
}

// https://w3c.github.io/gamepad/#event-gamepadconnected
pub(crate) fn handle_gamepad_connected(vm: &Vm, window: &CppWindow, description: GamepadDescription) {
    let navigator = navigator_gamepad(vm, window);

    // When a gamepad becomes available on the system, run the following steps:
    if navigator.available_gamepads.borrow().contains(&description.handle) {
        return;
    }

    // 1. Let document be the current global object's associated Document; otherwise null.
    // FIXME: We can't use the current global object here, since it's not executing in a scripting context.
    // NOTE: NavigatorGamepad is only available on Window.
    // NOTE: document is never null.
    let global = window.realm().global_object();
    let document = window.associated_document();

    // 2. If document is not null and is not allowed to use the "gamepad" permission, then abort these steps.
    if !document.is_allowed_to_use_feature(PolicyControlledFeature::Gamepad) {
        return;
    }

    navigator.available_gamepads.borrow_mut().push(description.handle);

    // 3. Queue a global task on the gamepad task source with the current global object to perform the following steps:
    let connection = GamepadConnection {
        window: window.clone(),
        document,
        description,
    };
    queue_a_global_task(vm, global, TaskSource::Gamepad, connection, |vm, connection| {
        // 1. Let gamepad be a new Gamepad representing the gamepad.
        let gamepad = Gamepad::create(vm, &connection.window, connection.description);

        // 2. Let navigator be gamepad's relevant global object's Navigator object.
        let navigator = navigator_gamepad(vm, &connection.window);

        // 3. Set navigator.[[gamepads]][gamepad.index] to gamepad.
        navigator.gamepads.borrow_mut(vm)[gamepad.gamepad_index()] = Some(gamepad);

        // 4. If navigator.[[hasGamepadGesture]] is true:
        if navigator.has_gamepad_gesture.get() {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad.set_exposed(true);

            // 2. If document is not null and is fully active, then fire an event named gamepadconnected at gamepad's
            //    relevant global object using GamepadEvent with its gamepad attribute initialized to gamepad.
            if connection.document.is_fully_active() {
                fire_a_gamepad_event(vm, &connection.window, "gamepadconnected", gamepad);
            }
        }
    });
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct GamepadUpdate {
    window: CppWindow,
    #[gc(untraced)]
    state: GamepadState,
}

// https://w3c.github.io/gamepad/#dfn-receives-new-button-or-axis-input-values
pub(crate) fn handle_gamepad_updated(vm: &Vm, window: &CppWindow, state: GamepadState) {
    // When the system receives new button or axis input values, run the following steps:
    if !navigator_gamepad(vm, window)
        .available_gamepads
        .borrow()
        .contains(&state.handle)
    {
        return;
    }

    // 2. Queue a global task on the gamepad task source with gamepad's relevant global object to update gamepad state
    //    for gamepad.
    // AD-HOC: Step 1 runs in the task. A newly available gamepad is created by an earlier task on the same task
    //         source, so input received before that task has run still reaches the gamepad.
    let global = window.realm().global_object();
    let update = GamepadUpdate {
        window: window.clone(),
        state,
    };
    queue_a_global_task(vm, global, TaskSource::Gamepad, update, |vm, update| {
        // 1. Let gamepad be the Gamepad object representing the device that received new button or axis input values.
        let navigator = navigator_gamepad(vm, &update.window);
        let gamepad = navigator
            .gamepads
            .borrow()
            .iter()
            .flatten()
            .copied()
            .find(|gamepad| gamepad.handle() == update.state.handle);
        let Some(gamepad) = gamepad else {
            return;
        };

        gamepad.set_latest_state(update.state);
        Gamepad::update_gamepad_state(gamepad, vm);
    });
}

pub(crate) fn handle_gamepad_disconnected(vm: &Vm, window: &CppWindow, handle: u32) {
    // When a gamepad becomes unavailable on the system, run the following steps:
    let navigator = navigator_gamepad(vm, window);
    let was_available = {
        let mut available_gamepads = navigator.available_gamepads.borrow_mut();
        let position = available_gamepads
            .iter()
            .position(|available_gamepad| *available_gamepad == handle);
        position.map(|position| available_gamepads.remove(position)).is_some()
    };

    if !was_available {
        return;
    }

    // 2. Queue a global task on the gamepad task source with gamepad's relevant global object to perform the
    //    following steps:
    // AD-HOC: Step 1 runs in the task. A newly available gamepad is created by an earlier task on the same task
    //         source, so a gamepad that becomes unavailable before that task has run is still removed.
    let global = window.realm().global_object();
    queue_a_global_task(
        vm,
        global,
        TaskSource::Gamepad,
        (window.clone(), handle),
        |vm, (window, handle)| {
            // 1. Let gamepad be the Gamepad representing the unavailable device.
            let navigator = navigator_gamepad(vm, &window);
            let gamepad = navigator
                .gamepads
                .borrow()
                .iter()
                .flatten()
                .copied()
                .find(|gamepad| gamepad.handle() == handle);
            let Some(gamepad) = gamepad else {
                return;
            };

            // 1. Set gamepad.[[connected]] to false.
            gamepad.set_connected(false);

            // 2. Let document be gamepad's relevant global object's associated Document; otherwise null.
            let document = window.associated_document();

            // 3. If gamepad.[[exposed]] is true and document is not null and is fully active, then fire an event named
            //    gamepaddisconnected at gamepad's relevant global object using GamepadEvent with its gamepad attribute
            //    initialized to gamepad.
            if gamepad.is_exposed() && document.is_fully_active() {
                fire_a_gamepad_event(vm, &window, "gamepaddisconnected", gamepad);
            }

            // 4. Let navigator be gamepad's relevant global object's Navigator object.
            // 5. Set navigator.[[gamepads]][gamepad.index] to null.
            let mut gamepads = navigator.gamepads.borrow_mut(vm);
            gamepads[gamepad.gamepad_index()] = None;

            // 6. While navigator.[[gamepads]] is not empty and the last item of navigator.[[gamepads]] is null, remove the
            //    last item of navigator.[[gamepads]].
            while gamepads.last().is_some_and(Option::is_none) {
                gamepads.pop();
            }
        },
    );
}
