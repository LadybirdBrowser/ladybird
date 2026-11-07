/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;

use libjs_rust::api::Object;
use libjs_rust::api::{Gc, Trace};
use libweb_rust::web_audio::control_message::FfiSharedCppObject;
use libweb_rust::web_audio::rendering::node_message::SharedCppObject;
use libweb_rust::web_audio::rendering::render_node::NodeHandle;

use super::audio_context::AudioContext;
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::AnyBaseAudioContext;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MediaStreamAudioDestinationNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-stream
    stream: crate::bindings::cpp_cell::CppCell,
    track: crate::bindings::cpp_cell::CppCell,
    #[gc(untraced)]
    shared_state: RefCell<Option<libweb_rust::web_audio::rendering::node_message::SharedCppObject>>,
}

impl Inherits<AudioNode> for MediaStreamAudioDestinationNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for MediaStreamAudioDestinationNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        // Drop the drain callback — and with it the GC root it holds on the track — on the
        // control thread. Any drain the render node still has queued becomes a no-op.
        if let Some(shared_state) = &*node.shared_state.borrow() {
            unsafe { web_audio_media_destination_clear_drain(shared_state.pointer) };
        }
        node.node.finalize_node();
    }
}

impl GetTheParent for MediaStreamAudioDestinationNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

unsafe extern "C" {
    fn web_audio_media_destination_create_stream(stream: *mut *mut c_void, track: *mut *mut c_void);
    fn web_audio_media_destination_create_state(
        track: *mut c_void,
        node_id: u64,
        channels: u32,
        sample_rate: u32,
        render_node: *mut *mut c_void,
    ) -> FfiSharedCppObject;
    fn web_audio_media_destination_clear_drain(state: *mut c_void);
    fn web_audio_media_destination_wrap_stream(realm: *mut c_void, stream: *mut c_void) -> *mut c_void;
}

impl MediaStreamAudioDestinationNode {
    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-mediastreamaudiodestinationnode
    pub(crate) fn create(
        cx: &mut CallContext,
        context: Ref<AudioContext>,
        options: &AudioNodeOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-stream
        // A MediaStream containing a single MediaStreamTrack with the same number of channels as
        // the node itself, and whose kind attribute has the value "audio".
        // NB: The track is deviceless — it carries no deviceId, so registering audio sinks on it
        //     never opens a capture stream; it is fed exclusively by this node's render node.
        let mut stream = std::ptr::null_mut();
        let mut track = std::ptr::null_mut();
        unsafe { web_audio_media_destination_create_stream(&raw mut stream, &raw mut track) };
        let stream = unsafe { CppCell::from_raw(stream) };
        let track = unsafe { CppCell::from_raw(track) };

        // 1. Initialize the AudioNode this, with context and options as arguments.
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, AnyBaseAudioContext::AudioContext(context), 2),
            stream,
            track,
            shared_state: RefCell::new(None),
        });
        node.node.set_this(AnyAudioNode::MediaStreamAudioDestinationNode(node));
        node.node.event_target().set_owner(node);

        // https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Explicit,
            channel_interpretation: ChannelInterpretation::Speakers,
        };
        node.node.initialize_audio_node_options(cx, options, &default_options)?;
        let sample_rate = context.parent().sample_rate_value() as u32;
        let mut render_node = std::ptr::null_mut();
        let shared_state = unsafe {
            web_audio_media_destination_create_state(
                node.track.as_ptr(),
                node.node.node_id(),
                node.node.channel_count_value(),
                sample_rate,
                &raw mut render_node,
            )
        };
        *node.shared_state.borrow_mut() = Some(SharedCppObject {
            pointer: shared_state.pointer,
            retain: shared_state.retain,
            release: shared_state.release,
        });
        node.node
            .queue_render_node_creation(unsafe { Box::from_raw(render_node.cast::<NodeHandle>()) });
        Ok(node)
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: Ref<AudioContext>,
        options: AudioNodeOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-stream
    pub(crate) fn stream(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        let wrapper =
            unsafe { web_audio_media_destination_wrap_stream(cx.realm().as_ptr().cast(), self.stream.as_ptr()) };
        Ok(unsafe { CppInterface::from_wrapper(Gc::<Object>::from_non_null(NonNull::new(wrapper.cast()).unwrap())) })
    }
}
