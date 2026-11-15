/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Gamepad, https://w3c.github.io/gamepad/#dom-gamepad

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, MarkedVec, Realm, Trace, Vm};

use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::gamepad::gamepad_button::GamepadButton;
use crate::gamepad::gamepad_haptic_actuator::GamepadHapticActuator;
use crate::gamepad::navigator_gamepad::{fire_a_gamepad_event, navigator_gamepad, select_an_unused_gamepad_index};

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct GamepadInputDescriptor {
    pub(crate) has_canonical_index: bool,
    pub(crate) canonical_index: u32,
    pub(crate) logical_minimum: i16,
    pub(crate) logical_maximum: i16,
}

#[derive(Clone)]
pub(crate) struct GamepadDescription {
    pub(crate) handle: u32,
    pub(crate) id: String,
    pub(crate) has_standard_mapping: bool,
    pub(crate) supports_dual_rumble: bool,
    pub(crate) supports_trigger_rumble: bool,
    pub(crate) axes: Vec<GamepadInputDescriptor>,
    pub(crate) buttons: Vec<GamepadInputDescriptor>,
}

#[derive(Clone, Default)]
pub(crate) struct GamepadState {
    pub(crate) handle: u32,
    pub(crate) axis_values: Vec<i16>,
    pub(crate) button_values: Vec<i16>,
}

#[repr(C)]
pub struct GamepadDescriptionAbi<'a> {
    handle: u32,
    id: ak::Span<'a, u8>,
    has_standard_mapping: bool,
    supports_dual_rumble: bool,
    supports_trigger_rumble: bool,
    axes: ak::Span<'a, GamepadInputDescriptor>,
    buttons: ak::Span<'a, GamepadInputDescriptor>,
}

#[repr(C)]
pub struct GamepadStateAbi<'a> {
    handle: u32,
    axis_values: ak::Span<'a, i16>,
    button_values: ak::Span<'a, i16>,
}

impl GamepadDescription {
    pub(crate) fn from_abi(description: &GamepadDescriptionAbi<'_>) -> Self {
        Self {
            handle: description.handle,
            id: String::from_utf8_lossy(description.id.as_slice()).into_owned(),
            has_standard_mapping: description.has_standard_mapping,
            supports_dual_rumble: description.supports_dual_rumble,
            supports_trigger_rumble: description.supports_trigger_rumble,
            axes: description.axes.as_slice().to_vec(),
            buttons: description.buttons.as_slice().to_vec(),
        }
    }

    fn to_abi(&self) -> GamepadDescriptionAbi<'_> {
        GamepadDescriptionAbi {
            handle: self.handle,
            id: ak::Span::new(self.id.as_bytes()),
            has_standard_mapping: self.has_standard_mapping,
            supports_dual_rumble: self.supports_dual_rumble,
            supports_trigger_rumble: self.supports_trigger_rumble,
            axes: ak::Span::new(&self.axes),
            buttons: ak::Span::new(&self.buttons),
        }
    }
}

impl GamepadState {
    pub(crate) fn from_abi(state: &GamepadStateAbi<'_>) -> Self {
        Self {
            handle: state.handle,
            axis_values: state.axis_values.as_slice().to_vec(),
            button_values: state.button_values.as_slice().to_vec(),
        }
    }

    fn to_abi(&self) -> GamepadStateAbi<'_> {
        GamepadStateAbi {
            handle: self.handle,
            axis_values: ak::Span::new(&self.axis_values),
            button_values: ak::Span::new(&self.button_values),
        }
    }
}

unsafe extern "C" {
    fn web_high_resolution_time_current_high_resolution_time(global_object: *mut c_void) -> f64;
    fn web_gamepad_normalize_axis_value(logical_value: i16, logical_minimum: i16, logical_maximum: i16) -> f64;
    fn web_gamepad_normalize_button_value(logical_value: i16, logical_minimum: i16, logical_maximum: i16) -> f64;
    fn web_gamepad_is_button_pressed(logical_value: i16, logical_minimum: i16, logical_maximum: i16) -> bool;
    fn web_gamepad_contains_gamepad_user_gesture(
        description: *const GamepadDescriptionAbi,
        state: *const GamepadStateAbi,
    ) -> bool;
}

