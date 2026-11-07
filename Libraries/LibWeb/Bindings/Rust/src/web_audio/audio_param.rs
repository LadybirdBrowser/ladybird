/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::web_audio::audio_param_timeline::{AudioParamTimeline, InsertResult};
use libweb_rust::web_audio::rendering::render_node::RenderAudioParam;

use super::audio_node::AnyAudioNode;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) use libweb_rust::web_audio::rendering::render_node::AutomationRate;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum FixedAutomationRate {
    No,
    Yes,
}

// https://webaudio.github.io/web-audio-api/#AudioParam
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioParam {
    context: AnyBaseAudioContext,

    // An AudioParam is a facet of the AudioNode that exposes it, so holding on to the parameter keeps that node, and
    // with it the node's render node and automation, alive.
    owner: Cell<Option<AnyAudioNode>>,
    #[gc(untraced)]
    timeline: Arc<AudioParamTimeline>,
    #[gc(untraced)]
    render_param: Arc<RenderAudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-current-value-slot
    default_value: f32,
    min_value: f32,
    max_value: f32,
    #[gc(untraced)]
    automation_rate: Cell<AutomationRate>,
    #[gc(untraced)]
    fixed_automation_rate: FixedAutomationRate,
}

impl Wrappable for AudioParam {}
fn clamp(value: f32, min: f32, max: f32) -> f32 {
    assert!(max >= min);
    if value > max {
        return max;
    }
    if value < min {
        return min;
    }
    value
}

fn max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

impl AudioParam {
    // The owner is the AudioNode this parameter belongs to, or null for AudioListener parameters.
    #[allow(clippy::too_many_arguments, reason = "preserve the C++ parameter initialization")]
    pub(crate) fn create(
        vm: &Vm,
        context: AnyBaseAudioContext,
        owner: Option<AnyAudioNode>,
        default_value: f32,
        min_value: f32,
        max_value: f32,
        automation_rate: AutomationRate,
        fixed_automation_rate: FixedAutomationRate,
    ) -> Ref<Self> {
        allocate(vm, |_| {
            let timeline = Arc::new(AudioParamTimeline::new(default_value));
            let render_param = Arc::new(RenderAudioParam::new(
                Arc::clone(&timeline),
                min_value,
                max_value,
                automation_rate,
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            ));
            Self {
                context,
                owner: Cell::new(owner),
                timeline,
                render_param,
                default_value,
                min_value,
                max_value,
                automation_rate: Cell::new(automation_rate),
                fixed_automation_rate,
            }
        })
    }

    pub(crate) fn set_owner(&self, owner: AnyAudioNode) {
        self.owner.set(Some(owner));
    }

    pub(crate) fn context_value(&self) -> AnyBaseAudioContext {
        self.context
    }

    pub(crate) fn render_param(&self) -> &Arc<RenderAudioParam> {
        &self.render_param
    }

    fn current_time(&self) -> f64 {
        self.context.current_time_value()
    }

