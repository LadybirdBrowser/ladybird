/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, SetPannerParameters};
use libweb_rust::web_audio::rendering::render_nodes::create_panner_render_node;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) use libweb_rust::web_audio::rendering::node_message::{DistanceModelType, PanningModelType};

// https://webaudio.github.io/web-audio-api/#PannerNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PannerNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-panningmodel
    #[gc(untraced)]
    panning_model: Cell<PanningModelType>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-positionx
    position_x: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-positiony
    position_y: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-positionz
    position_z: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-orientationx
    orientation_x: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-orientationy
    orientation_y: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-orientationz
    orientation_z: Ref<AudioParam>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-distancemodel
    #[gc(untraced)]
    distance_model: Cell<DistanceModelType>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
    ref_distance: Cell<f64>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
    max_distance: Cell<f64>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
    rolloff_factor: Cell<f64>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneinnerangle
    cone_inner_angle: Cell<f64>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneouterangle
    cone_outer_angle: Cell<f64>,

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
    cone_outer_gain: Cell<f64>,
}

impl Inherits<AudioNode> for PannerNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for PannerNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for PannerNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct PannerOptions {
    pub parent: AudioNodeOptions,
    pub panning_model: PanningModelType,
    pub distance_model: DistanceModelType,
    pub ref_distance: f64,
    pub max_distance: f64,
    pub rolloff_factor: f64,
    pub cone_inner_angle: f64,
    pub cone_outer_angle: f64,
    pub cone_outer_gain: f64,
    pub position_x: f32,
    pub position_y: f32,
    pub position_z: f32,
    pub orientation_x: f32,
    pub orientation_y: f32,
    pub orientation_z: f32,
}