/// The enumeration GamepadMappingType of Gamepad/Gamepad.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum GamepadMappingType {
    Empty,
    Standard,
    #[allow(dead_code, reason = "no gamepad uses the xr-standard mapping")]
    XrStandard,
}

pub(crate) fn current_high_resolution_time(window: &CppWindow) -> f64 {
    // SAFETY: The Window's global object is live.
    unsafe { web_high_resolution_time_current_high_resolution_time(window.realm().global_object().as_ptr().cast()) }
}

struct InitializedInputs {
    mapping: Vec<usize>,
    minimums: Vec<i16>,
    maximums: Vec<i16>,
    size: usize,
}

// https://w3c.github.io/gamepad/#dom-gamepad
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Gamepad {
    window: CppWindow,
    #[gc(untraced)]
    description: GamepadDescription,

    // https://w3c.github.io/gamepad/#dom-gamepad-id
    // An identification string for the gamepad. This string identifies the brand or style of connected gamepad device.
    // The exact format of the id string is left unspecified. It is RECOMMENDED that the user agent select a string
    // that identifies the product without uniquely identifying the device. For example, a USB gamepad may be
    // identified by its idVendor and idProduct values. Unique identifiers like serial numbers or Bluetooth device
    // addresses MUST NOT be included in the id string.
    id: Utf16String,

    // https://w3c.github.io/gamepad/#dom-gamepad-index
    // The index of the gamepad in the Navigator. When multiple gamepads are connected to a user agent, indices MUST be
    // assigned on a first-come, first-serve basis, starting at zero. If a gamepad is disconnected, previously assigned
    // indices MUST NOT be reassigned to gamepads that continue to be connected. However, if a gamepad is disconnected,
    // and subsequently the same or a different gamepad is then connected, the lowest previously used index MUST be
    // reused.
    index: usize,

    // https://w3c.github.io/gamepad/#dfn-connected
    // A flag indicating that the device is connected to the system
    connected: Cell<bool>,

    // https://w3c.github.io/gamepad/#dfn-timestamp
    // The last time data for this Gamepad was updated
    timestamp: Cell<f64>,

    // https://w3c.github.io/gamepad/#dfn-axes
    // A sequence of double values representing the current state of axes exposed by this device.
    // https://w3c.github.io/gamepad/#dom-gamepad-axes
    // Array of values for all axes of the gamepad. All axis values MUST be linearly normalized to the range [-1 .. 1].
    // If the controller is perpendicular to the ground with the directional stick pointing up, -1 SHOULD correspond to
    // "forward" or "left", and 1 SHOULD correspond to "backward" or "right". Axes that are drawn from a 2D input
    // device SHOULD appear next to each other in the axes array, X then Y. It is RECOMMENDED that axes appear in
    // decreasing order of importance, such that element 0 and 1 typically represent the X and Y axis of a directional
    // stick. The same object MUST be returned until the user agent needs to return different values (or values in a
    // different order).
    // FIXME: Our current FrozenArray implementation only supports returning new objects everytime.
    #[gc(untraced)]
    axes: RefCell<Vec<f64>>,

    // https://w3c.github.io/gamepad/#dfn-axismapping
    // Mapping from unmapped axis index to an index in the axes array
    axis_mapping: Vec<usize>,

    // https://w3c.github.io/gamepad/#dfn-axisminimums
    // A list containing the minimum logical value for each axis
    // NOTE: While the Gamepad API internally uses u32 to represent raw axis values, SDL uses i16 for axes.
    axis_minimums: Vec<i16>,

    // https://w3c.github.io/gamepad/#dfn-axismaximums
    // A list containing the maximum logical value for each axis
    // NOTE: While the Gamepad API internally uses u32 to represent raw axis values, SDL uses i16 for axes.
    axis_maximums: Vec<i16>,

    // https://w3c.github.io/gamepad/#dfn-buttons
    // A sequence of GamepadButton objects representing the current state of buttons exposed by this device
    // Array of button states for all buttons of the gamepad. It is RECOMMENDED that buttons appear in decreasing
    // importance such that the primary button, secondary button, tertiary button, and so on appear as elements 0, 1,
    // 2, ... in the buttons array. The same object MUST be returned until the user agent needs to return different
    // values (or values in a different order).
    // FIXME: Our current FrozenArray implementation only supports returning new objects everytime.
    buttons: Vec<Ref<GamepadButton>>,

    // https://w3c.github.io/gamepad/#dfn-buttonmapping
    // Mapping from unmapped button index to an index in the buttons array
    button_mapping: Vec<usize>,

    // https://w3c.github.io/gamepad/#dfn-buttonminimums
    // A list containing the minimum logical value for each button.
    // NOTE: While the Gamepad API internally uses u32 to represent raw button values, SDL uses bool for buttons and
    //       i16 for axes. The left and right triggers are buttons in the Gamepad API.
    button_minimums: Vec<i16>,

    // https://w3c.github.io/gamepad/#dfn-buttonmaximums
    // A list containing the maximum logical value for each button
    button_maximums: Vec<i16>,

    // https://w3c.github.io/gamepad/#dfn-exposed
    // A flag indicating that the Gamepad object has been exposed to script
    exposed: Cell<bool>,

    // https://w3c.github.io/gamepad/#dfn-vibrationactuator
    vibration_actuator: Cell<Option<Ref<GamepadHapticActuator>>>,

    // https://w3c.github.io/gamepad/#dom-gamepad-mapping
    // The mapping in use for this device. If the user agent has knowledge of the layout of the device, then it SHOULD
    // indicate that a mapping is in use by setting mapping to the corresponding GamepadMappingType value.
    #[gc(untraced)]
    mapping: GamepadMappingType,

    #[gc(untraced)]
    latest_state: RefCell<GamepadState>,
}

