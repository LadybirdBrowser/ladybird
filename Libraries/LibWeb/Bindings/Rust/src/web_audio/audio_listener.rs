/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_rust::api::{Trace, Vm};

use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::base_audio_context::AnyBaseAudioContext;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#AudioListener
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioListener {
    pub(crate) forward_x: Ref<AudioParam>,
    pub(crate) forward_y: Ref<AudioParam>,
    pub(crate) forward_z: Ref<AudioParam>,
    pub(crate) position_x: Ref<AudioParam>,
    pub(crate) position_y: Ref<AudioParam>,
    pub(crate) position_z: Ref<AudioParam>,
    pub(crate) up_x: Ref<AudioParam>,
    pub(crate) up_y: Ref<AudioParam>,
    pub(crate) up_z: Ref<AudioParam>,
}

impl Wrappable for AudioListener {}
impl AudioListener {
    pub(crate) fn create(vm: &Vm, context: AnyBaseAudioContext) -> Ref<Self> {
        allocate(vm, |_| Self {
            forward_x: AudioParam::create(
                vm,
                context,
                None,
                0.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            forward_y: AudioParam::create(
                vm,
                context,
                None,
                0.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            forward_z: AudioParam::create(
                vm,
                context,
                None,
                -1.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            position_x: AudioParam::create(
                vm,
                context,
                None,
                0.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            position_y: AudioParam::create(
                vm,
                context,
                None,
                0.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            position_z: AudioParam::create(
                vm,
                context,
                None,
                0.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            up_x: AudioParam::create(
                vm,
                context,
                None,
                0.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            up_y: AudioParam::create(
                vm,
                context,
                None,
                1.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            up_z: AudioParam::create(
                vm,
                context,
                None,
                0.,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
        })
    }

    pub(crate) fn forward_x(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.forward_x)
    }

    pub(crate) fn forward_y(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.forward_y)
    }

    pub(crate) fn forward_z(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.forward_z)
    }

    pub(crate) fn position_x(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.position_x)
    }

    pub(crate) fn position_y(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.position_y)
    }

    pub(crate) fn position_z(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.position_z)
    }

    pub(crate) fn up_x(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.up_x)
    }

    pub(crate) fn up_y(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.up_y)
    }

    pub(crate) fn up_z(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.up_z)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiolistener-setposition
    pub(crate) fn set_position(&self, cx: &mut CallContext, x: f32, y: f32, z: f32) -> WebIdlResult<()> {
        // This method is DEPRECATED. It is equivalent to setting positionX.value, positionY.value, and
        // positionZ.value directly with the given x, y, and z values, respectively.

        // Consequently, if any of the positionX, positionY, and positionZ AudioParams for this AudioListener have an
        // automation curve set using setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be
        // thrown.

        AudioParam::set_value(&self.position_x, cx, x)?;
        AudioParam::set_value(&self.position_y, cx, y)?;
        AudioParam::set_value(&self.position_z, cx, z)?;
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audiolistener-setorientation
    #[allow(clippy::too_many_arguments, reason = "the IDL operation has these arguments")]
    pub(crate) fn set_orientation(
        &self,
        cx: &mut CallContext,
        x: f32,
        y: f32,
        z: f32,
        x_up: f32,
        y_up: f32,
        z_up: f32,
    ) -> WebIdlResult<()> {
        // This method is DEPRECATED. It is equivalent to setting forwardX.value, forwardY.value,
        // forwardZ.value, upX.value, upY.value, and upZ.value directly with the given x, y, z, xUp,
        // yUp, and zUp values, respectively.

        // Consequently, if any of the forwardX, forwardY, forwardZ, upX, upY and upZ AudioParams have an automation curve
        // set using setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be thrown.

        AudioParam::set_value(&self.forward_x, cx, x)?;
        AudioParam::set_value(&self.forward_y, cx, y)?;
        AudioParam::set_value(&self.forward_z, cx, z)?;
        AudioParam::set_value(&self.up_x, cx, x_up)?;
        AudioParam::set_value(&self.up_y, cx, y_up)?;
        AudioParam::set_value(&self.up_z, cx, z_up)?;
        Ok(())
    }
}
