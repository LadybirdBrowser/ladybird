/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Module records: creating them, loading, linking and evaluating module graphs, the module requests that
//! HostLoadImportedModule receives and FinishLoadingImportedModule completes, and module namespaces.
//!
//! The exported functions run on the thread that owns the VM and trust their arguments, as object.rs states: `vm` is
//! the embedder's VM; modules, realms and the records of referrers and payloads are live cells of its heap;
//! host-defined cells are null or live cells of its heap; module requests are ones the runtime lent or the embedder
//! owns; and views and out parameters are valid. Cells these functions return are not rooted.
//!
//! Loading, linking and evaluating create promises and run JavaScript, so like HTML, which prepares to run script
//! first, the embedder calls them with an execution context of the module's realm running.

#![allow(
    clippy::missing_safety_doc,
    reason = "the module documentation states the contract every exported function shares"
)]

use core::ffi::c_void;

use crate::embedding::abi_types::{
    CellAbi, JSRealm, JSUtf16View, append_to_string_sink, cell_from_abi, cell_into_abi, completion_from_abi,
    completion_into_abi, optional_cell_into_abi, vm_from_abi,
};
use crate::embedding::environment::JSEnvironment;
use crate::embedding::hooks::{
    JSImportedModulePayload, JSImportedModuleReferrer, JSModuleRequest, imported_module_payload_from_abi,
    imported_module_referrer_from_abi, module_request_from_abi, module_request_into_abi,
};
use crate::embedding::realm::host_defined_slot_of;
use crate::embedding::script::{JSParserErrorSink, append_to_parser_error_sink};
use crate::layout::cell::Gc;
use crate::layout::host_class::{
    JS_RESOLVED_BINDING_AMBIGUOUS, JS_RESOLVED_BINDING_BINDING_NAME, JS_RESOLVED_BINDING_NAMESPACE,
    JS_RESOLVED_BINDING_NULL, JSCompletion, JSModule, JSPromiseCapability, JSResolvedBinding, JSVM, JSValue,
};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::cyclic_module::CyclicModule;
use crate::runtime::module::{Module, ResolvedBindingType, finish_loading_imported_module};
use crate::runtime::module_request::{ImportAttribute, ModuleRequest};
use crate::runtime::promise_capability::PromiseCapability;
use crate::runtime::source_text_module::SourceTextModule;
use crate::runtime::synthetic_module::{SyntheticModule, create_text_module, parse_json_module};
use crate::source_code::SourceCode;
use crate::utf16::Utf16View;

impl CellAbi for JSModule {
    type Cell = Module;
}

pub(crate) fn promise_capability_into_abi(capability: Gc<PromiseCapability>) -> *mut JSPromiseCapability {
    capability.as_ptr().cast()
}

fn cyclic_module_of(module: &Module) -> &CyclicModule {
    module
        .as_cyclic_module()
        .expect("only Cyclic Module Records have requested modules")
}

// Module requests

/// An ImportAttribute Record of a module request, as views of its key and value.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JSImportAttribute {
    pub key: JSUtf16View,
    pub value: JSUtf16View,
}

fn owned_module_request_into_abi(module_request: ModuleRequest) -> *mut JSModuleRequest {
    Box::into_raw(Box::new(module_request)).cast()
}

/// A new ModuleRequest Record with the specifier and the `attribute_count` attributes of `attributes`, in the order
/// given, which the embedder owns until it destroys it. Copies the strings. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_request_create(
    specifier: JSUtf16View,
    attributes: *const JSImportAttribute,
    attribute_count: usize,
) -> *mut JSModuleRequest {
    let attributes = if attribute_count == 0 {
        &[]
    } else {
        // SAFETY: The embedder passes that many attributes.
        unsafe { core::slice::from_raw_parts(attributes, attribute_count) }
    };
    // SAFETY: See the module documentation.
    let module_request = unsafe {
        ModuleRequest {
            module_specifier: specifier.as_view().to_utf16_fly_string(),
            attributes: attributes
                .iter()
                .map(|attribute| {
                    ImportAttribute::new(
                        attribute.key.as_view().to_utf16_string(),
                        attribute.value.as_view().to_utf16_string(),
                    )
                })
                .collect(),
        }
    };
    owned_module_request_into_abi(module_request)
}