impl Wrappable for Gamepad {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.window.realm())
    }
}

// https://w3c.github.io/gamepad/#dfn-initializing-axes
fn initialize_axes(description: &GamepadDescription) -> InitializedInputs {
    // 1. Let inputCount be the number of axis inputs exposed by the device represented by gamepad.
    let inputs = &description.axes;

    // 2. Set gamepad.[[axisMinimums]] to a list of unsigned long values with size equal to inputCount containing minimum logical values for each of the axis inputs.
    // 3. Set gamepad.[[axisMaximums]] to a list of unsigned long values with size equal to inputCount containing maximum logical values for each of the axis inputs.
    let minimums = inputs.iter().map(|input| input.logical_minimum).collect();
    let maximums = inputs.iter().map(|input| input.logical_maximum).collect();
    let mut mapping = vec![0; inputs.len()];

    // 4. Let unmappedInputList be an empty list.
    let mut unmapped_input_list = Vec::new();

    // 5. Let mappedIndexList be an empty list.
    let mut mapped_index_list = Vec::new();

    // 6. Let axesSize be 0.
    let mut axes_size = 0;

    // 7. For each rawInputIndex of the range from 0 to inputCount − 1:
    for (raw_input_index, input) in inputs.iter().enumerate() {
        // 1. If the gamepad axis at index rawInputIndex represents a Standard Gamepad axis:
        if input.has_canonical_index {
            // 1. Let canonicalIndex be the canonical index for the axis.
            let canonical_index = input.canonical_index as usize;

            // 2. If mappedIndexList contains canonicalIndex, then append rawInputIndex to unmappedInputList.
            if mapped_index_list.contains(&canonical_index) {
                unmapped_input_list.push(raw_input_index);
            } else {
                // Otherwise:
                // 1. Set gamepad.[[axisMapping]][rawInputIndex] to canonicalIndex.
                mapping[raw_input_index] = canonical_index;

                // 2. Append canonicalIndex to mappedIndexList.
                mapped_index_list.push(canonical_index);

                // 3. If canonicalIndex + 1 is greater than axesSize, then set axesSize to canonicalIndex + 1.
                if canonical_index + 1 > axes_size {
                    axes_size = canonical_index + 1;
                }
            }
        } else {
            // Otherwise, append rawInputIndex to unmappedInputList.
            unmapped_input_list.push(raw_input_index);
        }
    }

    // 8. Let axisIndex be 0.
    let mut axis_index = 0;

    // 9. For each rawInputIndex of unmappedInputList:
    for raw_input_index in unmapped_input_list {
        // 1. While mappedIndexList contains axisIndex:
        while mapped_index_list.contains(&axis_index) {
            // 1. Increment axisIndex.
            axis_index += 1;
        }

        // 2. Set gamepad.[[axisMapping]][rawInputIndex] to axisIndex.
        mapping[raw_input_index] = axis_index;

        // 3. Append axisIndex to mappedIndexList.
        mapped_index_list.push(axis_index);

        // 4. If axisIndex + 1 is greater than axesSize, then set axesSize to axisIndex + 1.
        if axis_index + 1 > axes_size {
            axes_size = axis_index + 1;
        }
    }

    // NB: Instead of returning a list, the caller creates the axes from the size.
    // 10. Let axes be an empty list.
    // 11. For each axisIndex of the range from 0 to axesSize − 1, append 0 to axes.
    // 12. Return axes.
    InitializedInputs {
        mapping,
        minimums,
        maximums,
        size: axes_size,
    }
}

