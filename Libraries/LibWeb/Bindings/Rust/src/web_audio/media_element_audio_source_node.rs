/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;

use libjs_rust::api::Object;
use libjs_rust::api::{Gc, Trace};

use super::audio_context::AudioContext;
use super::audio_node::{AnyAudioNode, AudioNode};
use super::base_audio_context::AnyBaseAudioContext;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MediaElementAudioSourceNode {
    node: AudioNode,
    media_element: CppCell,
}

impl Inherits<AudioNode> for MediaElementAudioSourceNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for MediaElementAudioSourceNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for MediaElementAudioSourceNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

unsafe extern "C" {
    fn web_audio_media_element_from_wrapper(wrapper: *mut c_void) -> *mut c_void;
    fn web_audio_media_element_wrap(realm: *mut c_void, cell: *mut c_void) -> *mut c_void;
}

pub(crate) struct MediaElementAudioSourceOptions {
    pub media_element: CppInterface,
}

impl MediaElementAudioSourceNode {
    pub(crate) fn create(
        cx: &mut CallContext,
        context: Ref<AudioContext>,
        media_element: CppInterface,
    ) -> WebIdlResult<Ref<Self>> {
        let media_element = unsafe {
            CppCell::from_raw(web_audio_media_element_from_wrapper(
                media_element.wrapper().as_ptr().cast(),
            ))
        };
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, AnyBaseAudioContext::AudioContext(context), 2),
            media_element,
        });
        node.node.set_this(AnyAudioNode::MediaElementAudioSourceNode(node));
        node.node.event_target().set_owner(node);
        Ok(node)
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: Ref<AudioContext>,
        options: MediaElementAudioSourceOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, context, options.media_element)
    }

    pub(crate) fn media_element(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        let wrapper = unsafe { web_audio_media_element_wrap(cx.realm().as_ptr().cast(), self.media_element.as_ptr()) };
        Ok(unsafe { CppInterface::from_wrapper(Gc::<Object>::from_non_null(NonNull::new(wrapper.cast()).unwrap())) })
    }
}
