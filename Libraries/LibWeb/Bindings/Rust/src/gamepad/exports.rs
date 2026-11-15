/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points through which LibWeb's C++ code runs the Navigator steps of the Gamepad API, declared in
//! Libraries/LibWeb/Gamepad/NavigatorGamepad.cpp.

use core::ffi::c_void;

use libjs_rust::api::Vm;

use crate::bindings::global_object::CppWindow;
use crate::gamepad::gamepad::{GamepadDescription, GamepadDescriptionAbi, GamepadState, GamepadStateAbi};
use crate::gamepad::navigator_gamepad::{
    get_gamepads, handle_gamepad_connected, handle_gamepad_disconnected, handle_gamepad_updated,
};

/// https://w3c.github.io/gamepad/#dom-navigator-getgamepads
///
/// # Safety
///
/// The pointers must be the live VM and live Windows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_gamepad_get_gamepads(
    vm: *mut c_void,
    navigator_window: *mut c_void,
    current_window: *mut c_void,
    ctx: *mut c_void,
    on_gamepad: unsafe extern "C" fn(*mut c_void, *mut c_void),
) -> bool {
    // SAFETY: The caller passes the live VM and live Windows.
    let (vm, navigator_window, current_window) = unsafe {
        (
            &*vm.cast::<Vm>(),
            CppWindow::from_raw(navigator_window),
            CppWindow::from_raw(current_window),
        )
    };
    let Ok(gamepads) = get_gamepads(vm, &navigator_window, &current_window) else {
        return false;
    };
    for index in 0..gamepads.len() {
        let gamepad = gamepads.get(index).flatten();
        let cell = gamepad.map_or(core::ptr::null_mut(), |gamepad| gamepad.as_ptr().cast());
        // SAFETY: The cell is live, and the caller roots it.
        unsafe { on_gamepad(ctx, cell) };
    }
    true
}

/// https://w3c.github.io/gamepad/#event-gamepadconnected
///
/// # Safety
///
/// The pointers must be the live VM, a live Window and a description that the caller lends for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_gamepad_handle_gamepad_connected(
    vm: *mut c_void,
    window: *mut c_void,
    description: *const GamepadDescriptionAbi<'_>,
) {
    // SAFETY: The caller passes the live VM, a live Window and a lent description.
    let (vm, window, description) = unsafe { (&*vm.cast::<Vm>(), CppWindow::from_raw(window), &*description) };
    handle_gamepad_connected(vm, &window, GamepadDescription::from_abi(description));
}

/// https://w3c.github.io/gamepad/#dfn-receives-new-button-or-axis-input-values
///
/// # Safety
///
/// The pointers must be the live VM, a live Window and a state that the caller lends for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_gamepad_handle_gamepad_updated(
    vm: *mut c_void,
    window: *mut c_void,
    state: *const GamepadStateAbi<'_>,
) {
    // SAFETY: The caller passes the live VM, a live Window and a lent state.
    let (vm, window, state) = unsafe { (&*vm.cast::<Vm>(), CppWindow::from_raw(window), &*state) };
    handle_gamepad_updated(vm, &window, GamepadState::from_abi(state));
}

/// https://w3c.github.io/gamepad/#event-gamepaddisconnected
///
/// # Safety
///
/// The pointers must be the live VM and a live Window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_gamepad_handle_gamepad_disconnected(
    vm: *mut c_void,
    window: *mut c_void,
    handle: u32,
) {
    // SAFETY: The caller passes the live VM and a live Window.
    let (vm, window) = unsafe { (&*vm.cast::<Vm>(), CppWindow::from_raw(window)) };
    handle_gamepad_disconnected(vm, &window, handle);
}