// https://w3c.github.io/gamepad/#dfn-initializing-buttons
fn initialize_buttons(description: &GamepadDescription) -> InitializedInputs {
    // 1. Let inputCount be the number of button inputs exposed by the device represented by gamepad.
    let inputs = &description.buttons;

    // 2. Set gamepad.[[buttonMinimums]] to be a list of unsigned long values with size equal to inputCount containing minimum logical values for each of the button inputs.
    // 3. Set gamepad.[[buttonMaximums]] to be a list of unsigned long values with size equal to inputCount containing maximum logical values for each of the button inputs.
    let minimums = inputs.iter().map(|input| input.logical_minimum).collect();
    let maximums = inputs.iter().map(|input| input.logical_maximum).collect();
    let mut mapping = vec![0; inputs.len()];

    // 4. Let unmappedInputList be an empty list.
    let mut unmapped_input_list = Vec::new();

    // 5. Let mappedIndexList be an empty list.
    let mut mapped_index_list = Vec::new();

    // 6. Let buttonsSize be 0.
    let mut buttons_size = 0;

    // 7. For each rawInputIndex of the range from 0 to inputCount − 1:
    for (raw_input_index, input) in inputs.iter().enumerate() {
        // 1. If the gamepad button at index rawInputIndex represents a Standard Gamepad button:
        if input.has_canonical_index {
            // 1. Let canonicalIndex be the canonical index for the button.
            let canonical_index = input.canonical_index as usize;

            // 2. If mappedIndexList contains canonicalIndex, then append rawInputIndex to unmappedInputList.
            if mapped_index_list.contains(&canonical_index) {
                unmapped_input_list.push(raw_input_index);
            } else {
                // Otherwise:
                // 1. Set gamepad.[[buttonMapping]][rawInputIndex] to canonicalIndex.
                mapping[raw_input_index] = canonical_index;

                // 2. Append canonicalIndex to mappedIndexList.
                mapped_index_list.push(canonical_index);

                // 3. If canonicalIndex + 1 is greater than buttonsSize, then set buttonsSize to canonicalIndex + 1.
                if canonical_index + 1 > buttons_size {
                    buttons_size = canonical_index + 1;
                }
            }
        } else {
            // Otherwise, append rawInputIndex to unmappedInputList.
            unmapped_input_list.push(raw_input_index);
        }

        // 2. Increment rawInputIndex.
    }

    // 8. Let buttonIndex be 0.
    let mut button_index = 0;

    // 9. For each rawInputIndex of unmappedInputList:
    for raw_input_index in unmapped_input_list {
        // 1. While mappedIndexList contains buttonIndex:
        while mapped_index_list.contains(&button_index) {
            // 1. Increment buttonIndex.
            button_index += 1;
        }

        // 2. Set gamepad.[[buttonMapping]][rawInputIndex] to buttonIndex.
        mapping[raw_input_index] = button_index;

        // 3. Append buttonIndex to mappedIndexList.
        mapped_index_list.push(button_index);

        // 4. If buttonIndex + 1 is greater than buttonsSize, then set buttonsSize to buttonIndex + 1.
        if button_index + 1 > buttons_size {
            buttons_size = button_index + 1;
        }
    }

    // NB: Instead of returning a list, the caller creates the buttons from the size.
    // 10. Let buttons be an empty list.
    // 11. For each buttonIndex of the range from 0 to buttonsSize − 1, append a new GamepadButton to buttons.
    // 12. Return buttons.
    InitializedInputs {
        mapping,
        minimums,
        maximums,
        size: buttons_size,
    }
}

