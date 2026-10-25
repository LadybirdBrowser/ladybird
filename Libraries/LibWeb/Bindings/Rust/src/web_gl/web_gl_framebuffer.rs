/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.5

use libjs_rust::api::{Gc, GcRefCell, Realm, Trace, Vm};

use super::state::ContextState;
use super::web_gl_object::WebGLObject;
use super::web_gl_renderbuffer::WebGLRenderbuffer;
use super::web_gl_texture::WebGLTexture;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};

#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Attachment {
    #[gc(untraced)]
    attachment: u32,
    pub(crate) texture: Option<Ref<WebGLTexture>>,
    pub(crate) renderbuffer: Option<Ref<WebGLRenderbuffer>>,
    #[gc(untraced)]
    pub(crate) texture_target: u32,
    #[gc(untraced)]
    pub(crate) texture_level: i32,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLFramebuffer {
    object: WebGLObject,
    attachments: GcRefCell<Vec<Attachment>>,
}

impl Wrappable for WebGLFramebuffer {
    fn relevant_realm(&self, vm: &Vm) -> Option<Gc<Realm>> {
        self.object.relevant_realm(vm)
    }
}

impl Inherits<WebGLObject> for WebGLFramebuffer {
    fn parent(&self) -> &WebGLObject {
        &self.object
    }
}

impl WebGLFramebuffer {
    pub(crate) fn create(vm: &Vm, context: &ContextState, handle: u32) -> Ref<Self> {
        allocate(
            vm,
            Self {
                object: WebGLObject::new(context, handle),
                attachments: GcRefCell::new(Vec::new()),
            },
        )
    }

    pub(crate) fn attachment(&self, attachment: u32) -> Option<Attachment> {
        self.attachments
            .borrow()
            .iter()
            .find(|existing| existing.attachment == attachment)
            .copied()
    }

    fn set_attachment(&self, attachment: u32, new_attachment: Option<Attachment>) {
        let mut attachments = self.attachments.borrow_mut();
        attachments.retain(|existing| existing.attachment != attachment);
        attachments.extend(new_attachment);
    }

    pub(crate) fn set_renderbuffer_attachment(&self, attachment: u32, renderbuffer: Option<Ref<WebGLRenderbuffer>>) {
        let new_attachment = renderbuffer.map(|renderbuffer| Attachment {
            attachment,
            texture: None,
            renderbuffer: Some(renderbuffer),
            texture_target: 0,
            texture_level: 0,
        });
        self.set_attachment(attachment, new_attachment);
    }

    pub(crate) fn set_texture_attachment(
        &self,
        attachment: u32,
        texture: Option<Ref<WebGLTexture>>,
        texture_target: u32,
        texture_level: i32,
    ) {
        let new_attachment = texture.map(|texture| Attachment {
            attachment,
            texture: Some(texture),
            renderbuffer: None,
            texture_target,
            texture_level,
        });
        self.set_attachment(attachment, new_attachment);
    }
}