impl PannerNode {
    fn queue_panner_parameters_update(&self) {
        self.node
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetPannerParameters(
                SetPannerParameters {
                    node_id: self.node.node_id(),
                    panning_model: self.panning_model.get(),
                    distance_model: self.distance_model.get(),
                    ref_distance: self.ref_distance.get(),
                    max_distance: self.max_distance.get(),
                    rolloff_factor: self.rolloff_factor.get(),
                    cone_inner_angle: self.cone_inner_angle.get(),
                    cone_outer_angle: self.cone_outer_angle.get(),
                    cone_outer_gain: self.cone_outer_gain.get(),
                },
            )));
    }

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &PannerOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // Create the node and allocate memory
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            panning_model: Cell::new(options.panning_model),
            position_x: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.position_x,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            position_y: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.position_y,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            position_z: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.position_z,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            orientation_x: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.orientation_x,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            orientation_y: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.orientation_y,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            orientation_z: AudioParam::create(
                cx.vm(),
                context,
                None,
                options.orientation_z,
                f32::MIN,
                f32::MAX,
                AutomationRate::ARate,
                FixedAutomationRate::No,
            ),
            distance_model: Cell::new(options.distance_model),
            ref_distance: Cell::new(options.ref_distance),
            max_distance: Cell::new(options.max_distance),
            rolloff_factor: Cell::new(options.rolloff_factor),
            cone_inner_angle: Cell::new(options.cone_inner_angle),
            cone_outer_angle: Cell::new(options.cone_outer_angle),
            cone_outer_gain: Cell::new(options.cone_outer_gain),
        });
        node.node.set_this(AnyAudioNode::PannerNode(node));
        node.node.event_target().set_owner(node);
        node.position_x.set_owner(AnyAudioNode::PannerNode(node));
        node.position_y.set_owner(AnyAudioNode::PannerNode(node));
        node.position_z.set_owner(AnyAudioNode::PannerNode(node));
        node.orientation_x.set_owner(AnyAudioNode::PannerNode(node));
        node.orientation_y.set_owner(AnyAudioNode::PannerNode(node));
        node.orientation_z.set_owner(AnyAudioNode::PannerNode(node));

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#PannerNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::ClampedMax,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to maybe
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        let listener = context.listener_value();
        let params = [
            node.position_x.render_param(),
            node.position_y.render_param(),
            node.position_z.render_param(),
            node.orientation_x.render_param(),
            node.orientation_y.render_param(),
            node.orientation_z.render_param(),
            listener.position_x.render_param(),
            listener.position_y.render_param(),
            listener.position_z.render_param(),
            listener.forward_x.render_param(),
            listener.forward_y.render_param(),
            listener.forward_z.render_param(),
            listener.up_x.render_param(),
            listener.up_y.render_param(),
            listener.up_z.render_param(),
        ];
        node.node.queue_render_node_creation(create_panner_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            &params,
        ));
        node.queue_panner_parameters_update();
        Ok(node)
    }

    pub(crate) fn validate_options(options: &PannerOptions) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
        // A RangeError exception MUST be thrown if this is set to a negative value.
        if options.ref_distance < 0. {
            return Err(Exception::range_error("refDistance cannot be negative"));
        }

        // https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
        // A RangeError exception MUST be thrown if this is set to a negative value.
        if options.rolloff_factor < 0. {
            return Err(Exception::range_error("rolloffFactor cannot be negative"));
        }

        // https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
        // A RangeError exception MUST be thrown if this is set to a non-positive value.
        if options.max_distance <= 0. {
            return Err(Exception::range_error("maxDistance must be positive"));
        }

        // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
        // It is a linear value (not dB) in the range [0, 1]. An InvalidStateError MUST be thrown if the parameter is outside this range.
        if options.cone_outer_gain < 0. || options.cone_outer_gain > 1. {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"coneOuterGain must be in the range of [0, 1]"),
            ));
        }
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-pannernode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: PannerOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::validate_options(&options)?;
        Self::create(cx, context, &options)
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

    pub(crate) fn orientation_x(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.orientation_x)
    }

    pub(crate) fn orientation_y(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.orientation_y)
    }

    pub(crate) fn orientation_z(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParam>> {
        Ok(self.orientation_z)
    }

    pub(crate) fn panning_model(&self, _: &mut CallContext) -> WebIdlResult<PanningModelType> {
        Ok(self.panning_model.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-panningmodel
    pub(crate) fn set_panning_model(&self, _: &mut CallContext, value: PanningModelType) -> WebIdlResult<()> {
        self.panning_model.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    pub(crate) fn distance_model(&self, _: &mut CallContext) -> WebIdlResult<DistanceModelType> {
        Ok(self.distance_model.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-distancemodel
    pub(crate) fn set_distance_model(&self, _: &mut CallContext, value: DistanceModelType) -> WebIdlResult<()> {
        self.distance_model.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    pub(crate) fn ref_distance(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.ref_distance.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
    pub(crate) fn set_ref_distance(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // A RangeError exception MUST be thrown if this is set to a negative value.
        if value < 0. {
            return Err(Exception::range_error("refDistance cannot be negative"));
        }
        self.ref_distance.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    pub(crate) fn max_distance(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.max_distance.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
    pub(crate) fn set_max_distance(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // A RangeError exception MUST be thrown if this is set to a non-positive value.
        if value <= 0. {
            return Err(Exception::range_error("maxDistance must be positive"));
        }
        self.max_distance.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    pub(crate) fn rolloff_factor(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.rolloff_factor.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
    pub(crate) fn set_rolloff_factor(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // A RangeError exception MUST be thrown if this is set to a negative value.
        if value < 0. {
            return Err(Exception::range_error("rolloffFactor cannot be negative"));
        }
        self.rolloff_factor.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    pub(crate) fn cone_inner_angle(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.cone_inner_angle.get())
    }

    pub(crate) fn set_cone_inner_angle(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.cone_inner_angle.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    pub(crate) fn cone_outer_angle(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.cone_outer_angle.get())
    }

    pub(crate) fn set_cone_outer_angle(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.cone_outer_angle.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    pub(crate) fn cone_outer_gain(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.cone_outer_gain.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
    pub(crate) fn set_cone_outer_gain(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // It is a linear value (not dB) in the range [0, 1]. An InvalidStateError MUST be thrown if the parameter is outside this range.
        if value < 0. || value > 1. {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"coneOuterGain must be in the range of [0, 1]"),
            ));
        }
        self.cone_outer_gain.set(value);
        self.queue_panner_parameters_update();
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-setposition
    pub(crate) fn set_position(&self, cx: &mut CallContext, x: f32, y: f32, z: f32) -> WebIdlResult<()> {
        // This method is DEPRECATED. It is equivalent to setting positionX.value, positionY.value, and positionZ.value
        // attribute directly with the x, y and z parameters, respectively.
        // Consequently, if any of the positionX, positionY, and positionZ AudioParams have an automation curve set using
        // setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be thrown.
        AudioParam::set_value(&self.position_x, cx, x)?;
        AudioParam::set_value(&self.position_y, cx, y)?;
        AudioParam::set_value(&self.position_z, cx, z)?;
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-setorientation
    pub(crate) fn set_orientation(&self, cx: &mut CallContext, x: f32, y: f32, z: f32) -> WebIdlResult<()> {
        // This method is DEPRECATED. It is equivalent to setting orientationX.value, orientationY.value, and
        // orientationZ.value attribute directly, with the x, y and z parameters, respectively.
        // Consequently, if any of the orientationX, orientationY, and orientationZ AudioParams have an automation curve set
        // using setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be thrown.
        AudioParam::set_value(&self.orientation_x, cx, x)?;
        AudioParam::set_value(&self.orientation_y, cx, y)?;
        AudioParam::set_value(&self.orientation_z, cx, z)?;
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, channel_count: u32) -> WebIdlResult<()> {
        if channel_count > 2 {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"PannerNode does not support channel count greater than 2"),
            ));
        }

        self.node.set_channel_count_impl(channel_count)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
    pub(crate) fn set_channel_count_mode_override(
        &self,
        _: &mut CallContext,
        mode: ChannelCountMode,
    ) -> WebIdlResult<()> {
        if mode == ChannelCountMode::Max {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"PannerNode does not support 'max' as channelCountMode."),
            ));
        }

        self.node.set_channel_count_mode_impl(mode)
    }
}
