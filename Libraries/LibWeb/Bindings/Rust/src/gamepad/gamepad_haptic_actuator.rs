/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GamepadHapticActuator, https://w3c.github.io/gamepad/#gamepadhapticactuator-interface

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Object, Realm, Trace, Vm};

use crate::bindings::conversions::ToJs;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::global_object::{CppWindow, window_from_global_object};
use crate::bindings::interface::{
    Ref, Wrappable, allocate, cell_from_abi, with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, deferred_invoke, promise_realm, queue_a_global_task, reject_promise_with_exception,
    resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::gamepad::gamepad::Gamepad;
use crate::html::visibility_state::VisibilityState;
use crate::platform::timer::CppTimer;

unsafe extern "C" {
    fn web_gamepad_play_effect(
        window: *mut c_void,
        handle: u32,
        effect_type: u8,
        first_magnitude: u16,
        second_magnitude: u16,
        duration: u32,
    );
    fn web_gamepad_stop_effects(window: *mut c_void, handle: u32);
}

/// The enumeration GamepadHapticsResult of Gamepad/GamepadHapticActuator.idl.
#[derive(Clone, Copy, PartialEq, Eq, Trace)]
pub(crate) enum GamepadHapticsResult {
    Complete,
    Preempted,
}

impl GamepadHapticsResult {
    fn as_str(self) -> &'static str {
        match self {
            Self::Complete => "complete",
            Self::Preempted => "preempted",
        }
    }
}

/// The enumeration GamepadHapticEffectType of Gamepad/GamepadHapticActuator.idl.
#[derive(Clone, Copy, PartialEq, Eq, Trace)]
pub(crate) enum GamepadHapticEffectType {
    DualRumble,
    TriggerRumble,
}

/// The dictionary GamepadEffectParameters of Gamepad/GamepadHapticActuator.idl.
#[derive(Clone, Copy, Default, Trace)]
pub(crate) struct GamepadEffectParameters {
    pub(crate) duration: u64,
    pub(crate) start_delay: u64,
    pub(crate) strong_magnitude: f64,
    pub(crate) weak_magnitude: f64,
    pub(crate) left_trigger: f64,
    pub(crate) right_trigger: f64,
}

#[derive(Clone, Trace)]
struct HapticsCompletionSteps {
    promise: WebIdlPromise,
    queue_on: Option<Gc<Object>>,
}

fn resolve_haptics_promise(vm: &Vm, promise: WebIdlPromise, result: GamepadHapticsResult) {
    let realm = promise_realm(promise);
    with_temporary_execution_context_and_callbacks(realm, || {
        let result = Utf16String::from_ascii(result.as_str().as_bytes())
            .to_js(&CallContext::new(vm, realm))
            .expect("a string converts to a JS value");
        resolve_promise(vm, promise, result);
    });
}

impl HapticsCompletionSteps {
    fn run(self, vm: &Vm, result: GamepadHapticsResult) {
        match self.queue_on {
            Some(global) => {
                queue_a_global_task(
                    vm,
                    global,
                    TaskSource::Gamepad,
                    (self.promise, result),
                    |vm, (promise, result)| resolve_haptics_promise(vm, promise, result),
                );
            }
            None => resolve_haptics_promise(vm, self.promise, result),
        }
    }
}

// FIXME: What is a valid duration and startDelay? The spec doesn't define that.
//        Safari: clamps any duration above 5000ms to 5000ms and doesn't seem to clamp or reject any startDelay.
//        Chrome: rejects if duration + startDelay > 5000ms.
//        Firefox doesn't support vibration at the time of writing.
const MAX_VIBRATION_DURATION: u64 = 5000;

// https://w3c.github.io/gamepad/#gamepadhapticactuator-interface
#[derive(Trace)]
pub(crate) struct GamepadHapticActuator {
    gamepad: Ref<Gamepad>,
    window: CppWindow,
    document_became_hidden_observer: GcRefCell<Option<CppCell>>,

    // https://w3c.github.io/gamepad/#dfn-effects
    // Represents the effects supported by the actuator.
    #[gc(untraced)]
    effects: Vec<GamepadHapticEffectType>,

