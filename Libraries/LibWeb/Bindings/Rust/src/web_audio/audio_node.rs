/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::ffi::c_void;
use std::ops::Deref;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::DeferGc;
use libjs_rust::api::{GcRefCell, ThrowCompletionOr, Trace, Value};
use libweb_rust::web_audio::control_message::{
    AddNode, ControlMessage, RemoveNode, ReplaceConnections, SetChannelConfig,
};
use libweb_rust::web_audio::rendering::render_node::{NodeHandle, OutgoingNodeConnection, OutgoingParamConnection};

use super::analyser_node::AnalyserNode;
use super::audio_buffer_source_node::AudioBufferSourceNode;
use super::audio_destination_node::AudioDestinationNode;
use super::audio_param::AudioParam;
use super::audio_scheduled_source_node::AudioScheduledSourceNode;
use super::audio_worklet_node::AudioWorkletNode;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use super::biquad_filter_node::BiquadFilterNode;
use super::channel_merger_node::ChannelMergerNode;
use super::channel_splitter_node::ChannelSplitterNode;
use super::constant_source_node::ConstantSourceNode;
use super::convolver_node::ConvolverNode;
use super::delay_node::DelayNode;
use super::dynamics_compressor_node::DynamicsCompressorNode;
use super::gain_node::GainNode;
use super::iir_filter_node::IIRFilterNode;
use super::media_element_audio_source_node::MediaElementAudioSourceNode;
use super::media_stream_audio_destination_node::MediaStreamAudioDestinationNode;
use super::media_stream_audio_source_node::MediaStreamAudioSourceNode;
use super::oscillator_node::OscillatorNode;
use super::panner_node::PannerNode;
use super::script_processor_node::ScriptProcessorNode;
use super::stereo_panner_node::StereoPannerNode;
use super::wave_shaper_node::WaveShaperNode;
use crate::bindings::conversions::ToJs;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) use libweb_rust::web_audio::rendering::render_node::{ChannelCountMode, ChannelInterpretation};

#[derive(Default)]
pub(crate) struct AudioNodeOptions {
    pub channel_count: Option<u32>,
    pub channel_count_mode: Option<ChannelCountMode>,
    pub channel_interpretation: Option<ChannelInterpretation>,
}

pub(crate) struct AudioNodeDefaultOptions {
    pub channel_count: u32,
    pub channel_count_mode: ChannelCountMode,
    pub channel_interpretation: ChannelInterpretation,
}

#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
struct AudioNodeConnection {
    destination_node: AnyAudioNode,
    output: u32,
    input: u32,
}

#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
struct AudioParamConnection {
    destination_param: Ref<AudioParam>,
    output: u32,
}

// https://webaudio.github.io/web-audio-api/#AudioNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioNode {
    event_target: CppEventTarget,
    context: AnyBaseAudioContext,
    channel_count: Cell<u32>,
    #[gc(untraced)]
    channel_count_mode: Cell<ChannelCountMode>,
    #[gc(untraced)]
    channel_interpretation: Cell<ChannelInterpretation>,

    // Connections from this node's outputs into other AudioNode inputs.
    output_connections: GcRefCell<Vec<AudioNodeConnection>>,

    // Connections from this node's outputs into AudioParams.
    param_connections: GcRefCell<Vec<AudioParamConnection>>,
    node_id: u64,
    this: Cell<Option<AnyAudioNode>>,
}

impl Wrappable for AudioNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.finalize_node();
    }
}

impl GetTheParent for AudioNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl AudioNode {
    pub(crate) fn new(_defer_gc: &DeferGc<'_>, context: AnyBaseAudioContext, channel_count: u32) -> Self {
        let node_id = context.next_node_id();
        Self {
            event_target: CppEventTarget::new(),
            context,
            channel_count: Cell::new(channel_count),
            channel_count_mode: Cell::new(ChannelCountMode::Max),
            channel_interpretation: Cell::new(ChannelInterpretation::Speakers),
            output_connections: GcRefCell::new(Vec::new()),
            param_connections: GcRefCell::new(Vec::new()),
            node_id,
            this: Cell::new(None),
        }
    }

