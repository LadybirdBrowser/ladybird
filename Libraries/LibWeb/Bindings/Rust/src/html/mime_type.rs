/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/system-state.html#mimetype

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::plugin::Plugin;
use crate::html::window::with_pdf_viewer_plugin_objects;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MimeType {
    window: CppWindow,
    // https://html.spec.whatwg.org/multipage/system-state.html#concept-mimetype-type
    pub(crate) r#type: Utf16String,
}

impl Wrappable for MimeType {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.window.realm())
    }
}

impl MimeType {
    pub(crate) fn create(vm: &Vm, window: CppWindow, r#type: Utf16String) -> Ref<Self> {
        allocate(vm, |_| Self { window, r#type })
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The MimeType interface's type getter steps are to return this's type.
        Ok(self.r#type.clone())
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-description
    pub(crate) fn description(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The MimeType interface's description getter steps are to return "Portable Document Format".
        Ok(Utf16String::from_utf8("Portable Document Format"))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-suffixes
    pub(crate) fn suffixes(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The MimeType interface's suffixes getter steps are to return "pdf".
        Ok(Utf16String::from_utf8("pdf"))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-enabledplugin
    pub(crate) fn enabled_plugin(&self, cx: &mut CallContext) -> WebIdlResult<Ref<Plugin>> {
        // The MimeType interface's enabledPlugin getter steps are to return this's relevant global object's PDF viewer
        // plugin objects[0] (i.e., the generic "PDF Viewer" one).
        Ok(with_pdf_viewer_plugin_objects(cx.vm(), &self.window, |plugins| {
            *plugins.first().expect("the PDF viewer is supported")
        }))
    }
}