    // https://w3c.github.io/gamepad/#dfn-playingeffectpromise
    // The completion steps for playing some effect, or null if no effect is playing.
    playing_effect_completion_steps: GcRefCell<Option<HapticsCompletionSteps>>,
    playing_effect_timer: GcRefCell<Option<CppTimer>>,
}

impl Wrappable for GamepadHapticActuator {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.window.realm())
    }
}

unsafe extern "C" fn document_visibility_changed(vm: *mut c_void, actuator: *mut c_void, visible: bool) {
    // SAFETY: LibWeb passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    // SAFETY: The observer holds the actuator, whose cell it was given.
    let actuator: Ref<GamepadHapticActuator> = unsafe { cell_from_abi(actuator) };
    if !visible {
        GamepadHapticActuator::document_became_hidden(actuator, vm);
    }
}

fn gamepad_effect_magnitudes(r#type: GamepadHapticEffectType, params: &GamepadEffectParameters) -> (f64, f64) {
    match r#type {
        GamepadHapticEffectType::DualRumble => (params.strong_magnitude, params.weak_magnitude),
        GamepadHapticEffectType::TriggerRumble => (params.left_trigger, params.right_trigger),
    }
}

fn magnitude_to_u16(magnitude: f64) -> u16 {
    #[allow(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "a valid magnitude is in the range [0 .. 1]"
    )]
    let magnitude = (magnitude * f64::from(u16::MAX)) as u16;
    magnitude
}

impl GamepadHapticActuator {
    // https://w3c.github.io/gamepad/#dfn-constructing-a-gamepadhapticactuator
    pub(crate) fn create(vm: &Vm, window: &CppWindow, gamepad: Ref<Gamepad>) -> Ref<Self> {
        // 1. Let gamepadHapticActuator be a newly created GamepadHapticActuator instance.
        // 2. Let supportedEffectsList be an empty list.
        let mut supported_effects_list = Vec::new();

        // 3. For each enum value type of GamepadHapticEffectType, if the user agent can send a command to initiate effects
        //    of that type on that actuator, append type to supportedEffectsList.
        if gamepad.description().supports_dual_rumble {
            supported_effects_list.push(GamepadHapticEffectType::DualRumble);
        }

        if gamepad.description().supports_trigger_rumble {
            supported_effects_list.push(GamepadHapticEffectType::TriggerRumble);
        }

        // 4. Set gamepadHapticActuator.[[effects]] to supportedEffectsList.
        let gamepad_haptic_actuator = allocate(vm, |_| Self {
            gamepad,
            window: window.clone(),
            document_became_hidden_observer: GcRefCell::new(None),
            effects: supported_effects_list,
            playing_effect_completion_steps: GcRefCell::new(None),
            playing_effect_timer: GcRefCell::new(None),
        });

        let observer = window
            .associated_document()
            .observe_visibility_state(gamepad_haptic_actuator.as_ptr().cast(), document_visibility_changed);
        *gamepad_haptic_actuator.document_became_hidden_observer.borrow_mut(vm) = Some(observer);

        gamepad_haptic_actuator
    }

    pub(crate) fn effects(&self, _: &mut CallContext) -> WebIdlResult<Vec<GamepadHapticEffectType>> {
        Ok(self.effects.clone())
    }