/// Destroys a module request that js_module_request_create() returned. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_request_destroy(module_request: *mut JSModuleRequest) {
    assert!(!module_request.is_null(), "the embedder passes a module request");
    // SAFETY: The embedder passes a module request it owns, which came out of a Box.
    drop(unsafe { Box::from_raw(module_request.cast::<ModuleRequest>()) });
}

/// The [[Specifier]] of the module request, valid while the request lives. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_request_specifier(module_request: *const JSModuleRequest) -> JSUtf16View {
    // SAFETY: See the module documentation.
    let module_request = unsafe { module_request_from_abi(module_request) };
    JSUtf16View::of(Utf16View::of_fly_string(&module_request.module_specifier))
}

/// The number of [[Attributes]] of the module request. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_request_attribute_count(module_request: *const JSModuleRequest) -> usize {
    // SAFETY: See the module documentation.
    unsafe { module_request_from_abi(module_request) }.attributes.len()
}

/// The attribute at `index` of the module request's [[Attributes]], whose strings are valid while the request lives.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_request_attribute(
    module_request: *const JSModuleRequest,
    index: usize,
) -> JSImportAttribute {
    // SAFETY: See the module documentation.
    let attribute = &unsafe { module_request_from_abi(module_request) }.attributes[index];
    JSImportAttribute {
        key: JSUtf16View::of(Utf16View::of_string(&attribute.key)),
        value: JSUtf16View::of(Utf16View::of_string(&attribute.value)),
    }
}

// Creating module records

/// SourceTextModule::parse(source_text, realm, filename, display_filename, host_defined, line_number_offset):
/// ParseModule ( sourceText, realm, hostDefined ) for `source`, which starts `line_number_offset` lines into the text
/// it was taken from. Module loading resolves the module's imports against `filename`, and its code reports
/// `display_filename`, or the filename if that is empty, in its stack frames and errors. `host_defined` is null or one
/// of the embedder's cells, which the module keeps alive as its [[HostDefined]]. Returns an unrooted module, or null
/// after appending the syntax errors to `errors` (which may be null). Main thread only.
#[unsafe(no_mangle)]
#[allow(
    clippy::too_many_arguments,
    reason = "C++ SourceTextModule::parse takes all of these"
)]
pub unsafe extern "C" fn js_module_parse_source_text_module(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    source: JSUtf16View,
    filename: JSUtf16View,
    display_filename: JSUtf16View,
    host_defined: *mut c_void,
    line_number_offset: usize,
    errors: *const JSParserErrorSink,
) -> *mut JSModule {
    // SAFETY: See the module documentation.
    let (vm, realm, source, filename, display_filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            source.as_view(),
            filename.as_view(),
            display_filename.as_view(),
            host_defined_slot_of(host_defined),
        )
    };
    let display_filename = if display_filename.is_empty() {
        filename.to_utf16_string()
    } else {
        display_filename.to_utf16_string()
    };
    let source_code = SourceCode::create(display_filename, source.to_utf16_string());
    match SourceTextModule::parse_with_host_defined(
        vm,
        source_code,
        realm,
        &filename.to_utf8(),
        host_defined,
        line_number_offset,
    ) {
        Ok(module) => cell_into_abi(module.upcast::<Module>()),
        Err(parser_errors) => {
            // SAFETY: See the module documentation.
            unsafe { append_to_parser_error_sink(errors, &parser_errors) };
            core::ptr::null_mut()
        }
    }
}

