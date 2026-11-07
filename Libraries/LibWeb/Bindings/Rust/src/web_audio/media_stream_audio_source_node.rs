/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::collapsible_if, reason = "preserve the C++ branch structure")]

use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::MarkedVec;
use libjs_rust::api::Object;
use libjs_rust::api::{Gc, Trace};
use libweb_rust::web_audio::control_message::{ControlMessage, FfiSharedCppObject};
use libweb_rust::web_audio::rendering::audio_frame_ring::AudioFrameRingCallbacks;
use libweb_rust::web_audio::rendering::media_stream_source_render_node::create_media_stream_source_render_node;
use libweb_rust::web_audio::rendering::node_message::{NodeMessage, SetMediaStreamSourceRing, SharedCppObject};

use super::audio_context::AudioContext;
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MediaStreamAudioSourceNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiosourcenode-mediastream
    media_stream: CppCell,

    // [[input track]]: the first audio track of the input stream, ordered by track id.
    input_track: libjs_rust::api::GcRefCell<Option<CppCell>>,

    // Carries the track's audio from the producing thread to the render thread; the sink is
    // registered on the input track and only ever references the ring, never this GC node.
    #[gc(untraced)]
    ring: RefCell<Option<SharedCppObject>>,
    #[gc(untraced)]
    sink: RefCell<Option<SharedCppObject>>,
}

impl Inherits<AudioNode> for MediaStreamAudioSourceNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for MediaStreamAudioSourceNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();

        // NB: Finalizers run before any dead cell is destroyed, so detaching from the input track
        //     is safe even when the track is collected in the same garbage collection cycle.
        if let Some(track) = &*node.input_track.borrow() {
            if let Some(sink) = &*node.sink.borrow() {
                unsafe { web_audio_media_source_detach(track.as_ptr(), sink.pointer) };
            }
        }
    }
}

impl GetTheParent for MediaStreamAudioSourceNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

unsafe extern "C" {
    fn web_audio_media_source_stream_from_wrapper(wrapper: *mut c_void) -> *mut c_void;
    fn web_audio_media_source_stream_wrap(realm: *mut c_void, cell: *mut c_void) -> *mut c_void;
}

pub(crate) struct MediaStreamAudioSourceOptions {
    pub media_stream: CppInterface,
}