// https://w3c.github.io/gamepad/#dfn-selecting-a-mapping
fn select_a_mapping(description: &GamepadDescription) -> GamepadMappingType {
    // 1. If the button and axis layout of the gamepad device corresponds with the Standard Gamepad layout, then
    //    return "standard".
    // 2. Return "".
    // NB: The UI process performs this check when it builds the description.
    if description.has_standard_mapping {
        GamepadMappingType::Standard
    } else {
        GamepadMappingType::Empty
    }
}

impl Gamepad {
    // https://w3c.github.io/gamepad/#dfn-a-new-gamepad
    pub(crate) fn create(vm: &Vm, window: &CppWindow, description: GamepadDescription) -> Ref<Self> {
        // 1. Let gamepad be a newly created Gamepad instance:
        //    1. Initialize gamepad's id attribute to an identification string for the gamepad.
        let id = Utf16String::from_utf8(&description.id);

        //    2. Initialize gamepad's index attribute to the result of selecting an unused gamepad index for gamepad.
        //    https://w3c.github.io/gamepad/#dfn-selecting-an-unused-gamepad-index
        //    1. Let navigator be gamepad's relevant global object's Navigator object.
        //    The rest of the steps are implemented in navigator_gamepad.rs.
        //    NOTE: Gamepad is only exposed on Window.
        let index = select_an_unused_gamepad_index(vm, &navigator_gamepad(vm, window));

        //    3. Initialize gamepad's mapping attribute to the result of selecting a mapping for the gamepad device.
        let mapping = select_a_mapping(&description);

        //    4. Set gamepad.[[connected]] to true.
        //    5. Set gamepad.[[timestamp]] to the current high resolution time given gamepad's relevant global object.
        let timestamp = current_high_resolution_time(window);

        //    6. Set gamepad.[[axes]] to the result of initializing axes for gamepad.
        let axes = initialize_axes(&description);

        //    7. Set gamepad.[[buttons]] to the result of initializing buttons for gamepad.
        let buttons = initialize_buttons(&description);
        let button_objects = MarkedVec::with_capacity(vm, buttons.size);
        for _ in 0..buttons.size {
            button_objects.push(GamepadButton::create(vm));
        }

        let gamepad = allocate(vm, |_| Self {
            window: window.clone(),
            description,
            id,
            index,
            connected: Cell::new(true),
            timestamp: Cell::new(timestamp),
            axes: RefCell::new(vec![0.0; axes.size]),
            axis_mapping: axes.mapping,
            axis_minimums: axes.minimums,
            axis_maximums: axes.maximums,
            buttons: button_objects.to_vec(),
            button_mapping: buttons.mapping,
            button_minimums: buttons.minimums,
            button_maximums: buttons.maximums,
            exposed: Cell::new(false),
            vibration_actuator: Cell::new(None),
            mapping,
            latest_state: RefCell::default(),
        });

        //    8. Set gamepad.[[vibrationActuator]] to the result of constructing a GamepadHapticActuator for gamepad.
        gamepad
            .vibration_actuator
            .set(Some(GamepadHapticActuator::create(vm, window, gamepad)));

        // 2. Return gamepad.
        gamepad
    }

    pub(crate) fn handle(&self) -> u32 {
        self.description.handle
    }

    pub(crate) fn description(&self) -> &GamepadDescription {
        &self.description
    }

    pub(crate) fn gamepad_index(&self) -> usize {
        self.index
    }

    pub(crate) fn is_exposed(&self) -> bool {
        self.exposed.get()
    }

    pub(crate) fn set_exposed(&self, exposed: bool) {
        self.exposed.set(exposed);
    }

    pub(crate) fn set_connected(&self, connected: bool) {
        self.connected.set(connected);
    }

    pub(crate) fn set_timestamp(&self, timestamp: f64) {
        self.timestamp.set(timestamp);
    }