/// ParseJSONModule ( source ): a normal completion whose payload is the new Synthetic Module Record, which exports the
/// parsed value as its default, or the throw completion of ParseJSON. Like ParseJSON, it creates the value and any
/// SyntaxError in the current realm, so the embedder calls it with an execution context of `realm` running, as HTML
/// does with a TemporaryExecutionContext. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_parse_json_module(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    source: JSUtf16View,
    filename: JSUtf16View,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, realm, source, filename) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            source.as_view(),
            filename.as_view().to_utf8(),
        )
    };
    let module = parse_json_module(vm, realm, source, filename);
    completion_into_abi(module.map(|module| cell_into_abi::<JSModule>(module.upcast())))
}

/// CreateTextModule ( source ): a new Synthetic Module Record whose default export is the text. Returns an unrooted
/// module. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_create_text_module(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    text: JSUtf16View,
    filename: JSUtf16View,
) -> *mut JSModule {
    // SAFETY: See the module documentation.
    let (vm, realm, text, filename) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            text.as_view(),
            filename.as_view().to_utf8(),
        )
    };
    cell_into_abi(create_text_module(vm, realm, text, filename).upcast::<Module>())
}

/// CreateDefaultExportSyntheticModule ( defaultExport ): a new Synthetic Module Record whose default export is the
/// value, as CSS module scripts export their style sheet. Returns an unrooted module. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_create_default_export_synthetic_module(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    default_export: JSValue,
    filename: JSUtf16View,
) -> *mut JSModule {
    // SAFETY: See the module documentation.
    let (vm, realm, filename) = unsafe { (vm_from_abi(vm), cell_from_abi(realm), filename.as_view().to_utf8()) };
    let module = SyntheticModule::create_default_export_synthetic_module(vm, realm, Value(default_export), filename);
    cell_into_abi(module.upcast::<Module>())
}

// The fields of module records

/// The module's [[Realm]]. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_realm(module: *mut JSModule) -> *mut JSRealm {
    // SAFETY: See the module documentation.
    cell_into_abi(unsafe { cell_from_abi::<JSModule>(module) }.realm())
}

/// The module's [[HostDefined]] cell, or null if it has none. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_host_defined(module: *mut JSModule) -> *mut c_void {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSModule>(module) }
        .host_defined()
        .map_or(core::ptr::null_mut(), core::ptr::NonNull::as_ptr)
}

/// The module's [[Environment]], a module environment, or null before linking creates it. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_environment(module: *mut JSModule) -> *mut JSEnvironment {
    // SAFETY: See the module documentation.
    let environment = unsafe { cell_from_abi::<JSModule>(module) }.environment();
    optional_cell_into_abi::<JSEnvironment>(environment.map(Gc::upcast))
}

/// The number of [[RequestedModules]] of a Cyclic Module Record. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_requested_module_count(module: *mut JSModule) -> usize {
    // SAFETY: See the module documentation.
    let module = unsafe { cell_from_abi::<JSModule>(module) };
    cyclic_module_of(&module).requested_modules().len()
}

/// The module request at `index` of a Cyclic Module Record's [[RequestedModules]], which the module lends for as long
/// as it lives. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_requested_module(module: *mut JSModule, index: usize) -> *const JSModuleRequest {
    // SAFETY: See the module documentation.
    let module = unsafe { cell_from_abi::<JSModule>(module) };
    module_request_into_abi(&cyclic_module_of(&module).requested_modules()[index])
}

/// GetImportedModule ( referrer, request ): the module that loading the requested modules of the Cyclic Module Record
/// `referrer` found for `module_request`. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_get_imported_module(
    referrer: *mut JSModule,
    module_request: *const JSModuleRequest,
) -> *mut JSModule {
    // SAFETY: See the module documentation.
    let (referrer, module_request) = unsafe {
        (
            cell_from_abi::<JSModule>(referrer),
            module_request_from_abi(module_request),
        )
    };
    cell_into_abi(cyclic_module_of(&referrer).get_imported_module(module_request))
}

// Loading, linking and evaluating module graphs

