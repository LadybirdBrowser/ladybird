/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{Gc, Trace};
use libjs_rust::api::{TypedArrayBase, TypedArrayKind, create_typed_array};
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::rendering::audio_data::WaveShaperCurve;
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, SetWaveShaperParameters};
use libweb_rust::web_audio::rendering::render_nodes::create_wave_shaper_render_node;

use super::audio_array::{copy_float32_array, float32_array_length, overwrite_float32_array};
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) use libweb_rust::web_audio::rendering::node_message::OverSampleType;

// https://webaudio.github.io/web-audio-api/#WaveShaperNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WaveShaperNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
    curve: Cell<Option<Gc<TypedArrayBase>>>,

    // When this attribute is set, an internal copy of the curve is created by the WaveShaperNode.
    #[gc(untraced)]
    curve_copy: RefCell<Option<Arc<WaveShaperCurve>>>,

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve-set-slot
    curve_set: Cell<bool>,

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-oversample
    #[gc(untraced)]
    oversample: Cell<OverSampleType>,
}

impl Inherits<AudioNode> for WaveShaperNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for WaveShaperNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for WaveShaperNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct WaveShaperOptions {
    pub parent: AudioNodeOptions,
    pub curve: Option<Vec<f32>>,
    pub oversample: OverSampleType,
}

impl WaveShaperNode {
    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &WaveShaperOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // When the constructor is called with a BaseAudioContext c and an option object option, the user agent MUST
        // initialize the AudioNode this, with context and options as arguments.
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            curve: Cell::new(None),
            curve_copy: RefCell::new(None),
            curve_set: Cell::new(false),
            oversample: Cell::new(OverSampleType::None),
        });
        node.node.set_this(AnyAudioNode::WaveShaperNode(node));
        node.node.event_target().set_owner(node);

        // https://webaudio.github.io/web-audio-api/#WaveShaperNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to yes when the oversample attribute is "2x" or "4x".
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.node.queue_render_node_creation(create_wave_shaper_render_node(
            node.node.node_id(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
        ));
        node.oversample.set(options.oversample);

        // Also, let [[curve set]] be an internal slot of this WaveShaperNode. Initialize this slot to false. If options is
        // given and specifies a curve, set [[curve set]] to true.
        // NB: Setting the curve attribute to the curve of options sets [[curve set]] to true.
        if let Some(curve) = &options.curve {
            let array = create_typed_array(cx.vm(), context.realm(), TypedArrayKind::Float32Array, curve.len())?;
            overwrite_float32_array(&array, curve);
            node.set_curve(cx, Some(array))?;
        } else {
            node.send_parameters_to_render_node();
        }
        Ok(node)
    }

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-waveshapernode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: WaveShaperOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
    pub(crate) fn set_curve(&self, _: &mut CallContext, new_curve: Option<Gc<TypedArrayBase>>) -> WebIdlResult<()> {
        // A InvalidStateError MUST be thrown if this attribute is set with a Float32Array that has a length less than 2.
        if new_curve.is_some_and(|curve| float32_array_length(&curve) < 2) {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"The curve must have at least two values"),
            ));
        }

        // To set the curve attribute, execute these steps:
        // 1. Let new curve be a Float32Array to be assigned to curve or null. .
        // 2. If new curve is not null and [[curve set]] is true, throw an InvalidStateError and abort these steps.
        if new_curve.is_some() && self.curve_set.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"The curve has already been set"),
            ));
        }

        // 3. If new curve is not null, set [[curve set]] to true.
        if new_curve.is_some() {
            self.curve_set.set(true);
        }

        // 4. Assign new curve to the curve attribute.
        self.curve.set(new_curve);

        // When this attribute is set, an internal copy of the curve is created by the WaveShaperNode. Subsequent
        // modifications of the contents of the array used to set the attribute therefore have no effect.
        *self.curve_copy.borrow_mut() = new_curve.map(|curve| {
            Arc::new(WaveShaperCurve {
                values: copy_float32_array(&curve),
            })
        });
        self.send_parameters_to_render_node();
        Ok(())
    }

    pub(crate) fn curve(&self, _: &mut CallContext) -> WebIdlResult<Option<Gc<TypedArrayBase>>> {
        Ok(self.curve.get())
    }

    pub(crate) fn oversample(&self, _: &mut CallContext) -> WebIdlResult<OverSampleType> {
        Ok(self.oversample.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-oversample
    pub(crate) fn set_oversample(&self, _: &mut CallContext, value: OverSampleType) -> WebIdlResult<()> {
        self.oversample.set(value);
        self.send_parameters_to_render_node();
        Ok(())
    }

    fn send_parameters_to_render_node(&self) {
        self.node
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetWaveShaperParameters(
                SetWaveShaperParameters {
                    node_id: self.node.node_id(),
                    curve: self.curve_copy.borrow().clone(),
                    oversample: self.oversample.get(),
                },
            )));
    }
}