    pub(crate) fn set_this(&self, this: AnyAudioNode) {
        self.this.set(Some(this));
    }

    pub(crate) fn this(&self) -> AnyAudioNode {
        self.this.get().expect("a node knows itself after allocation")
    }

    // A node that is no longer reachable from script or the audio graph retires
    // its renderer node during finalization while the owning context is alive.
    pub(crate) fn finalize_node(&self) {
        self.context
            .queue_control_message(ControlMessage::RemoveNode(RemoveNode { node_id: self.node_id }));
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn context_value(&self) -> AnyBaseAudioContext {
        self.context
    }

    pub(crate) fn node_id(&self) -> u64 {
        self.node_id
    }

    pub(crate) fn queue_render_node_creation(&self, node: Box<NodeHandle>) {
        self.context
            .queue_control_message(ControlMessage::AddNode(AddNode { node }));

        // The channel configuration is settled before the render node is created, and the rendering thread ignores
        // messages for nodes it does not know about yet, so it has to be sent again now.
        self.queue_channel_config_update();
    }

    fn queue_connection_update(&self) {
        let node_connections = self
            .output_connections
            .borrow()
            .iter()
            .map(|connection| OutgoingNodeConnection {
                destination: connection.destination_node.node_id(),
                output_index: connection.output as usize,
                input_index: connection.input as usize,
            })
            .collect();
        let param_connections = self
            .param_connections
            .borrow()
            .iter()
            .map(|connection| OutgoingParamConnection {
                destination: Arc::clone(connection.destination_param.render_param()),
                output_index: connection.output as usize,
            })
            .collect();
        self.context
            .queue_control_message(ControlMessage::ReplaceConnections(ReplaceConnections {
                source: self.node_id(),
                node_connections,
                param_connections,
            }));
    }

    fn queue_channel_config_update(&self) {
        self.context
            .queue_control_message(ControlMessage::SetChannelConfig(SetChannelConfig {
                node_id: self.node_id(),
                channel_count: self.channel_count.get() as usize,
                channel_count_mode: self.channel_count_mode.get(),
                channel_interpretation: self.channel_interpretation.get(),
            }));
    }

    pub(crate) fn initialize_audio_node_options(
        &self,
        cx: &mut CallContext,
        given_options: &AudioNodeOptions,
        default_options: &AudioNodeDefaultOptions,
    ) -> WebIdlResult<()> {
        // Set channel count, fallback to default if not provided
        if let Some(channel_count) = given_options.channel_count {
            self.set_channel_count(cx, channel_count)?;
        } else {
            self.set_channel_count(cx, default_options.channel_count)?;
        }

        // Set channel count mode, fallback to default if not provided
        if let Some(channel_count_mode) = given_options.channel_count_mode {
            self.set_channel_count_mode(cx, channel_count_mode)?;
        } else {
            self.set_channel_count_mode(cx, default_options.channel_count_mode)?;
        }

        // Set channel interpretation, fallback to default if not provided
        if let Some(channel_interpretation) = given_options.channel_interpretation {
            self.set_channel_interpretation(cx, channel_interpretation)?;
        } else {
            self.set_channel_interpretation(cx, default_options.channel_interpretation)?;
        }
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-connect
    pub(crate) fn connect_to_node(
        &self,
        cx: &mut CallContext,
        destination_node: AnyAudioNode,
        output: u32,
        input: u32,
    ) -> WebIdlResult<AnyAudioNode> {
        let output_connection = AudioNodeConnection {
            destination_node,
            output,
            input,
        };

        // There can only be one connection between a given output of one specific node and a given input of another specific node.
        // Multiple connections with the same termini are ignored.
        for existing_connection in self.output_connections.borrow().iter() {
            if *existing_connection == output_connection {
                return Ok(destination_node);
            }
        }

        // If the destination parameter is an AudioNode that has been created using another AudioContext, an InvalidAccessError MUST be thrown.
        if self.context != destination_node.context_value() {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_ascii(b"Cannot connect to an AudioNode in a different AudioContext"),
            ));
        }

        // The output parameter is an index describing which output of the AudioNode from which to connect.
        // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if output >= self.number_of_outputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Output index {output} exceeds number of outputs")),
            ));
        }

        // The input parameter is an index describing which input of the destination AudioNode to connect to.
        // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if input >= destination_node.number_of_inputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Input index '{input}' exceeds number of inputs")),
            ));
        }

        // Connect node's output to destination_node input.
        self.output_connections.borrow_mut(cx.vm()).push(output_connection);

        self.queue_connection_update();

        Ok(destination_node)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-connect-destinationparam-output
    pub(crate) fn connect_to_param(
        &self,
        cx: &mut CallContext,
        destination_param: Ref<AudioParam>,
        output: u32,
    ) -> WebIdlResult<()> {
        let param_connection = AudioParamConnection {
            destination_param,
            output,
        };

        // There can only be one connection between a given output of one specific node and a specific AudioParam. Multiple connections
        //  with the same termini are ignored.
        for existing_connection in self.param_connections.borrow().iter() {
            if *existing_connection == param_connection {
                return Ok(());
            }
        }

        // If destinationParam belongs to an AudioNode that belongs to a BaseAudioContext that is different from the BaseAudioContext
        // that has created the AudioNode on which this method was called, an InvalidAccessError MUST be thrown.
        if self.context != destination_param.context_value() {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_ascii(b"Cannot connect to an AudioParam in a different AudioContext"),
            ));
        }

        // The output parameter is an index describing which output of the AudioNode from which to connect.
        // If the parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if output >= self.number_of_outputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Output index {output} exceeds number of outputs")),
            ));
        }

        // Connect node's output to destination_param.
        self.param_connections.borrow_mut(cx.vm()).push(param_connection);

        self.queue_connection_update();

        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect
    pub(crate) fn disconnect_all(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        self.output_connections.borrow_mut(cx.vm()).clear();
        self.param_connections.borrow_mut(cx.vm()).clear();

        self.queue_connection_update();
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-output
    pub(crate) fn disconnect_output(&self, cx: &mut CallContext, output: u32) -> WebIdlResult<()> {
        // The output parameter is an index describing which output of the AudioNode to disconnect.
        // It disconnects all outgoing connections from the given output.
        // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if output >= self.number_of_outputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Output index {output} exceeds number of outputs")),
            ));
        }

        self.output_connections
            .borrow_mut(cx.vm())
            .retain(|connection| !(connection.output == output));

        self.param_connections
            .borrow_mut(cx.vm())
            .retain(|connection| !(connection.output == output));

        self.queue_connection_update();

        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode
    pub(crate) fn disconnect_node(&self, cx: &mut CallContext, destination_node: AnyAudioNode) -> WebIdlResult<()> {
        // The destinationNode parameter is the AudioNode to disconnect.
        // It disconnects all outgoing connections to the given destinationNode.
        let before = self.output_connections.borrow().len();
        self.output_connections
            .borrow_mut(cx.vm())
            .retain(|connection| !(connection.destination_node == destination_node));

        // If there is no connection to the destinationNode, an InvalidAccessError exception MUST be thrown.
        if self.output_connections.borrow().len() == before {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_ascii(b"No connection to given AudioNode"),
            ));
        }

        self.queue_connection_update();

        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output
    pub(crate) fn disconnect_node_output(
        &self,
        cx: &mut CallContext,
        destination_node: AnyAudioNode,
        output: u32,
    ) -> WebIdlResult<()> {
        // The output parameter is an index describing which output of the AudioNode from which to disconnect.
        // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if output >= self.number_of_outputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Output index {output} exceeds number of outputs")),
            ));
        }

        // The destinationNode parameter is the AudioNode to disconnect.
        let before = self.output_connections.borrow().len();
        self.output_connections
            .borrow_mut(cx.vm())
            .retain(|connection| !(connection.destination_node == destination_node && connection.output == output));

        //  If there is no connection to the destinationNode from the given output, an InvalidAccessError exception MUST be thrown.
        if self.output_connections.borrow().len() == before {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8(&format!("No connection from output {output} to given AudioNode")),
            ));
        }

        self.queue_connection_update();

        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output-input
    pub(crate) fn disconnect_node_output_input(
        &self,
        cx: &mut CallContext,
        destination_node: AnyAudioNode,
        output: u32,
        input: u32,
    ) -> WebIdlResult<()> {
        // The output parameter is an index describing which output of the AudioNode from which to disconnect.
        // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if output >= self.number_of_outputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Output index {output} exceeds number of outputs")),
            ));
        }

        // The input parameter is an index describing which input of the destination AudioNode to disconnect.
        // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if input >= destination_node.number_of_inputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Input index '{input}' exceeds number of inputs")),
            ));
        }

        // The destinationNode parameter is the AudioNode to disconnect.
        let before = self.output_connections.borrow().len();
        self.output_connections.borrow_mut(cx.vm()).retain(|connection| {
            !(connection.destination_node == destination_node
                && connection.output == output
                && connection.input == input)
        });

        // If there is no connection to the destinationNode from the given output to the given input, an InvalidAccessError exception MUST be thrown.
        if self.output_connections.borrow().len() == before {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8(&format!(
                    "No connection from output {output} to input {input} of given AudioNode"
                )),
            ));
        }

        self.queue_connection_update();

        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam
    pub(crate) fn disconnect_param(
        &self,
        cx: &mut CallContext,
        destination_param: Ref<AudioParam>,
    ) -> WebIdlResult<()> {
        // The destinationParam parameter is the AudioParam to disconnect.
        let before = self.param_connections.borrow().len();
        self.param_connections
            .borrow_mut(cx.vm())
            .retain(|connection| !(connection.destination_param == destination_param));

        // If there is no connection to the destinationParam, an InvalidAccessError exception MUST be thrown.
        if self.param_connections.borrow().len() == before {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_ascii(b"No connection to given AudioParam"),
            ));
        }

        self.queue_connection_update();

        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam-output
    pub(crate) fn disconnect_param_output(
        &self,
        cx: &mut CallContext,
        destination_param: Ref<AudioParam>,
        output: u32,
    ) -> WebIdlResult<()> {
        // The output parameter is an index describing which output of the AudioNode from which to disconnect.
        // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
        if output >= self.number_of_outputs_value() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_utf8(&format!("Output index {output} exceeds number of outputs")),
            ));
        }

        // The destinationParam parameter is the AudioParam to disconnect.
        let before = self.param_connections.borrow().len();
        self.param_connections
            .borrow_mut(cx.vm())
            .retain(|connection| !(connection.destination_param == destination_param && connection.output == output));

        // If there is no connection to the destinationParam, an InvalidAccessError exception MUST be thrown.
        if self.param_connections.borrow().len() == before {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8(&format!("No connection from output {output} to given AudioParam")),
            ));
        }

        self.queue_connection_update();

        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-context
    pub(crate) fn context(&self, _: &mut CallContext) -> WebIdlResult<AnyBaseAudioContext> {
        // The BaseAudioContext which owns this AudioNode.
        Ok(self.context)
    }

    pub(crate) fn channel_count_value(&self) -> u32 {
        self.channel_count.get()
    }

    pub(crate) fn channel_count(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.channel_count_value())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-numberofinputs
    pub(crate) fn number_of_inputs(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.number_of_inputs_value())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-numberofoutputs
    pub(crate) fn number_of_outputs(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.number_of_outputs_value())
    }

    pub(crate) fn number_of_inputs_value(&self) -> u32 {
        self.this().number_of_inputs()
    }

    pub(crate) fn number_of_outputs_value(&self) -> u32 {
        self.this().number_of_outputs()
    }

    pub(crate) fn set_channel_count(&self, cx: &mut CallContext, value: u32) -> WebIdlResult<()> {
        self.this().set_channel_count(cx, value)
    }

    pub(crate) fn set_channel_count_mode(&self, cx: &mut CallContext, value: ChannelCountMode) -> WebIdlResult<()> {
        self.this().set_channel_count_mode(cx, value)
    }

    pub(crate) fn set_channel_interpretation(
        &self,
        cx: &mut CallContext,
        value: ChannelInterpretation,
    ) -> WebIdlResult<()> {
        self.this().set_channel_interpretation(cx, value)
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
    pub(crate) fn set_channel_count_impl(&self, value: u32) -> WebIdlResult<()> {
        // If this value is set to zero or to a value greater than the implementation’s maximum number
        // of channels the implementation MUST throw a NotSupportedError exception.
        if value == 0 || value > BaseAudioContext::MAX_NUMBER_OF_CHANNELS {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Invalid channel count"),
            ));
        }
        self.channel_count.set(value);
        self.queue_channel_config_update();
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
    pub(crate) fn set_channel_count_mode_impl(&self, value: ChannelCountMode) -> WebIdlResult<()> {
        self.channel_count_mode.set(value);
        self.queue_channel_config_update();
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
    pub(crate) fn channel_count_mode(&self, _: &mut CallContext) -> WebIdlResult<ChannelCountMode> {
        Ok(self.channel_count_mode.get())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelinterpretation
    pub(crate) fn set_channel_interpretation_impl(&self, value: ChannelInterpretation) -> WebIdlResult<()> {
        self.channel_interpretation.set(value);
        self.queue_channel_config_update();
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelinterpretation
    pub(crate) fn channel_interpretation(&self, _: &mut CallContext) -> WebIdlResult<ChannelInterpretation> {
        Ok(self.channel_interpretation.get())
    }
}

#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
#[allow(clippy::enum_variant_names, reason = "the variants have the interfaces’ names")]
pub(crate) enum AnyAudioNode {
    AudioNode(Ref<AudioNode>),
    AudioScheduledSourceNode(Ref<AudioScheduledSourceNode>),
    AnalyserNode(Ref<AnalyserNode>),
    AudioBufferSourceNode(Ref<AudioBufferSourceNode>),
    AudioDestinationNode(Ref<AudioDestinationNode>),
    AudioWorkletNode(Ref<AudioWorkletNode>),
    BiquadFilterNode(Ref<BiquadFilterNode>),
    ChannelMergerNode(Ref<ChannelMergerNode>),
    ChannelSplitterNode(Ref<ChannelSplitterNode>),
    ConstantSourceNode(Ref<ConstantSourceNode>),
    ConvolverNode(Ref<ConvolverNode>),
    DelayNode(Ref<DelayNode>),
    DynamicsCompressorNode(Ref<DynamicsCompressorNode>),
    GainNode(Ref<GainNode>),
    IIRFilterNode(Ref<IIRFilterNode>),
    MediaElementAudioSourceNode(Ref<MediaElementAudioSourceNode>),
    MediaStreamAudioDestinationNode(Ref<MediaStreamAudioDestinationNode>),
    MediaStreamAudioSourceNode(Ref<MediaStreamAudioSourceNode>),
    OscillatorNode(Ref<OscillatorNode>),
    PannerNode(Ref<PannerNode>),
    ScriptProcessorNode(Ref<ScriptProcessorNode>),
    StereoPannerNode(Ref<StereoPannerNode>),
    WaveShaperNode(Ref<WaveShaperNode>),
}

impl Deref for AnyAudioNode {
    type Target = AudioNode;

    fn deref(&self) -> &AudioNode {
        match self {
            Self::AudioNode(node) => node,
            Self::AudioScheduledSourceNode(node) => node.parent(),
            Self::AnalyserNode(node) => node.parent(),
            Self::AudioBufferSourceNode(node) => node.parent().parent(),
            Self::AudioDestinationNode(node) => node.parent(),
            Self::AudioWorkletNode(node) => node.parent(),
            Self::BiquadFilterNode(node) => node.parent(),
            Self::ChannelMergerNode(node) => node.parent(),
            Self::ChannelSplitterNode(node) => node.parent(),
            Self::ConstantSourceNode(node) => node.parent().parent(),
            Self::ConvolverNode(node) => node.parent(),
            Self::DelayNode(node) => node.parent(),
            Self::DynamicsCompressorNode(node) => node.parent(),
            Self::GainNode(node) => node.parent(),
            Self::IIRFilterNode(node) => node.parent(),
            Self::MediaElementAudioSourceNode(node) => node.parent(),
            Self::MediaStreamAudioDestinationNode(node) => node.parent(),
            Self::MediaStreamAudioSourceNode(node) => node.parent(),
            Self::OscillatorNode(node) => node.parent().parent(),
            Self::PannerNode(node) => node.parent(),
            Self::ScriptProcessorNode(node) => node.parent(),
            Self::StereoPannerNode(node) => node.parent(),
            Self::WaveShaperNode(node) => node.parent(),
        }
    }
}

impl ToJs for AnyAudioNode {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        match self {
            Self::AudioNode(node) => node.to_js(cx),
            Self::AudioScheduledSourceNode(node) => node.to_js(cx),
            Self::AnalyserNode(node) => node.to_js(cx),
            Self::AudioBufferSourceNode(node) => node.to_js(cx),
            Self::AudioDestinationNode(node) => node.to_js(cx),
            Self::AudioWorkletNode(node) => node.to_js(cx),
            Self::BiquadFilterNode(node) => node.to_js(cx),
            Self::ChannelMergerNode(node) => node.to_js(cx),
            Self::ChannelSplitterNode(node) => node.to_js(cx),
            Self::ConstantSourceNode(node) => node.to_js(cx),
            Self::ConvolverNode(node) => node.to_js(cx),
            Self::DelayNode(node) => node.to_js(cx),
            Self::DynamicsCompressorNode(node) => node.to_js(cx),
            Self::GainNode(node) => node.to_js(cx),
            Self::IIRFilterNode(node) => node.to_js(cx),
            Self::MediaElementAudioSourceNode(node) => node.to_js(cx),
            Self::MediaStreamAudioDestinationNode(node) => node.to_js(cx),
            Self::MediaStreamAudioSourceNode(node) => node.to_js(cx),
            Self::OscillatorNode(node) => node.to_js(cx),
            Self::PannerNode(node) => node.to_js(cx),
            Self::ScriptProcessorNode(node) => node.to_js(cx),
            Self::StereoPannerNode(node) => node.to_js(cx),
            Self::WaveShaperNode(node) => node.to_js(cx),
        }
    }
}

impl AnyAudioNode {
    fn number_of_inputs(self) -> u32 {
        match self {
            Self::AudioNode(_) => unreachable!("an abstract audio node has no instances"),
            Self::AudioScheduledSourceNode(_) => unreachable!("an abstract audio node has no instances"),
            Self::AnalyserNode(_) => 1,
            Self::AudioBufferSourceNode(_) => 0,
            Self::AudioDestinationNode(_) => 1,
            Self::AudioWorkletNode(node) => node.number_of_inputs_value(),
            Self::BiquadFilterNode(_) => 1,
            Self::ChannelMergerNode(node) => node.number_of_inputs_value(),
            Self::ChannelSplitterNode(node) => node.number_of_inputs_value(),
            Self::ConstantSourceNode(_) => 0,
            Self::ConvolverNode(_) => 1,
            Self::DelayNode(_) => 1,
            Self::DynamicsCompressorNode(_) => 1,
            Self::GainNode(_) => 1,
            Self::IIRFilterNode(_) => 1,
            Self::MediaElementAudioSourceNode(_) => 0,
            Self::MediaStreamAudioDestinationNode(_) => 1,
            Self::MediaStreamAudioSourceNode(_) => 0,
            Self::OscillatorNode(_) => 0,
            Self::PannerNode(_) => 1,
            Self::ScriptProcessorNode(node) => node.number_of_inputs_value(),
            Self::StereoPannerNode(_) => 1,
            Self::WaveShaperNode(_) => 1,
        }
    }

    fn number_of_outputs(self) -> u32 {
        match self {
            Self::AudioNode(_) => unreachable!("an abstract audio node has no instances"),
            Self::AudioScheduledSourceNode(_) => unreachable!("an abstract audio node has no instances"),
            Self::AnalyserNode(_) => 1,
            Self::AudioBufferSourceNode(_) => 1,
            Self::AudioDestinationNode(_) => 1,
            Self::AudioWorkletNode(node) => node.number_of_outputs_value(),
            Self::BiquadFilterNode(_) => 1,
            Self::ChannelMergerNode(node) => node.number_of_outputs_value(),
            Self::ChannelSplitterNode(node) => node.number_of_outputs_value(),
            Self::ConstantSourceNode(_) => 1,
            Self::ConvolverNode(_) => 1,
            Self::DelayNode(_) => 1,
            Self::DynamicsCompressorNode(_) => 1,
            Self::GainNode(_) => 1,
            Self::IIRFilterNode(_) => 1,
            Self::MediaElementAudioSourceNode(_) => 1,
            Self::MediaStreamAudioDestinationNode(_) => 0,
            Self::MediaStreamAudioSourceNode(_) => 1,
            Self::OscillatorNode(_) => 1,
            Self::PannerNode(_) => 1,
            Self::ScriptProcessorNode(node) => node.number_of_outputs_value(),
            Self::StereoPannerNode(_) => 1,
            Self::WaveShaperNode(_) => 1,
        }
    }

    fn set_channel_count(self, cx: &mut CallContext, value: u32) -> WebIdlResult<()> {
        match self {
            Self::AudioDestinationNode(node) => node.set_channel_count_override(cx, value),
            Self::ChannelMergerNode(node) => node.set_channel_count_override(cx, value),
            Self::ChannelSplitterNode(node) => node.set_channel_count_override(cx, value),
            Self::DynamicsCompressorNode(node) => node.set_channel_count_override(cx, value),
            Self::PannerNode(node) => node.set_channel_count_override(cx, value),
            Self::StereoPannerNode(node) => node.set_channel_count_override(cx, value),
            Self::ScriptProcessorNode(node) => node.set_channel_count_override(cx, value),
            Self::ConvolverNode(node) => node.set_channel_count_override(cx, value),
            _ => self.deref().set_channel_count_impl(value),
        }
    }

    fn set_channel_count_mode(self, cx: &mut CallContext, value: ChannelCountMode) -> WebIdlResult<()> {
        match self {
            Self::ChannelMergerNode(node) => node.set_channel_count_mode_override(cx, value),
            Self::ChannelSplitterNode(node) => node.set_channel_count_mode_override(cx, value),
            Self::DynamicsCompressorNode(node) => node.set_channel_count_mode_override(cx, value),
            Self::PannerNode(node) => node.set_channel_count_mode_override(cx, value),
            Self::StereoPannerNode(node) => node.set_channel_count_mode_override(cx, value),
            Self::ScriptProcessorNode(node) => node.set_channel_count_mode_override(cx, value),
            Self::ConvolverNode(node) => node.set_channel_count_mode_override(cx, value),
            _ => self.deref().set_channel_count_mode_impl(value),
        }
    }

    fn set_channel_interpretation(self, cx: &mut CallContext, value: ChannelInterpretation) -> WebIdlResult<()> {
        match self {
            Self::ChannelSplitterNode(node) => node.set_channel_interpretation_override(cx, value),
            _ => self.deref().set_channel_interpretation_impl(value),
        }
    }
}
