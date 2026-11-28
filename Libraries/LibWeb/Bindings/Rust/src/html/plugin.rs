/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::mime_type::MimeType;
use crate::html::window::{PDF_VIEWER_MIME_TYPES, with_pdf_viewer_mime_type_objects};

#[derive(Trace)]
pub(crate) struct Plugin {
    window: CppWindow,
    // https://html.spec.whatwg.org/multipage/system-state.html#concept-plugin-name
    pub(crate) name: Utf16String,
}

impl Wrappable for Plugin {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.window.realm())
    }
}

impl Plugin {
    pub(crate) fn create(vm: &Vm, window: CppWindow, name: Utf16String) -> Ref<Self> {
        allocate(vm, |_| Self { window, name })
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The Plugin interface's name getter steps are to return this's name.
        Ok(self.name.clone())
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-description
    pub(crate) fn description(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The Plugin interface's description getter steps are to return "Portable Document Format".
        Ok(Utf16String::from_utf8("Portable Document Format"))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-filename
    pub(crate) fn filename(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The Plugin interface's filename getter steps are to return "internal-pdf-viewer".
        Ok(Utf16String::from_utf8("internal-pdf-viewer"))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewing-support:support-named-properties-3
    pub(crate) fn supported_property_names(&self, _: &mut CallContext) -> Vec<Utf16String> {
        // The Plugin interface supports named properties. If the user agent's PDF viewer supported is true, then they
        // are the PDF viewer mime types. Otherwise, they are the empty list.
        if !self.window.pdf_viewer_supported() {
            return Vec::new();
        }
        PDF_VIEWER_MIME_TYPES.into_iter().map(Utf16String::from_utf8).collect()
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-length
    pub(crate) fn length(&self, cx: &mut CallContext) -> WebIdlResult<u32> {
        // The Plugin interface's length getter steps are to return this's relevant global object's PDF viewer mime type
        // objects's size.
        Ok(with_pdf_viewer_mime_type_objects(cx.vm(), &self.window, |mime_types| {
            u32::try_from(mime_types.len()).expect("there are two PDF viewer mime type objects")
        }))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-item
    pub(crate) fn item(&self, cx: &mut CallContext, index: u32) -> WebIdlResult<Option<Ref<MimeType>>> {
        // 1. Let mimeTypes be this's relevant global object's PDF viewer mime type objects.
        // 2. If index < mimeTypes's size, then return mimeTypes[index].
        // 3. Return null.
        Ok(with_pdf_viewer_mime_type_objects(cx.vm(), &self.window, |mime_types| {
            mime_types.get(index as usize).copied()
        }))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-nameditem
    pub(crate) fn named_item(&self, cx: &mut CallContext, name: Utf16String) -> WebIdlResult<Option<Ref<MimeType>>> {
        // 1. For each MimeType mimeType of this's relevant global object's PDF viewer mime type objects: if mimeType's
        //    type is name, then return mimeType.
        // 2. Return null.
        Ok(with_pdf_viewer_mime_type_objects(cx.vm(), &self.window, |mime_types| {
            mime_types.iter().find(|mime_type| mime_type.r#type == name).copied()
        }))
    }
}
