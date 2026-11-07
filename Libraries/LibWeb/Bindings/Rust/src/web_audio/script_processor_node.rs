/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libjs_rust::api::Vm;
use libjs_rust::api::{EmbedderCell, GcWeak};
use libweb_rust::web_audio::rendering::script_processor_pipe::{
    ScriptProcessorPipe, ScriptProcessorPipeClient, ScriptProcessorPipeConfig,
};
use libweb_rust::web_audio::rendering::script_processor_render_node::create_script_processor_render_node;

use super::audio_buffer::AudioBuffer;
use super::audio_node::{AnyAudioNode, AudioNode, ChannelCountMode};
use super::audio_processing_event::AudioProcessingEvent;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::CppEvent;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::with_temporary_execution_context_and_callbacks;
use crate::bindings::interface::{Inherits, Ref, Wrappable, Wrapped, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ScriptProcessorNode {
    node: AudioNode,
    #[gc(untraced)]
    pipe: RefCell<Option<Arc<ScriptProcessorPipe>>>,
    buffer_size: Cell<i32>,
    number_of_input_channels: u8,
    number_of_output_channels: u8,
}

impl Inherits<AudioNode> for ScriptProcessorNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for ScriptProcessorNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        if let Some(pipe) = &*node.pipe.borrow() {
            pipe.set_client(None);
            pipe.shut_down();
        }
        node.node.finalize_node();
    }
}

impl GetTheParent for ScriptProcessorNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

struct PipeClient {
    vm: NonNull<Vm>,
    node: GcWeak<EmbedderCell<Wrapped<ScriptProcessorNode>>>,
}

impl ScriptProcessorPipeClient for PipeClient {
    fn process(&self, block: usize, start_frame: u64) {
        if let Some(node) = self.node.get() {
            // SAFETY: The VM of the control thread outlives its audio nodes.
            node.process_block(unsafe { self.vm.as_ref() }, block, start_frame);
        }
    }
}

// Other engines have a latency of two buffers. A realtime context needs more when the device pulls a burst of audio
// that is longer than a buffer, since script cannot run in the middle of a burst.
fn block_count_for_context(context: AnyBaseAudioContext, buffer_size: usize) -> usize {
    if matches!(context, AnyBaseAudioContext::OfflineAudioContext(_)) {
        return 2;
    }
    let quantum_size = BaseAudioContext::RENDER_QUANTUM_SIZE as usize;
    let sizing = libweb_rust::web_audio::rendering::audio_worklet_pipe::ring_sizing_for_device_latency(
        100,
        context.sample_rate_value(),
        quantum_size,
    );
    let lookahead_frames = sizing.prime_level * quantum_size;
    (lookahead_frames.div_ceil(buffer_size) + 1).max(2)
}

