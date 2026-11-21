/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AnimationEffect, https://www.w3.org/TR/web-animations-1/#the-animationeffect-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
pub(crate) use libcompositing_rust::animation_timing::{FillMode, PlaybackDirection};
use libcompositing_rust::animation_timing::{SpecifiedIterationDuration, TimeValue};
use libjs_rust::api::{Gc, Realm, Trace, Visitor};
use libweb_rust::AnimationRecordRef;
use libweb_rust::web_animations::{EffectRecordRef, ParsedEasing, parse_easing_function};

use super::animation::{
    animation_relevant_realm, current_time_for_observation, effect_timing_changed, update_style_if_needed,
};
use super::animation_playback_event::time_value_as_css_numberish;
use super::keyframe_effect::KeyframeEffect;
use crate::bindings::conversions::ToJs;
use crate::bindings::interface::Wrappable;
use crate::bindings::interface::{Inherits, Ref, cell_from_abi, cell_into_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::css::css_numeric_value::{AnyCSSNumericValue, CSSNumberish};
use core::ops::Deref;
use libjs_rust::api::{ThrowCompletionOr, Value, Vm};

unsafe extern "C" {
    fn web_animation_effect_visit_host_cells(effect: *const c_void, visitor: *mut c_void);
}

/// The union (unrestricted double or CSSNumericValue or DOMString) of the duration member of EffectTiming.
pub(crate) enum UnrestrictedDoubleOrCSSNumericValueOrDOMString {
    UnrestrictedDouble(f64),
    CSSNumericValue(AnyCSSNumericValue),
    DOMString(Utf16String),
}

/// The union (unrestricted double or DOMString) of the duration member of OptionalEffectTiming.
pub(crate) enum UnrestrictedDoubleOrDOMString {
    UnrestrictedDouble(f64),
    DOMString(Utf16String),
}

// https://www.w3.org/TR/web-animations-1/#dictdef-effecttiming
pub(crate) struct EffectTiming {
    pub(crate) delay: f64,
    pub(crate) direction: PlaybackDirection,
    pub(crate) duration: UnrestrictedDoubleOrCSSNumericValueOrDOMString,
    pub(crate) easing: Utf16String,
    pub(crate) end_delay: f64,
    pub(crate) fill: FillMode,
    pub(crate) iteration_start: f64,
    pub(crate) iterations: f64,
}

// https://www.w3.org/TR/web-animations-1/#dictdef-computedeffecttiming
pub(crate) struct ComputedEffectTiming {
    pub(crate) parent: EffectTiming,
    pub(crate) active_duration: Option<CSSNumberish>,
    pub(crate) current_iteration: Option<Option<f64>>,
    pub(crate) end_time: Option<CSSNumberish>,
    pub(crate) local_time: Option<Option<CSSNumberish>>,
    pub(crate) progress: Option<Option<f64>>,
}

// https://www.w3.org/TR/web-animations-1/#dictdef-optionaleffecttiming
#[derive(Default)]
pub(crate) struct OptionalEffectTiming {
    pub(crate) delay: Option<f64>,
    pub(crate) direction: Option<PlaybackDirection>,
    pub(crate) duration: Option<UnrestrictedDoubleOrDOMString>,
    pub(crate) easing: Option<Utf16String>,
    pub(crate) end_delay: Option<f64>,
    pub(crate) fill: Option<FillMode>,
    pub(crate) iteration_start: Option<f64>,
    pub(crate) iterations: Option<f64>,
}

/// Whether the UTF-16 code units are the ASCII text.
pub(crate) fn units_equal(units: &[u16], text: &str) -> bool {
    units.iter().copied().eq(text.encode_utf16())
}

// https://www.w3.org/TR/web-animations-1/#the-animationeffect-interface
pub(crate) struct AnimationEffect {
    record: EffectRecordRef,
}

// SAFETY: Visits the cells of the host that keep the record's associated animation and target element alive, as the
//         host visits them.
unsafe impl Trace for AnimationEffect {
    fn trace(&self, visitor: &mut Visitor) {
        // SAFETY: The effect holds a reference to the record, and the visitor is one LibGC passed in.
        unsafe { web_animation_effect_visit_host_cells(self.record.as_host_ptr(), visitor.as_raw().cast()) };
    }
}

impl Wrappable for AnimationEffect {
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        self.relevant_realm()
    }
}

