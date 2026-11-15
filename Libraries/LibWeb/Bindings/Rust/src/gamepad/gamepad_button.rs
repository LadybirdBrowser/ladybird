/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GamepadButton, https://w3c.github.io/gamepad/#dom-gamepadbutton

use core::cell::Cell;

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://w3c.github.io/gamepad/#dom-gamepadbutton
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct GamepadButton {
    // https://w3c.github.io/gamepad/#dfn-pressed
    // A flag indicating that the button is pressed
    pressed: Cell<bool>,

    // https://w3c.github.io/gamepad/#dfn-touched
    // A flag indicating that the button is touched
    touched: Cell<bool>,

    // https://w3c.github.io/gamepad/#dfn-value
    // A double representing the button value scaled to the range [0 .. 1]
    value: Cell<f64>,
}

impl Wrappable for GamepadButton {}

impl GamepadButton {
    pub(crate) fn create(vm: &Vm) -> Ref<Self> {
        allocate(vm, |_| Self {
            pressed: Cell::new(false),
            touched: Cell::new(false),
            value: Cell::new(0.0),
        })
    }

    pub(crate) fn pressed(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.pressed.get())
    }

    pub(crate) fn touched(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.touched.get())
    }

    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.value.get())
    }

    pub(crate) fn is_pressed(&self) -> bool {
        self.pressed.get()
    }

    pub(crate) fn set_pressed(&self, pressed: bool) {
        self.pressed.set(pressed);
    }

    pub(crate) fn set_touched(&self, touched: bool) {
        self.touched.set(touched);
    }

    pub(crate) fn set_value(&self, value: f64) {
        self.value.set(value);
    }
}
