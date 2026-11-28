/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/system-state.html#pluginarray

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::cpp_cell::{LentCppCell, cpp_class};
use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::plugin::Plugin;
use crate::html::window::{PDF_VIEWER_PLUGIN_NAMES, with_pdf_viewer_plugin_objects};

#[derive(Trace)]
pub(crate) struct PluginArray {
    window: CppWindow,
}

impl Wrappable for PluginArray {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.window.realm())
    }
}

impl PluginArray {
    // https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-refresh
    pub(crate) fn refresh(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The PluginArray interface's refresh() method steps are to do nothing.
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewing-support:support-named-properties
    pub(crate) fn supported_property_names(&self, _: &mut CallContext) -> Vec<Utf16String> {
        // The PluginArray interface supports named properties. If the user agent's PDF viewer supported is true, then
        // they are the PDF viewer plugin names. Otherwise, they are the empty list.
        if !self.window.pdf_viewer_supported() {
            return Vec::new();
        }
        PDF_VIEWER_PLUGIN_NAMES
            .into_iter()
            .map(Utf16String::from_utf8)
            .collect()
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-length
    pub(crate) fn length(&self, cx: &mut CallContext) -> WebIdlResult<u32> {
        // The PluginArray interface's length getter steps are to return this's relevant global object's PDF viewer
        // plugin objects's size.
        Ok(with_pdf_viewer_plugin_objects(cx.vm(), &self.window, |plugins| {
            u32::try_from(plugins.len()).expect("there are five PDF viewer plugin objects")
        }))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-item
    pub(crate) fn item(&self, cx: &mut CallContext, index: u32) -> WebIdlResult<Option<Ref<Plugin>>> {
        // 1. Let plugins be this's relevant global object's PDF viewer plugin objects.
        // 2. If index < plugins's size, then return plugins[index].
        // 3. Return null.
        Ok(with_pdf_viewer_plugin_objects(cx.vm(), &self.window, |plugins| {
            plugins.get(index as usize).copied()
        }))
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-nameditem
    pub(crate) fn named_item(&self, cx: &mut CallContext, name: Utf16String) -> WebIdlResult<Option<Ref<Plugin>>> {
        // 1. For each Plugin plugin of this's relevant global object's PDF viewer plugin objects: if plugin's name is
        //    name, then return plugin.
        // 2. Return null.
        Ok(with_pdf_viewer_plugin_objects(cx.vm(), &self.window, |plugins| {
            plugins.iter().find(|plugin| plugin.name == name).copied()
        }))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_plugin_array_create(vm: &Vm, window: &LentCppCell<cpp_class::Window>) -> *mut c_void {
    let window = CppWindow::from_lent(window);
    cell_into_abi(allocate(vm, |_| PluginArray { window }))
}