    // https://w3c.github.io/gamepad/#dfn-valid-effect
    fn is_valid_effect(r#type: GamepadHapticEffectType, params: &GamepadEffectParameters) -> bool {
        let is_in_unit_range = |value: f64| (0.0..=1.0).contains(&value);

        // 1. Given the value of GamepadHapticEffectType type, switch on:
        //    "dual-rumble"
        //          If params does not describe a valid dual-rumble effect, return false.
        //    "trigger-rumble"
        //          If params does not describe a valid trigger-rumble effect, return false.
        // 2. Return true
        match r#type {
            GamepadHapticEffectType::DualRumble => {
                // https://w3c.github.io/gamepad/#dfn-valid-dual-rumble-effect
                // Given GamepadEffectParameters params, a valid dual-rumble effect must have a valid duration, a valid
                // startDelay, and both the strongMagnitude and the weakMagnitude must be in the range [0 .. 1].
                let Some(total_duration) = params.duration.checked_add(params.start_delay) else {
                    return false;
                };

                if total_duration > MAX_VIBRATION_DURATION {
                    return false;
                }

                if !is_in_unit_range(params.strong_magnitude) {
                    return false;
                }

                if !is_in_unit_range(params.weak_magnitude) {
                    return false;
                }

                true
            }
            GamepadHapticEffectType::TriggerRumble => {
                // https://w3c.github.io/gamepad/#dfn-valid-trigger-rumble-effect
                // Given GamepadEffectParameters params, a valid trigger-rumble effect must have a valid duration, a valid
                // startDelay, and the strongMagnitude, weakMagnitude, leftTrigger, and rightTrigger must be in the range
                // [0 .. 1].
                let Some(total_duration) = params.duration.checked_add(params.start_delay) else {
                    return false;
                };

                if total_duration > MAX_VIBRATION_DURATION {
                    return false;
                }

                if !is_in_unit_range(params.strong_magnitude) {
                    return false;
                }

                if !is_in_unit_range(params.weak_magnitude) {
                    return false;
                }

                if !is_in_unit_range(params.left_trigger) {
                    return false;
                }

                if !is_in_unit_range(params.right_trigger) {
                    return false;
                }

                true
            }
        }
    }

    fn can_play_effect_with_type(&self, r#type: GamepadHapticEffectType) -> bool {
        // https://w3c.github.io/gamepad/#ref-for-dfn-play-effects-with-type-1
        // A GamepadHapticActuator can play effects with type type if type can be found in the [[effects]] list.
        self.effects.contains(&r#type)
    }

    fn current_document_is_hidden_or_inactive(vm: &Vm) -> bool {
        let realm = vm.current_realm().expect("a binding runs in a realm");
        let window =
            window_from_global_object(realm.global_object()).expect("GamepadHapticActuator is only exposed on Window");
        let document = window.associated_document();
        !document.is_fully_active() || document.visibility_state() == VisibilityState::Hidden
    }

    // https://w3c.github.io/gamepad/#dom-gamepadhapticactuator-playeffect
    pub(crate) fn play_effect(
        this: &Ref<Self>,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        r#type: GamepadHapticEffectType,
        params: GamepadEffectParameters,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. If params does not describe a valid effect of type type, return a promise rejected with a TypeError.
        if !Self::is_valid_effect(r#type, &params) {
            reject_promise_with_exception(vm, promise, Exception::type_error("Invalid effect"));
            return Ok(());
        }

        // 2. Let document be the current settings object's relevant global object's associated Document.
        // 3. If document is null or document is not fully active or document's visibility state is "hidden", return a
        //    promise rejected with an "InvalidStateError" DOMException.
        if Self::current_document_is_hidden_or_inactive(vm) {
            reject_promise_with_exception(
                vm,
                promise,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_utf8("Haptics are not allowed in a hidden document"),
                ),
            );
            return Ok(());
        }

        // 5. If this GamepadHapticActuator cannot play effects with type type, return a promise rejected with reason
        //    NotSupportedError.
        if !this.can_play_effect_with_type(r#type) {
            reject_promise_with_exception(
                vm,
                promise,
                Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_utf8("Gamepad does not support this effect"),
                ),
            );
            return Ok(());
        }

        let completion_steps = HapticsCompletionSteps {
            promise,
            queue_on: Some(promise_realm(promise).global_object()),
        };
        Self::play_effect_with_completion_steps(*this, vm, r#type, params, completion_steps);
        Ok(())
    }

    // https://w3c.github.io/gamepad/#dom-gamepadhapticactuator-reset
    pub(crate) fn reset(this: &Ref<Self>, cx: &mut CallContext, promise: WebIdlPromise) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let document be the current settings object's relevant global object's associated Document.
        // 2. If document is null or document is not fully active or document's visibility state is "hidden", return a
        //    promise rejected with an "InvalidStateError" DOMException.
        if Self::current_document_is_hidden_or_inactive(vm) {
            reject_promise_with_exception(
                vm,
                promise,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_utf8("Haptics are not allowed in a hidden document"),
                ),
            );
            return Ok(());
        }

        Self::reset_with_completion_steps(
            *this,
            vm,
            HapticsCompletionSteps {
                promise,
                queue_on: None,
            },
        );
        Ok(())
    }

    // https://w3c.github.io/gamepad/#dom-gamepadhapticactuator-playeffect
    fn play_effect_with_completion_steps(
        this: Ref<Self>,
        vm: &Vm,
        r#type: GamepadHapticEffectType,
        params: GamepadEffectParameters,
        completion_steps: HapticsCompletionSteps,
    ) {
        // 4. If this.[[playingEffectPromise]] is not null:
        let playing_effect_completion_steps = this.playing_effect_completion_steps.borrow().clone();
        if let Some(effect_completion_steps) = playing_effect_completion_steps {
            // 1. Let effectPromise be this.[[playingEffectPromise]].
            // 2. Set this.[[playingEffectPromise]] to null.
            *this.playing_effect_completion_steps.borrow_mut(vm) = None;
            this.clear_playing_effect_timers(vm);

            // 3. Queue a global task on the gamepad task source with the relevant global object of this to resolve
            //    effectPromise with "preempted".
            effect_completion_steps.run(vm, GamepadHapticsResult::Preempted);
        }

        // 6. Let [[playingEffectPromise]] be a new promise.
        *this.playing_effect_completion_steps.borrow_mut(vm) = Some(completion_steps);

        // 7. Let playEffectTimestamp be the current high resolution time given the document's relevant global object.
        // NOTE: Unused.

        // 8. Do the following steps in parallel:
        deferred_invoke(vm, (this, r#type, params), |vm, (this, r#type, params)| {
            // 1. Issue a haptic effect to the actuator with type, params, and the playEffectTimestamp.
            Self::issue_haptic_effect(this, vm, r#type, params);
        });

        // 9. Return [[playingEffectPromise]].
    }

    fn effect_completed(this: Ref<Self>, vm: &Vm) {
        // 2. When the effect completes, if this.[[playingEffectPromise]] is not null, queue a global task on the
        //    gamepad task source with the relevant global object of this to run the following steps:
        let playing_effect_completion_steps = this.playing_effect_completion_steps.borrow().clone();
        if let Some(completion_steps) = playing_effect_completion_steps {
            // 2. Resolve this.[[playingEffectPromise]] with "complete".
            completion_steps.run(vm, GamepadHapticsResult::Complete);

            // 3. Set this.[[playingEffectPromise]] to null.
            *this.playing_effect_completion_steps.borrow_mut(vm) = None;
            this.clear_playing_effect_timers(vm);
        }
    }

    fn reset_with_completion_steps(this: Ref<Self>, vm: &Vm, reset_completion_steps: HapticsCompletionSteps) {
        // 4. If this.[[playingEffectPromise]] is not null, do the following steps in parallel:
        if this.playing_effect_completion_steps.borrow().is_none() {
            return;
        }

        deferred_invoke(
            vm,
            (this, reset_completion_steps),
            |vm, (this, reset_completion_steps)| {
                // 1. Let effectPromise be this.[[playingEffectPromise]].
                let effect_completion_steps = this.playing_effect_completion_steps.borrow().clone();

                // 2. Stop haptic effects on this's gamepad's actuator.
                this.stop_haptic_effects();

                // 3. If the effect has been successfully stopped, do:
                // AD-HOC: The stop command is sent to the UI process without waiting for a reply, so it is taken to have
                //         succeeded, matching other engines.
                //    1. If effectPromise and this.[[playingEffectPromise]] are still the same,
                //       set this.[[playingEffectPromise]] to null.
                let playing_effect_promise = this
                    .playing_effect_completion_steps
                    .borrow()
                    .as_ref()
                    .map(|steps| steps.promise);
                if effect_completion_steps.as_ref().map(|steps| steps.promise) == playing_effect_promise {
                    *this.playing_effect_completion_steps.borrow_mut(vm) = None;
                }

                //    2. Queue a global task on the gamepad task source with the relevant global object of this to resolve
                //       effectPromise with "preempted".
                if let Some(effect_completion_steps) = effect_completion_steps {
                    effect_completion_steps.run(vm, GamepadHapticsResult::Preempted);
                }

                // 4. Resolve resetResultPromise with "complete"
                reset_completion_steps.run(vm, GamepadHapticsResult::Complete);
            },
        );
    }

    // https://w3c.github.io/gamepad/#handling-visibility-change
    fn document_became_hidden(this: Ref<Self>, vm: &Vm) {
        // When the document's visibility state becomes "hidden", run these steps for each GamepadHapticActuator actuator:
        // 1. If actuator.[[playingEffectPromise]] is null, abort these steps.
        let playing_effect_completion_steps = this.playing_effect_completion_steps.borrow().clone();
        let Some(completion_steps) = playing_effect_completion_steps else {
            return;
        };

        // 2. Queue a global task on the gamepad task source with the relevant global object of actuator to run the
        //    following steps:
        // 2. Resolve actuator.[[playingEffectPromise]] with "preempted".
        completion_steps.run(vm, GamepadHapticsResult::Preempted);

        // 3. Set actuator.[[playingEffectPromise]] to null.
        *this.playing_effect_completion_steps.borrow_mut(vm) = None;
        this.clear_playing_effect_timers(vm);

        // 3. Stop haptic effects on actuator.
        this.stop_haptic_effects();
    }

    // https://w3c.github.io/gamepad/#dfn-issue-a-haptic-effect
    fn issue_haptic_effect(this: Ref<Self>, vm: &Vm, r#type: GamepadHapticEffectType, params: GamepadEffectParameters) {
        // To issue a haptic effect on an actuator, the user agent MUST send a command to the device to render an effect
        // of type and try to make it use the provided params. The user agent SHOULD use the provided playEffectTimestamp
        // for more precise playback timing when params.startDelay is not 0.0. The user agent MAY modify the effect to
        // increase compatibility. For example, an effect intended for a rumble motor may be transformed into a
        // waveform-based effect for a device that supports waveform haptics but lacks rumble motors.
        let timer = CppTimer::create_single_shot(
            vm,
            params.start_delay,
            (this, r#type, params),
            |vm, (this, r#type, params)| {
                let (first_magnitude, second_magnitude) = gamepad_effect_magnitudes(r#type, &params);
                this.play_gamepad_effect(
                    r#type,
                    magnitude_to_u16(first_magnitude),
                    magnitude_to_u16(second_magnitude),
                    u32::try_from(params.duration).expect("a valid duration fits in 32 bits"),
                );

                let timer = CppTimer::create_single_shot(vm, params.duration, (this, r#type), |vm, (this, r#type)| {
                    // Explicitly stop the rumble before completing, ensuring the stop signal is sent synchronously.
                    this.play_gamepad_effect(r#type, 0, 0, 0);
                    Self::effect_completed(this, vm);
                });
                *this.playing_effect_timer.borrow_mut(vm) = Some(timer);
            },
        );
        *this.playing_effect_timer.borrow_mut(vm) = Some(timer);
    }

    fn play_gamepad_effect(
        &self,
        r#type: GamepadHapticEffectType,
        first_magnitude: u16,
        second_magnitude: u16,
        duration: u32,
    ) {
        // SAFETY: The Window is live.
        unsafe {
            web_gamepad_play_effect(
                self.window.as_ptr(),
                self.gamepad.handle(),
                r#type as u8,
                first_magnitude,
                second_magnitude,
                duration,
            );
        }
    }

    // https://w3c.github.io/gamepad/#dfn-stop-haptic-effects
    fn stop_haptic_effects(&self) {
        // To stop haptic effects on an actuator, the user agent MUST send a command to the device to abort any effects
        // currently being played. If a haptic effect was interrupted, the actuator SHOULD return to a motionless state
        // as quickly as possible.
        // SAFETY: The Window is live.
        unsafe { web_gamepad_stop_effects(self.window.as_ptr(), self.gamepad.handle()) };
    }

    fn clear_playing_effect_timers(&self, vm: &Vm) {
        if let Some(timer) = self.playing_effect_timer.borrow_mut(vm).take() {
            timer.stop();
        }
    }
}