impl AnimationEffect {
    /// The part as an AnimationEffect of the object of the effect whose state `record` holds.
    pub(crate) fn from_record(record: EffectRecordRef) -> Self {
        Self { record }
    }

    /// The effect's state, which lives as long as the effect.
    pub(crate) fn record(&self) -> &EffectRecordRef {
        &self.record
    }

    /// The realm of the relevant global object of the associated animation, if there is one.
    pub(crate) fn relevant_realm(&self) -> Option<Gc<Realm>> {
        animation_relevant_realm(&self.record.associated_animation()?)
    }

    pub(crate) fn update_style_if_needed(&self) {
        if let Some(animation) = self.record.associated_animation() {
            update_style_if_needed(&animation);
        }
    }

    fn associated_animation(&self) -> Option<AnimationRecordRef> {
        self.record.associated_animation()
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animationeffect-gettiming
    pub(crate) fn get_timing(&self, _cx: &mut CallContext) -> WebIdlResult<EffectTiming> {
        self.update_style_if_needed();

        // Returns the specified timing properties for this animation effect.
        let specified = self.record.specified_timing();
        let timing = self.record.timing();
        Ok(EffectTiming {
            delay: specified.start_delay,
            direction: timing.playback_direction,
            duration: if specified.iteration_duration.is_auto {
                UnrestrictedDoubleOrCSSNumericValueOrDOMString::DOMString(Utf16String::from_ascii(b"auto"))
            } else {
                UnrestrictedDoubleOrCSSNumericValueOrDOMString::UnrestrictedDouble(
                    specified.iteration_duration.milliseconds,
                )
            },
            easing: self.record.timing_function_serialization(),
            end_delay: specified.end_delay,
            fill: timing.fill_mode,
            iteration_start: specified.iteration_start,
            iterations: specified.iteration_count,
        })
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animationeffect-getcomputedtiming
    // https://drafts.csswg.org/web-animations-2/#dom-animationeffect-getcomputedtiming
    pub(crate) fn get_computed_timing(&self, cx: &mut CallContext) -> WebIdlResult<ComputedEffectTiming> {
        let vm = cx.vm();
        self.update_style_if_needed();

        // NB: The local time is the current time of the associated animation as script observes it.
        let local_time = self
            .associated_animation()
            .and_then(|animation| current_time_for_observation(&animation));

        // Returns the calculated timing properties for this animation effect.

        // Although some of the attributes of the object returned by getTiming() and getComputedTiming() are common,
        // their values can differ in the following ways:

        //     - duration: while getTiming() can return the string auto, getComputedTiming() must return a number
        //       corresponding to the calculated value of the iteration duration as defined in the description of the
        //       duration member of the EffectTiming interface.
        //
        // https://drafts.csswg.org/web-animations-2/#dom-animationeffect-getcomputedtiming
        // The description of the duration attribute of the object needs to indicate that if timing.duration is the
        // string auto, this attribute will return the current calculated value of the intrinsic iteration duration,
        // which may be a expressed as a double representing the duration in milliseconds or a percentage when the
        // effect is associated with a progress-based timeline.
        let timing_properties = self.record.timing();
        let duration = match time_value_as_css_numberish(vm, timing_properties.iteration_duration) {
            CSSNumberish::Double(value) => UnrestrictedDoubleOrCSSNumericValueOrDOMString::UnrestrictedDouble(value),
            CSSNumberish::CSSNumericValue(value) => {
                UnrestrictedDoubleOrCSSNumericValueOrDOMString::CSSNumericValue(value)
            }
        };

        //     - fill: likewise, while getTiming() can return the string auto, getComputedTiming() must return the
        //       specific FillMode used for timing calculations as defined in the description of the fill member of the
        //       EffectTiming interface.
        //
        //       In this level of the specification, that simply means that an auto value is replaced by the none
        //       FillMode.
        let specified_fill_mode = timing_properties.fill_mode;
        let fill = if specified_fill_mode == FillMode::Auto {
            FillMode::None
        } else {
            specified_fill_mode
        };

        let specified = self.record.specified_timing();
        let timing = self.record.resolve_timing(local_time, specified_fill_mode, true);
        let as_css_numberish = |time: TimeValue| time_value_as_css_numberish(vm, time);
        Ok(ComputedEffectTiming {
            parent: EffectTiming {
                delay: specified.start_delay,
                direction: timing_properties.playback_direction,
                duration,
                easing: self.record.timing_function_serialization(),
                end_delay: specified.end_delay,
                fill,
                iteration_start: timing_properties.iteration_start,
                iterations: timing_properties.iteration_count,
            },
            active_duration: Some(as_css_numberish(timing.active_duration)),
            current_iteration: Some(timing.current_iteration),
            end_time: Some(as_css_numberish(timing.end_time)),
            local_time: Some(local_time.map(as_css_numberish)),
            progress: Some(timing.transformed_progress),
        })
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animationeffect-updatetiming
    pub(crate) fn update_timing(&self, _cx: &mut CallContext, timing: OptionalEffectTiming) -> WebIdlResult<()> {
        self.update_the_timing_properties(timing)
    }

    // https://www.w3.org/TR/web-animations-1/#update-the-timing-properties-of-an-animation-effect
    // https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
    pub(crate) fn update_the_timing_properties(&self, timing: OptionalEffectTiming) -> WebIdlResult<()> {
        // 1. If the iterationStart member of input exists and is less than zero, throw a TypeError and abort this
        //    procedure.
        if timing
            .iteration_start
            .is_some_and(|iteration_start| iteration_start < 0.0)
        {
            return Err(Exception::type_error("Invalid iteration start value"));
        }

        // 2. If the iterations member of input exists, and is less than zero or is the value NaN, throw a TypeError
        //    and abort this procedure.
        if timing
            .iterations
            .is_some_and(|iterations| iterations < 0.0 || iterations.is_nan())
        {
            return Err(Exception::type_error("Invalid iteration count value"));
        }

        // 3. If the duration member of input exists, and is less than zero, is the value NaN, or is a string other than
        //    the lowercase auto, throw a TypeError and abort this procedure.
        let has_valid_duration_value = match &timing.duration {
            None => true,
            Some(UnrestrictedDoubleOrDOMString::UnrestrictedDouble(duration)) => {
                !(*duration < 0.0 || duration.is_nan())
            }
            Some(UnrestrictedDoubleOrDOMString::DOMString(duration)) => units_equal(&duration.to_utf16(), "auto"),
        };
        if !has_valid_duration_value {
            return Err(Exception::type_error("Invalid duration value"));
        }

        // 4. If the easing member of input exists but cannot be parsed using the <easing-function> production
        //    [CSS-EASING-1], throw a TypeError and abort this procedure.
        let easing_value: Option<ParsedEasing> = match &timing.easing {
            Some(easing) => {
                Some(parse_easing_function(easing).ok_or_else(|| Exception::type_error("Invalid easing function"))?)
            }
            None => None,
        };

        // 5. Assign each member that exists in input to the corresponding timing property of effect as follows:
        let mut specified = self.record.specified_timing();

        //    - delay → specified start delay
        if let Some(delay) = timing.delay {
            specified.start_delay = delay;
        }

        //    - endDelay → specified end delay
        if let Some(end_delay) = timing.end_delay {
            specified.end_delay = end_delay;
        }

        //    - fill → fill mode
        if let Some(fill) = timing.fill {
            specified.fill_mode = fill;
        }

        //    - iterationStart → iteration start
        if let Some(iteration_start) = timing.iteration_start {
            specified.iteration_start = iteration_start;
        }

        //    - iterations → iteration count
        if let Some(iterations) = timing.iterations {
            specified.iteration_count = iterations;
        }

        //    - duration → specified iteration duration
        // NB: "auto" is the only string an iteration duration can be.
        match timing.duration {
            Some(UnrestrictedDoubleOrDOMString::UnrestrictedDouble(duration)) => {
                specified.iteration_duration = SpecifiedIterationDuration::milliseconds(duration);
            }
            Some(UnrestrictedDoubleOrDOMString::DOMString(_)) => {
                specified.iteration_duration = SpecifiedIterationDuration::AUTO;
            }
            None => {}
        }

        //    - direction → playback direction
        if let Some(direction) = timing.direction {
            specified.playback_direction = direction;
        }
        self.record.set_specified_timing(specified);

        //    - easing → timing function
        if let Some(easing_value) = &easing_value {
            self.record.set_timing_function(easing_value);
        }

        // 6. Follow the procedure to normalize specified timing.
        self.record.normalize_specified_timing();

        // AD-HOC: Notify the associated animation that the effect timing has changed.
        if let Some(animation) = self.associated_animation() {
            effect_timing_changed(&animation);
        }

        Ok(())
    }
}

/// An EffectTiming as the OptionalEffectTiming with each of its members, which the procedure to update the timing
/// properties of an animation effect takes; none of its durations is a CSSNumericValue.
pub(crate) fn to_optional_effect_timing(effect_timing: EffectTiming) -> OptionalEffectTiming {
    OptionalEffectTiming {
        delay: Some(effect_timing.delay),
        direction: Some(effect_timing.direction),
        duration: Some(match effect_timing.duration {
            UnrestrictedDoubleOrCSSNumericValueOrDOMString::UnrestrictedDouble(value) => {
                UnrestrictedDoubleOrDOMString::UnrestrictedDouble(value)
            }
            UnrestrictedDoubleOrCSSNumericValueOrDOMString::DOMString(value) => {
                UnrestrictedDoubleOrDOMString::DOMString(value)
            }
            // NB: We check that this isn't the case in the caller
            UnrestrictedDoubleOrCSSNumericValueOrDOMString::CSSNumericValue(_) => {
                unreachable!("the caller rejects a CSSNumericValue duration")
            }
        }),
        easing: Some(effect_timing.easing),
        end_delay: Some(effect_timing.end_delay),
        fill: Some(effect_timing.fill),
        iteration_start: Some(effect_timing.iteration_start),
        iterations: Some(effect_timing.iterations),
    }
}

/// An object script sees of an effect, of the interface that inherits from AnimationEffect it is of.
#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
#[allow(clippy::enum_variant_names, reason = "the variants have the interfaces’ names")]
pub(crate) enum AnyAnimationEffect {
    AnimationEffect(Ref<AnimationEffect>),
    KeyframeEffect(Ref<KeyframeEffect>),
}

impl Deref for AnyAnimationEffect {
    type Target = AnimationEffect;

    fn deref(&self) -> &AnimationEffect {
        match self {
            Self::AnimationEffect(effect) => effect,
            Self::KeyframeEffect(effect) => effect.parent(),
        }
    }
}

impl ToJs for AnyAnimationEffect {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        match self {
            Self::AnimationEffect(effect) => effect.to_js(cx),
            Self::KeyframeEffect(effect) => effect.to_js(cx),
        }
    }
}

impl AnyAnimationEffect {
    /// The bytes the object of the effect holds.
    pub(crate) fn memory_size(&self) -> usize {
        match self {
            Self::AnimationEffect(_) => size_of::<AnimationEffect>(),
            Self::KeyframeEffect(effect) => effect.memory_size(),
        }
    }
}

/// The object of the effect, if it has one.
pub(crate) fn existing_effect_object(effect: &EffectRecordRef) -> Option<AnyAnimationEffect> {
    let cell = effect.object_cell()?;
    // SAFETY: The object cell of an effect is the live cell of its object, a KeyframeEffect, the only AnimationEffect there
    //         is.
    Some(AnyAnimationEffect::KeyframeEffect(unsafe {
        cell_from_abi(cell.as_ptr())
    }))
}

/// The object script sees of the effect, a KeyframeEffect, which the effect of a CSS animation or transition gets as
/// script first asks for it.
pub(crate) fn effect_object(vm: &Vm, effect: &EffectRecordRef) -> AnyAnimationEffect {
    if let Some(object) = existing_effect_object(effect) {
        return object;
    }
    let object = KeyframeEffect::create_for_record(vm, effect.clone());
    effect.set_object_cell(NonNull::new(cell_into_abi(object)));
    AnyAnimationEffect::KeyframeEffect(object)
}