/// LoadRequestedModules ( [ hostDefined ] ) with `host_defined`, or without it for null: loads the module's graph
/// through the embedder's load_imported_module hook, which receives `host_defined` for every module of the graph.
/// Returns the promise capability, unrooted, whose promise settles once loading finishes or fails. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_load_requested_modules(
    vm: *mut JSVM,
    module: *mut JSModule,
    host_defined: *mut c_void,
) -> *mut JSPromiseCapability {
    // SAFETY: See the module documentation.
    let (vm, module, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSModule>(module),
            host_defined_slot_of(host_defined),
        )
    };
    promise_capability_into_abi(module.load_requested_modules(vm, host_defined))
}

/// Link ( ) of a module whose graph has loaded, with an unused payload. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_link(vm: *mut JSVM, module: *mut JSModule) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, module) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSModule>(module)) };
    completion_into_abi(module.link(vm))
}

/// Evaluate ( ) of a linked module: a normal completion whose payload is the unrooted promise capability whose promise
/// settles once the module's graph has evaluated. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_evaluate(vm: *mut JSVM, module: *mut JSModule) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, module) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSModule>(module)) };
    completion_into_abi(module.evaluate(vm).map(promise_capability_into_abi))
}

/// FinishLoadingImportedModule ( referrer, moduleRequest, payload, result ), for a referrer, module request and payload
/// that the load_imported_module hook received, sooner or later after it did, and that the embedder kept alive in
/// between. A normal `result` carries the loaded module as its payload, and a throw completion the reason loading
/// failed. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_finish_loading_imported_module(
    vm: *mut JSVM,
    referrer: JSImportedModuleReferrer,
    module_request: *const JSModuleRequest,
    payload: JSImportedModulePayload,
    result: JSCompletion,
) {
    let result: ThrowCompletionOr<Gc<Module>> = completion_from_abi(result).map(|module| {
        let module = core::ptr::with_exposed_provenance_mut::<JSModule>(module.0 as usize);
        // SAFETY: A normal result carries a live module.
        unsafe { cell_from_abi(module) }
    });
    // SAFETY: See the module documentation.
    let (vm, referrer, module_request, payload) = unsafe {
        (
            vm_from_abi(vm),
            imported_module_referrer_from_abi(referrer),
            module_request_from_abi(module_request),
            imported_module_payload_from_abi(payload),
        )
    };
    finish_loading_imported_module(vm, referrer, module_request, payload, result);
}

// Exports and namespaces

fn resolved_binding_type_into_abi(binding_type: ResolvedBindingType) -> u8 {
    match binding_type {
        ResolvedBindingType::BindingName => JS_RESOLVED_BINDING_BINDING_NAME,
        ResolvedBindingType::Namespace => JS_RESOLVED_BINDING_NAMESPACE,
        ResolvedBindingType::Ambiguous => JS_RESOLVED_BINDING_AMBIGUOUS,
        ResolvedBindingType::Null => JS_RESOLVED_BINDING_NULL,
    }
}

/// ResolveExport ( exportName ) of a module whose graph has loaded, which fills in `out` as a host module's
/// resolve_export hook does: it sets the type and the module, and appends the binding name to out->binding_name once
/// for a binding of type JS_RESOLVED_BINDING_BINDING_NAME. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_module_resolve_export(
    vm: *mut JSVM,
    module: *mut JSModule,
    export_name: JSUtf16View,
    out: *mut JSResolvedBinding,
) {
    // SAFETY: See the module documentation.
    let (vm, module, export_name, out) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSModule>(module),
            export_name.as_view().to_utf16_fly_string(),
            out.as_mut().expect("the embedder passes an out record"),
        )
    };
    let binding = module.resolve_export(vm, &export_name);
    out.r#type = resolved_binding_type_into_abi(binding.binding_type);
    out.module = optional_cell_into_abi::<JSModule>(binding.module);
    if binding.binding_type == ResolvedBindingType::BindingName {
        append_to_string_sink(&out.binding_name, Utf16View::of_fly_string(&binding.export_name));
    }
}