unsafe extern "C" {
    fn web_audio_media_source_audio_tracks(
        stream: *mut c_void,
        context: *mut c_void,
        callback: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
    fn web_audio_media_source_track_id_less(a: *mut c_void, b: *mut c_void) -> bool;
    fn web_audio_media_source_track_channels(track: *mut c_void) -> u32;
    fn web_audio_media_source_has_live_track(track: *mut c_void) -> bool;
    fn web_audio_media_source_ring(channels: u32) -> FfiSharedCppObject;
    fn web_audio_media_source_ring_callbacks(callbacks: *mut AudioFrameRingCallbacks);
    fn web_audio_media_source_attach(track: *mut c_void, ring: *mut c_void, sample_rate: f32) -> FfiSharedCppObject;
    fn web_audio_media_source_detach(track: *mut c_void, sink: *mut c_void);
}

impl MediaStreamAudioSourceNode {
    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiosourcenode-mediastreamaudiosourcenode
    pub(crate) fn create(
        cx: &mut CallContext,
        context: Ref<AudioContext>,
        options: &MediaStreamAudioSourceOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. If the mediaStream member of options does not reference a MediaStream that has at least one MediaStreamTrack
        //    whose kind attribute has the value "audio", throw an InvalidStateError and abort these steps. Else, let this
        //    stream be inputStream.
        let input_stream = unsafe {
            CppCell::from_raw(web_audio_media_source_stream_from_wrapper(
                options.media_stream.wrapper().as_ptr().cast(),
            ))
        };
        let audio_tracks = MarkedVec::<CppCell>::new(cx.vm());
        unsafe extern "C" fn append(context: *mut c_void, track: *mut c_void) {
            unsafe { &*context.cast::<MarkedVec<'_, CppCell>>() }.push(unsafe { CppCell::from_raw(track) });
        }
        unsafe {
            web_audio_media_source_audio_tracks(
                input_stream.as_ptr(),
                (&raw const audio_tracks).cast_mut().cast(),
                append,
            );
        };
        if audio_tracks.is_empty() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"MediaStream has no audio tracks"),
            ));
        }

        // 2. Let tracks be the list of all MediaStreamTracks of inputStream that have a kind of "audio".
        // 3. Sort the elements in tracks based on their id attribute using an ordering on sequences of code unit values.
        audio_tracks.values_mut().sort_unstable_by(|a, b| {
            if unsafe { web_audio_media_source_track_id_less(a.as_ptr(), b.as_ptr()) } {
                std::cmp::Ordering::Less
            } else if unsafe { web_audio_media_source_track_id_less(b.as_ptr(), a.as_ptr()) } {
                std::cmp::Ordering::Greater
            } else {
                std::cmp::Ordering::Equal
            }
        });

        // 4. Initialize the AudioNode this, with context and options as arguments.
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, AnyBaseAudioContext::AudioContext(context), 2),
            media_stream: input_stream,
            input_track: libjs_rust::api::GcRefCell::new(None),
            ring: RefCell::new(None),
            sink: RefCell::new(None),
        });
        node.node.set_this(AnyAudioNode::MediaStreamAudioSourceNode(node));
        node.node.event_target().set_owner(node);

        // https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };
        node.node
            .initialize_audio_node_options(cx, &AudioNodeOptions::default(), &default_options)?;

        // 5. Set an internal slot [[input track]] on this MediaStreamAudioSourceNode to be the first element of tracks.
        //    This is the track used as the input audio for this MediaStreamAudioSourceNode.
        *node.input_track.borrow_mut(cx.vm()) = audio_tracks.get(0);
        let mut callbacks = std::mem::MaybeUninit::uninit();
        unsafe { web_audio_media_source_ring_callbacks(callbacks.as_mut_ptr()) };
        node.node.queue_render_node_creation(unsafe {
            create_media_stream_source_render_node(
                node.node.node_id(),
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                callbacks.assume_init(),
            )
        });
        node.attach_input_track();
        context.parent().add_media_stream_source(cx.vm(), node);
        Ok(node)
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: Ref<AudioContext>,
        options: MediaStreamAudioSourceOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, &options)
    }

    fn attach_input_track(&self) {
        let track = self.input_track.borrow();
        let track = track.as_ref().expect("the input track has been selected");
        let channel_count = if unsafe { web_audio_media_source_track_channels(track.as_ptr()) } == 1 {
            1
        } else {
            2
        };
        let ring = unsafe { web_audio_media_source_ring(channel_count) };
        let ring = SharedCppObject {
            pointer: ring.pointer,
            retain: ring.retain,
            release: ring.release,
        };
        self.node
            .context_value()
            .queue_control_message(ControlMessage::NodeMessage(NodeMessage::SetMediaStreamSourceRing(
                SetMediaStreamSourceRing {
                    node_id: self.node.node_id(),
                    ring: Some(ring.clone()),
                    channel_count,
                },
            )));
        let sink = unsafe {
            web_audio_media_source_attach(
                track.as_ptr(),
                ring.pointer,
                self.node.context_value().sample_rate_value(),
            )
        };
        *self.ring.borrow_mut() = Some(ring);
        if !sink.pointer.is_null() {
            *self.sink.borrow_mut() = Some(SharedCppObject {
                pointer: sink.pointer,
                retain: sink.retain,
                release: sink.release,
            });
        }
    }

    pub(crate) fn has_live_input_track(&self) -> bool {
        self.input_track
            .borrow()
            .as_ref()
            .is_some_and(|track| unsafe { web_audio_media_source_has_live_track(track.as_ptr()) })
    }

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiosourcenode-mediastream
    pub(crate) fn media_stream(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        let wrapper =
            unsafe { web_audio_media_source_stream_wrap(cx.realm().as_ptr().cast(), self.media_stream.as_ptr()) };
        Ok(unsafe { CppInterface::from_wrapper(Gc::<Object>::from_non_null(NonNull::new(wrapper.cast()).unwrap())) })
    }
}