    pub(crate) fn set_latest_state(&self, state: GamepadState) {
        *self.latest_state.borrow_mut() = state;
    }

    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.id.clone())
    }

    pub(crate) fn index(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(i32::try_from(self.index).expect("a gamepad index fits in a long"))
    }

    pub(crate) fn connected(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.connected.get())
    }

    pub(crate) fn timestamp(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.timestamp.get())
    }

    pub(crate) fn mapping(&self, _: &mut CallContext) -> WebIdlResult<GamepadMappingType> {
        Ok(self.mapping)
    }

    pub(crate) fn axes(&self, _: &mut CallContext) -> WebIdlResult<Vec<f64>> {
        Ok(self.axes.borrow().clone())
    }

    pub(crate) fn buttons<'vm>(&self, cx: &mut CallContext<'vm>) -> WebIdlResult<MarkedVec<'vm, Ref<GamepadButton>>> {
        let buttons = MarkedVec::with_capacity(cx.vm(), self.buttons.len());
        buttons.extend_from_slice(&self.buttons);
        Ok(buttons)
    }

    pub(crate) fn vibration_actuator(&self, _: &mut CallContext) -> WebIdlResult<Ref<GamepadHapticActuator>> {
        Ok(self
            .vibration_actuator
            .get()
            .expect("a gamepad has a vibration actuator once it is created"))
    }

    // https://w3c.github.io/gamepad/#dfn-map-and-normalize-axes
    fn map_and_normalize_axes(&self) {
        // 1. Let axisValues be a list of unsigned long values representing the most recent logical axis input values for
        //    each axis input of the device represented by gamepad.
        // NOTE: While the Gamepad API internally uses u32 to represent raw axis values, SDL uses i16 for axes.
        let latest_state = self.latest_state.borrow();
        let axis_values = &latest_state.axis_values;
        let mut axes = self.axes.borrow_mut();

        // 2. Let maxRawAxisIndex be the size of axisValues − 1.
        // 3. For each rawAxisIndex of the range from 0 to maxRawAxisIndex:
        for (raw_axis_index, &logical_value) in axis_values.iter().enumerate().take(self.axis_minimums.len()) {
            // 1. Let mappedIndex be gamepad.[[axisMapping]][rawAxisIndex].
            let mapped_index = self.axis_mapping[raw_axis_index];

            // 2. Let logicalValue be axisValues[rawAxisIndex].

            // 3. Let logicalMinimum be gamepad.[[axisMinimums]][rawAxisIndex].
            let logical_minimum = self.axis_minimums[raw_axis_index];

            // 4. Let logicalMaximum be gamepad.[[axisMaximums]][rawAxisIndex].
            let logical_maximum = self.axis_maximums[raw_axis_index];

            // 5. Let normalizedValue be 2 (logicalValue − logicalMinimum) / (logicalMaximum − logicalMinimum) − 1.
            // SAFETY: The function only computes with its arguments.
            let normalized_value =
                unsafe { web_gamepad_normalize_axis_value(logical_value, logical_minimum, logical_maximum) };

            // 6. Set gamepad.[[axes]][axisIndex] to be normalizedValue.
            // FIXME: axisIndex should be mappedIndex.
            axes[mapped_index] = normalized_value;
        }
    }

    // https://w3c.github.io/gamepad/#dfn-map-and-normalize-buttons
    fn map_and_normalize_buttons(&self) {
        // 1. Let buttonValues be a list of unsigned long values representing the most recent logical button input values
        //    for each button input of the device represented by gamepad.
        // NOTE: While the Gamepad API internally uses u32 to represent raw button values, SDL uses bool for buttons and
        //       i16 for axes. The left and right triggers are buttons in the Gamepad API.
        let latest_state = self.latest_state.borrow();
        let button_values = &latest_state.button_values;

        // 2. Let maxRawButtonIndex be the size of buttonValues − 1.
        // 3. For each rawButtonIndex of the range from 0 to maxRawButtonIndex:
        for (raw_button_index, &logical_value) in button_values.iter().enumerate().take(self.button_minimums.len()) {
            // 1. Let mappedIndex be gamepad.[[buttonMapping]][rawButtonIndex].
            let mapped_index = self.button_mapping[raw_button_index];

            // 2. Let logicalValue be buttonValues[rawButtonIndex].

            // 3. Let logicalMinimum be gamepad.[[buttonMinimums]][rawButtonIndex].
            let logical_minimum = self.button_minimums[raw_button_index];

            // 4. Let logicalMaximum be gamepad.[[buttonMaximums]][rawButtonIndex].
            let logical_maximum = self.button_maximums[raw_button_index];

            // 5. Let normalizedValue be (logicalValue − logicalMinimum) / (logicalMaximum − logicalMinimum).
            // SAFETY: The function only computes with its arguments.
            let value = unsafe { web_gamepad_normalize_button_value(logical_value, logical_minimum, logical_maximum) };

            // 6. Let button be gamepad.[[buttons]][mappedIndex].
            let button = self.buttons[mapped_index];

            // 7. Set button.[[value]] to normalizedValue.
            button.set_value(value);

            // 8. If the button has a digital switch to indicate a pure pressed or released state, set button.[[pressed]]
            //    to true if the button is pressed or false if it is not pressed.
            //    Otherwise, set button.[[pressed]] to true if the value is above the button press threshold or false if
            //    it is not above the threshold.
            // SAFETY: The function only computes with its arguments.
            button
                .set_pressed(unsafe { web_gamepad_is_button_pressed(logical_value, logical_minimum, logical_maximum) });

            // 9. If the button is capable of detecting touch, set button.[[touched]] to true if the button is currently being touched.
            //    Otherwise, set button.[[touched]] to button.[[pressed]].
            // FIXME: Support the PS4/PS5 controller which has a touchpad, which is a button that can be touched and not pressed in at the same time.
            button.set_touched(button.is_pressed());
        }
    }

    // https://w3c.github.io/gamepad/#dfn-update-gamepad-state
    pub(crate) fn update_gamepad_state(this: Ref<Self>, vm: &Vm) {
        // 1. Let now be the current high resolution time given gamepad's relevant global object.
        let now = current_high_resolution_time(&this.window);
        let window = &this.window;

        // 2. Set gamepad.[[timestamp]] to now.
        this.timestamp.set(now);

        // 3. Run the steps to map and normalize axes for gamepad.
        this.map_and_normalize_axes();

        // 4. Run the steps to map and normalize buttons for gamepad.
        this.map_and_normalize_buttons();

        // FIXME: 5. Run the steps to record touches for gamepad.

        // 6. Let navigator be gamepad's relevant global object's Navigator object.
        let navigator = navigator_gamepad(vm, window);

        // 7. Let document be gamepad's relevant global object's associated Document; otherwise null.
        // 8. If document is null, abort these steps.
        // NB: A Window always has an associated Document.
        let document = window.associated_document();

        // AD-HOC: https://github.com/w3c/gamepad/pull/230 proposes a step here: "If document is a fully active descendant of
        //         a top-level traversable with user attention and gamepad contains a gamepad user gesture, then run the
        //         activation notification steps given document." The UI process runs it.

        // 9. If navigator.[[hasGamepadGesture]] is false and document is a fully active descendant of a top-level
        //    traversable with user attention and gamepad contains a gamepad user gesture:
        if !navigator.has_gamepad_gesture()
            && document.is_fully_active_descendant_of_a_top_level_traversable_with_user_attention()
            && this.contains_gamepad_user_gesture()
        {
            // 1. Set navigator.[[hasGamepadGesture]] to true.
            navigator.set_has_gamepad_gesture(true);

            // 2. For each connectedGamepad of navigator.[[gamepads]]:
            let gamepads = navigator.gamepads(vm);
            for connected_gamepad in gamepads.values().iter() {
                // 1. If connectedGamepad is not equal to null:
                let Some(connected_gamepad) = connected_gamepad else {
                    continue;
                };

                // 1. Set connectedGamepad.[[exposed]] to true.
                connected_gamepad.exposed.set(true);

                // 2. Set connectedGamepad.[[timestamp]] to now.
                connected_gamepad.timestamp.set(now);

                // 3. If document is not null and is fully active, then queue a global task on the gamepad task source
                //    to fire an event named gamepadconnected at gamepad's relevant global object using GamepadEvent
                //    with its gamepad attribute initialized to connectedGamepad.
                if document.is_fully_active() {
                    fire_a_gamepad_event(vm, window, "gamepadconnected", *connected_gamepad);
                }
            }
        }
    }

    // https://w3c.github.io/gamepad/#dfn-gamepad-user-gesture
    fn contains_gamepad_user_gesture(&self) -> bool {
        // NB: The UI process uses the same algorithm to decide when to tell this process about its gamepads.
        let latest_state = self.latest_state.borrow();
        let description = self.description.to_abi();
        let state = latest_state.to_abi();
        // SAFETY: The description and the state are lent for the call.
        unsafe { web_gamepad_contains_gamepad_user_gesture(&raw const description, &raw const state) }
    }
}