impl ScriptProcessorNode {
    pub(crate) const DEFAULT_BUFFER_SIZE: i32 = 1024;

    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        buffer_size: i32,
        number_of_input_channels: u32,
        number_of_output_channels: u32,
    ) -> WebIdlResult<Ref<Self>> {
        let script_processor_node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, number_of_input_channels as u8 as u32),
            number_of_input_channels: number_of_input_channels as u8,
            number_of_output_channels: number_of_output_channels as u8,
            buffer_size: Cell::new(0),
            pipe: RefCell::new(None),
        });
        script_processor_node
            .node
            .set_this(AnyAudioNode::ScriptProcessorNode(script_processor_node));
        script_processor_node
            .node
            .event_target()
            .set_owner(script_processor_node);
        script_processor_node.buffer_size.set(buffer_size);

        // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
        // The channel count mode cannot be changed from "explicit" and an NotSupportedError exception MUST be thrown for
        // any attempt to change the value.
        script_processor_node.set_channel_count_mode_override(cx, ChannelCountMode::Explicit)?;
        let pipe_config = ScriptProcessorPipeConfig {
            buffer_size: buffer_size as usize,
            input_channel_count: number_of_input_channels as usize,
            output_channel_count: number_of_output_channels as usize,
            block_count: block_count_for_context(context, buffer_size as usize),
            waits_for_processing: matches!(context, AnyBaseAudioContext::OfflineAudioContext(_)),
        };
        let pipe = ScriptProcessorPipe::new(pipe_config);
        pipe.set_client(Some(Rc::new(PipeClient {
            vm: NonNull::from(cx.vm()),
            node: GcWeak::new(cx.vm().heap(), script_processor_node),
        })));
        *script_processor_node.pipe.borrow_mut() = Some(Arc::clone(&pipe));
        context.add_script_processor_node(cx.vm(), script_processor_node);
        script_processor_node
            .node
            .queue_render_node_creation(create_script_processor_render_node(
                script_processor_node.node.node_id(),
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                Arc::clone(&pipe),
            ));
        Ok(script_processor_node)
    }

    fn process_block(&self, vm: &Vm, block: usize, start_frame: u64) {
        let pipe = self
            .pipe
            .borrow()
            .as_ref()
            .cloned()
            .expect("a process callback has a pipe");
        let config = pipe.config;
        if !self.node.context_value().document_is_fully_active() {
            unsafe { pipe.hand_block_back_to_rendering_thread(block, false) };
            return;
        }
        let sample_rate = self.node.context_value().sample_rate_value();
        let input_buffer = AudioBuffer::create(
            vm,
            config.input_channel_count as u32,
            config.buffer_size as u32,
            sample_rate,
        )
        .expect("input buffer allocation succeeds");
        for channel in 0..config.input_channel_count {
            let (destination, size) = input_buffer
                .channel_data(channel as u32)
                .unwrap_or_else(|_| panic!("the input channel exists"));
            let (source, length) = unsafe { pipe.input_channel(block, channel) };
            assert!(length * size_of::<f32>() <= size);
            unsafe { std::ptr::copy_nonoverlapping(source.cast::<u8>(), destination, length * size_of::<f32>()) };
        }
        let output_buffer = AudioBuffer::create(
            vm,
            config.output_channel_count as u32,
            config.buffer_size as u32,
            sample_rate,
        )
        .expect("output buffer allocation succeeds");
        let playback_time =
            (start_frame + (config.block_count * config.buffer_size) as u64) as f64 / f64::from(sample_rate);
        let realm = self.node.context_value().realm();
        with_temporary_execution_context_and_callbacks(realm, || {
            let event =
                CppEvent::create_for_owner(realm, &Utf16String::from_ascii(b"audioprocess"), false, false, false);
            let event = AudioProcessingEvent::create(vm, event, playback_time, input_buffer, output_buffer);
            self.node.event_target().dispatch_event(&event.event);
        });

        // https://webaudio.github.io/web-audio-api/#acquire-the-content
        // When the dispatch of an AudioProcessingEvent completes, it acquires the contents of its outputBuffer.
        let Some(output) = AudioBuffer::acquire_contents(output_buffer, vm) else {
            unsafe { pipe.hand_block_back_to_rendering_thread(block, false) };
            return;
        };
        for channel in 0..config.output_channel_count {
            let (destination, length) = unsafe { pipe.output_channel(block, channel) };
            assert!(output.channels[channel].len() <= length);
            unsafe {
                std::ptr::copy_nonoverlapping(
                    output.channels[channel].as_ptr(),
                    destination,
                    output.channels[channel].len(),
                );
            };
        }
        unsafe { pipe.hand_block_back_to_rendering_thread(block, true) };
    }

    pub(crate) fn validate_options(
        buffer_size: i32,
        number_of_input_channels: u32,
        number_of_output_channels: u32,
    ) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
        // It is invalid for both numberOfInputChannels and numberOfOutputChannels to be zero. In this case an
        // IndexSizeError MUST be thrown.
        if number_of_input_channels == 0 && number_of_output_channels == 0 {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(
                    b"Number of input and output channels cannot both be zero in a ScriptProcessorNode",
                ),
            ));
        }

        // This parameter determines the number of channels for this node’s input. The default value is 2. Values of up to
        // 32 must be supported. A NotSupportedError must be thrown if the number of channels is not supported.
        if number_of_input_channels > BaseAudioContext::MAX_NUMBER_OF_CHANNELS {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Invalid number of input channels"),
            ));
        }

        // This parameter determines the number of channels for this node’s output. The default value is 2. Values of up to
        // 32 must be supported. A NotSupportedError must be thrown if the number of channels is not supported.
        if number_of_output_channels > BaseAudioContext::MAX_NUMBER_OF_CHANNELS {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Invalid number of output channels"),
            ));
        }

        // The size of the buffer (in sample-frames) which needs to be processed each time audioprocess is fired. Legal
        // values are (256, 512, 1024, 2048, 4096, 8192, 16384).
        //
        // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createscriptprocessor
        // If the value of this parameter is not one of the allowed power-of-2 values listed above, an IndexSizeError MUST
        // be thrown.
        if !matches!(buffer_size, 256 | 512 | 1024 | 2048 | 4096 | 8192 | 16384) {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Unsupported buffer size for a ScriptProcessorNode"),
            ));
        }
        Ok(())
    }

    pub(crate) fn buffer_size(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(self.buffer_size.get())
    }

    pub(crate) fn number_of_inputs_value(&self) -> u32 {
        1
    }

    pub(crate) fn number_of_outputs_value(&self) -> u32 {
        1
    }

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
    pub(crate) fn set_channel_count_override(&self, _: &mut CallContext, _: u32) -> WebIdlResult<()> {
        // ScriptProcessorNode: The channel count cannot be changed, and an NotSupportedError exception MUST be thrown for
        // any attempt to change the value.
        Err(Exception::Dom(
            "InvalidStateError",
            Utf16String::from_ascii(b"Cannot modify channel count in a ScriptProcessorNode"),
        ))
    }

    pub(crate) fn set_channel_count_mode_override(
        &self,
        _: &mut CallContext,
        channel_count_mode: ChannelCountMode,
    ) -> WebIdlResult<()> {
        // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
        // ScriptProcessorNode: The channel count mode cannot be changed from "explicit" and an NotSupportedError exception
        // MUST be thrown for any attempt to change the value.
        if channel_count_mode != ChannelCountMode::Explicit {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Channel count mode must be 'explicit'"),
            ));
        }

        self.node.set_channel_count_mode_impl(channel_count_mode)
    }
}