    fn context_error(&self, vm: &Vm, name: &'static str, message: &[u8]) -> Exception {
        let realm = self.context.realm();
        Exception::Thrown(Exception::Dom(name, Utf16String::from_ascii(message)).throw(&CallContext::new(vm, realm)))
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-value
    // https://webaudio.github.io/web-audio-api/#simple-nominal-range
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        // Each AudioParam includes minValue and maxValue attributes that together form the simple nominal range
        // for the parameter. In effect, value of the parameter is clamped to the range [minValue, maxValue].
        Ok(clamp(self.timeline.current_value(), self.min_value, self.max_value))
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-value
    pub(crate) fn set_value(this: &Ref<Self>, cx: &mut CallContext, value: f32) -> WebIdlResult<()> {
        // Setting this attribute has the effect of assigning the requested value to the [[current value]] slot, and calling
        // the setValueAtTime() method with the current AudioContext's currentTime and [[current value]].
        this.timeline.set_current_value(value);
        Self::set_value_at_time(this, cx, value, this.current_time())?;
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#computedvalue
    pub(crate) fn intrinsic_value_at_time(&self, time: f64) -> f32 {
        self.timeline.value_at_time(time)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
    pub(crate) fn automation_rate(&self, _: &mut CallContext) -> WebIdlResult<AutomationRate> {
        Ok(self.automation_rate.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
    pub(crate) fn set_automation_rate(&self, _: &mut CallContext, automation_rate: AutomationRate) -> WebIdlResult<()> {
        if automation_rate != self.automation_rate.get() && self.fixed_automation_rate == FixedAutomationRate::Yes {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Automation rate cannot be changed"),
            ));
        }
        self.automation_rate.set(automation_rate);
        self.render_param.set_automation_rate(automation_rate);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-defaultvalue
    pub(crate) fn default_value(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.default_value)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-minvalue
    pub(crate) fn min_value(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.min_value)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-maxvalue
    pub(crate) fn max_value(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.max_value)
    }

    fn map_insert_result(&self, vm: &Vm, result: InsertResult) -> WebIdlResult<()> {
        match result {
            InsertResult::Success => Ok(()),
            InsertResult::EventContainedInValueCurve => Err(self.context_error(
                vm,
                "NotSupportedError",
                b"Cannot schedule an automation event during a value curve",
            )),
            InsertResult::ValueCurveOverlapsEvent => Err(self.context_error(
                vm,
                "NotSupportedError",
                b"Cannot schedule a value curve containing an automation event",
            )),
        }
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-setvalueattime
    pub(crate) fn set_value_at_time(
        this: &Ref<Self>,
        cx: &mut CallContext,
        value: f32,
        mut start_time: f64,
    ) -> WebIdlResult<Ref<Self>> {
        // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
        if start_time < 0. {
            return Err(Exception::range_error("startTime must not be negative"));
        }

        // If startTime is less than currentTime, it is clamped to currentTime.
        start_time = max(start_time, this.current_time());
        this.map_insert_result(cx.vm(), this.timeline.set_value_at_time(value, start_time))?;
        Ok(*this)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-linearramptovalueattime
    pub(crate) fn linear_ramp_to_value_at_time(
        this: &Ref<Self>,
        cx: &mut CallContext,
        value: f32,
        mut end_time: f64,
    ) -> WebIdlResult<Ref<Self>> {
        // A RangeError exception MUST be thrown if endTime is negative or is not a finite number.
        if end_time < 0. {
            return Err(Exception::range_error("endTime must not be negative"));
        }

        // If endTime is less than currentTime, it is clamped to currentTime.
        let current_time = this.current_time();
        end_time = max(end_time, current_time);
        this.map_insert_result(
            cx.vm(),
            this.timeline
                .linear_ramp_to_value_at_time(value, end_time, current_time, this.timeline.current_value()),
        )?;
        Ok(*this)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-exponentialramptovalueattime
    pub(crate) fn exponential_ramp_to_value_at_time(
        this: &Ref<Self>,
        cx: &mut CallContext,
        value: f32,
        mut end_time: f64,
    ) -> WebIdlResult<Ref<Self>> {
        // A RangeError exception MUST be thrown if value is equal to 0.
        if value == 0. {
            return Err(Exception::range_error("value must not be zero"));
        }

        // A RangeError exception MUST be thrown if endTime is negative or is not a finite number.
        if end_time < 0. {
            return Err(Exception::range_error("endTime must not be negative"));
        }

        // If endTime is less than currentTime, it is clamped to currentTime.
        let current_time = this.current_time();
        end_time = max(end_time, current_time);
        this.map_insert_result(
            cx.vm(),
            this.timeline.exponential_ramp_to_value_at_time(
                value,
                end_time,
                current_time,
                this.timeline.current_value(),
            ),
        )?;
        Ok(*this)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-settargetattime
    pub(crate) fn set_target_at_time(
        this: &Ref<Self>,
        cx: &mut CallContext,
        target: f32,
        mut start_time: f64,
        time_constant: f32,
    ) -> WebIdlResult<Ref<Self>> {
        // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
        if start_time < 0. {
            return Err(Exception::range_error("startTime must not be negative"));
        }

        // A RangeError exception MUST be thrown if timeConstant is negative or is not a finite number.
        if time_constant < 0. {
            return Err(Exception::range_error("timeConstant must not be negative"));
        }

        // If startTime is less than currentTime, it is clamped to currentTime.
        start_time = max(start_time, this.current_time());
        this.map_insert_result(
            cx.vm(),
            this.timeline.set_target_at_time(target, start_time, time_constant),
        )?;
        Ok(*this)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-setvaluecurveattime
    #[allow(
        clippy::redundant_clone,
        reason = "the C++ operation copies its borrowed sequence into the timeline"
    )]
    pub(crate) fn set_value_curve_at_time(
        this: &Ref<Self>,
        cx: &mut CallContext,
        values: Vec<f32>,
        mut start_time: f64,
        duration: f64,
    ) -> WebIdlResult<Ref<Self>> {
        // An InvalidStateError exception MUST be thrown if values has a length less than 2.
        if values.len() < 2 {
            return Err(this.context_error(
                cx.vm(),
                "InvalidStateError",
                b"values must contain at least two elements",
            ));
        }

        // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
        if start_time < 0. {
            return Err(Exception::range_error("startTime must not be negative"));
        }

        // A RangeError exception MUST be thrown if duration is not strictly positive or is not a finite number.
        if duration <= 0. {
            return Err(Exception::range_error("duration must be positive"));
        }

        // If startTime is less than currentTime, it is clamped to currentTime.
        start_time = max(start_time, this.current_time());
        this.map_insert_result(
            cx.vm(),
            this.timeline
                .set_value_curve_at_time(values.clone(), start_time, duration),
        )?;
        Ok(*this)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelscheduledvalues
    pub(crate) fn cancel_scheduled_values(
        this: &Ref<Self>,
        _: &mut CallContext,
        mut cancel_time: f64,
    ) -> WebIdlResult<Ref<Self>> {
        // A RangeError exception MUST be thrown if cancelTime is negative.
        if cancel_time < 0. {
            return Err(Exception::range_error("cancelTime must not be negative"));
        }

        // If cancelTime is less than currentTime, it is clamped to currentTime.
        cancel_time = max(cancel_time, this.current_time());
        this.timeline.cancel_scheduled_values(cancel_time);
        Ok(*this)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelandholdattime
    pub(crate) fn cancel_and_hold_at_time(
        this: &Ref<Self>,
        _: &mut CallContext,
        mut cancel_time: f64,
    ) -> WebIdlResult<Ref<Self>> {
        // A RangeError exception MUST be thrown if cancelTime is negative.
        if cancel_time < 0. {
            return Err(Exception::range_error("cancelTime must not be negative"));
        }

        // If cancelTime is less than currentTime, it is clamped to currentTime.
        cancel_time = max(cancel_time, this.current_time());
        this.timeline.cancel_and_hold_at_time(cancel_time);
        Ok(*this)
    }
}
