/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebAssembly Module Record of the ESM integration, https://webassembly.github.io/esm-integration/js-api/: a
//! host module whose abstract methods are those of WebAssembly modules.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16FlyString;
use libjs_rust::api::{
    EmbedderCell, EmbedderClass, EmbedderValue, ErrorType, ForeignCellSlot, Gc, HostModule, InitializeBindingHint,
    JS_HOST_CLASS_MODULE, JS_RESOLVED_BINDING_BINDING_NAME, JS_RESOLVED_BINDING_NULL, JSCompletion, JSHostClass,
    JSHostModuleHooks, JSModule, JSPromiseCapability, JSResolvedBinding, JSStringSink, Module as ModuleRecord,
    ModuleEnvironment, ModuleRequest, Must, RawNativeFunctionResult, Realm, ThrowCompletionOr, Trace, Value, Vm,
    completion_into_abi, host_module_class_of,
};
use libwasm_engine::module::ImportDescription;
use libwasm_engine::runtime::ExternValue;
use libwasm_engine::types::{GlobalType, ValType};

use crate::bindings::interface::{Ref, allocate, realm_arguments};
use crate::web_assembly::errors::{WebAssemblyError, throw_webassembly_error};
use crate::web_assembly::exported_function::{create_host_function, exported_function_address};
use crate::web_assembly::global::Global;
use crate::web_assembly::instance::{
    Imports, Instance, function_type_in_store, global_type_in_store, instantiate_the_core_of_a_webassembly_module,
};
use crate::web_assembly::memory::Memory;
use crate::web_assembly::module::Module;
use crate::web_assembly::table::Table;
use crate::web_assembly::tag::Tag;
use crate::web_assembly::values::{to_js_value, to_webassembly_value};
use crate::web_assembly::{Agent, compile_a_webassembly_module};

/// The fields that a WebAssembly Module Record has besides those of a Cyclic Module Record.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct WebAssemblyModuleRecord {
    // [[Instance]]
    instance: Cell<Option<Ref<Instance>>>,
    // [[ModuleSource]]
    module_source: Ref<Module>,
    // AD-HOC: The export name list, which never changes.
    #[gc(untraced)]
    export_name_list: Vec<Utf16FlyString>,
}

static WEBASSEMBLY_MODULE_RECORD_CLASS: EmbedderClass<WebAssemblyModuleRecord> =
    EmbedderClass::new("WebAssemblyModuleRecord");

impl EmbedderValue for WebAssemblyModuleRecord {
    const CLASS: &'static EmbedderClass<Self> = &WEBASSEMBLY_MODULE_RECORD_CLASS;
}

static WEBASSEMBLY_MODULE_HOOKS: JSHostModuleHooks = JSHostModuleHooks {
    get_exported_names: Some(get_exported_names),
    resolve_export: Some(resolve_export),
    initialize_environment: Some(initialize_environment),
    execute_module: Some(execute_module),
};

static WEBASSEMBLY_MODULE_CLASS: JSHostClass = JSHostClass::new(
    JS_HOST_CLASS_MODULE,
    "WebAssemblyModule",
    None,
    (&raw const WEBASSEMBLY_MODULE_HOOKS).cast(),
    0,
);

fn is_webassembly_module_record(module: &ModuleRecord) -> bool {
    host_module_class_of(module).is_some_and(|class| core::ptr::eq(class, &raw const WEBASSEMBLY_MODULE_CLASS))
}

fn data_of_webassembly_module_record(module: &ModuleRecord) -> Gc<EmbedderCell<WebAssemblyModuleRecord>> {
    let module = module
        .downcast_ref::<HostModule>()
        .expect("a WebAssembly Module Record is a host module");
    let data = module
        .host_data()
        .get()
        .expect("a WebAssembly Module Record has its fields");
    // SAFETY: The host data of a WebAssembly Module Record is the cell of its fields.
    unsafe { WEBASSEMBLY_MODULE_RECORD_CLASS.downcast(data) }.expect("a WebAssembly Module Record has its fields")
}

/// # Safety
///
/// `module` must be a live WebAssembly Module Record.
unsafe fn module_record_from_abi<'a>(module: *mut JSModule) -> &'a ModuleRecord {
    // SAFETY: The caller passes a live module.
    unsafe { &*module.cast::<ModuleRecord>() }
}

fn link_error<T>(vm: &Vm, realm: Gc<Realm>, message: &str) -> ThrowCompletionOr<T> {
    Err(throw_webassembly_error(vm, realm, WebAssemblyError::Link, message))
}

// https://webassembly.github.io/esm-integration/js-api/index.html#parse-a-webassembly-module
fn parse_a_webassembly_module(
    vm: &Vm,
    realm: Gc<Realm>,
    bytes: &[u8],
    filename: String,
    host_defined: ForeignCellSlot,
) -> ThrowCompletionOr<Gc<HostModule>> {
    // NB: The hooks of the record reach the store through the agent of the thread, which this ensures there is.
    Agent::the(vm);

    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    let stable_bytes = bytes.to_vec();

    // 2. Compile the WebAssembly module stableBytes and store the result as module.
    // 3. If module is error, throw a CompileError exception.
    // NOTE:  When integrating with the JS String Builtins proposal, builtinSetNames should be passed in the following
    //        step as « "js-string" » and importedStringModule as null.
    let module = compile_a_webassembly_module(vm, realm, &stable_bytes)?;

    // 5. Let requestedModules be a set.
    let mut requested_modules: Vec<ModuleRequest> = Vec::new();

    // 6. For each (moduleName, name, type) in module_imports(module.[[Module]]),
    for import in &module.imports {
        // 1. If moduleName starts with the prefix "wasm-js:",
        if import.module.starts_with("wasm-js:") {
            // 1. Throw a LinkError exception.
            return link_error(vm, realm, "Import with invalid module name");
        }

        // 2. If name starts with the prefix "wasm:" or "wasm-js:",
        if import.name.starts_with("wasm:") || import.name.starts_with("wasm-js:") {
            // 1. Throw a LinkError exception.
            return link_error(vm, realm, "Import with invalid name");
        }

        // NOTE: The following step only applies when integrating with the JS String Builtins proposal.
        // FIXME: 3. If Find a builtin with (moduleName, name, type) and builtins module.[[BuiltinSets]] is not null,
        //           then continue.

        // 4. Append moduleName to requestedModules.
        let module_name = Utf16FlyString::from_utf8(&import.module);
        if !requested_modules
            .iter()
            .any(|request| request.module_specifier == module_name)
        {
            requested_modules.push(ModuleRequest::new(module_name));
        }
    }

    // 7. For each (name, type) in module_exports(module.[[Module]])
    for export in &module.exports {
        // 1. If name starts with the prefix "wasm:" or "wasm-js:",
        if export.name.starts_with("wasm:") || export.name.starts_with("wasm-js:") {
            // 1. Throw a LinkError exception.
            return link_error(vm, realm, "Export with invalid name");
        }
    }

    // https://webassembly.github.io/esm-integration/js-api/index.html#export-name-list
    // 1. Let module be record’s [[ModuleSource]] internal slot.
    // 2. Let exports be an empty list.
    // 3. For each(name, type) in module_exports(module.[[Module]])
    //     1. Append name to the end of exports.
    // 4. Return exports.
    let export_name_list = module
        .exports
        .iter()
        .map(|export| Utf16FlyString::from_utf8(&export.name))
        .collect();

    // 4. Construct a WebAssembly module object from module and bytes, and let module be the result.
    let module_source = allocate(vm, |_| Module::new(module, Vec::new(), None));

    // 8. Let moduleRecord be { [[Instance]]: ~empty~, [[Realm]]: realm, [[Environment]]: ~empty~,
    //                          [[Namespace]]: ~empty~, [[ModuleSource]]: module, [[HostDefined]]: hostDefined,
    //                          [[Status]]: "new", [[EvaluationError]]: undefined, [[DFSIndex]]: undefined,
    //                          [[DFSAncestorIndex]]: undefined, [[RequestedModules]]: requestedModules,
    //                          [[LoadedModules]]: « », [[CycleRoot]]: ~empty~, [[HasTLA]]: false,
    //                          [[AsyncEvaluation]]: false, [[TopLevelCapability]]: ~empty~ [[AsyncParentModules]]: « »,
    //                          [[PendingAsyncDependencies]]: ~empty~, }.
    let data = WEBASSEMBLY_MODULE_RECORD_CLASS.allocate(vm, |_| WebAssemblyModuleRecord {
        instance: Cell::new(None),
        module_source,
        export_name_list,
    });
    let host_data = ForeignCellSlot::empty();
    // SAFETY: The fields are a live cell of the heap, which the slot then keeps alive.
    unsafe { host_data.set(Some(NonNull::new_unchecked(data.as_ptr().cast()))) };
    let module_record = HostModule::create(
        vm,
        realm,
        &WEBASSEMBLY_MODULE_CLASS,
        filename,
        requested_modules,
        host_defined,
        host_data,
    );

    // 9. Set module.[[ModuleRecord]] to moduleRecord.
    // NB: Nothing reads [[ModuleRecord]], which would keep the record alive from the module object.

    // 10. Return moduleRecord.
    Ok(module_record)
}

/// Parses a WebAssembly module, https://webassembly.github.io/esm-integration/js-api/index.html#parse-a-webassembly-module,
/// for HTML's create a WebAssembly module script, and writes the WebAssembly Module Record.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, `length` readable bytes, `filename_length` bytes of UTF-8, a live cell
/// or null, and a writable module pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_web_assembly_parse_a_webassembly_module(
    vm: *mut c_void,
    realm: *mut c_void,
    bytes: *const u8,
    length: usize,
    filename: *const u8,
    filename_length: usize,
    host_defined: *mut c_void,
    module_record: *mut *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM, a live realm, and the bytes.
    let (vm, realm, bytes, filename) = unsafe {
        let (vm, realm) = realm_arguments(vm, realm);
        (
            vm,
            realm,
            ak::slice_from_raw_parts(bytes, length),
            String::from_utf8_lossy(ak::slice_from_raw_parts(filename, filename_length)).into_owned(),
        )
    };
    let host_defined_slot = ForeignCellSlot::empty();
    // SAFETY: The caller passes a live cell or null, which the slot then keeps alive.
    unsafe { host_defined_slot.set(NonNull::new(host_defined)) };
    let result = parse_a_webassembly_module(vm, realm, bytes, filename, host_defined_slot).map(|record| {
        // SAFETY: The caller passes a writable module pointer.
        unsafe { *module_record = record.as_ptr().cast() };
        Value::UNDEFINED
    });
    RawNativeFunctionResult::from_completion(result)
}

fn append_to_string_sink(sink: &JSStringSink, name: &Utf16FlyString) {
    let code_units = name.to_utf16();
    let append = sink.append.expect("a string sink appends");
    // SAFETY: The sink takes the code units for the call.
    unsafe { append(sink.context, code_units.as_ptr(), code_units.len()) };
}

// https://webassembly.github.io/esm-integration/js-api/index.html#get-exported-names
unsafe extern "C" fn get_exported_names(module: *mut JSModule, names: *mut JSStringSink) {
    // 1. Let record be this WebAssembly Module Record.
    // SAFETY: The runtime passes the record and a sink that outlives the call.
    let (record, names) = unsafe { (module_record_from_abi(module), &*names) };

    // 2. Return the export name list of record.
    for name in &data_of_webassembly_module_record(record).export_name_list {
        append_to_string_sink(names, name);
    }
}

// https://webassembly.github.io/esm-integration/js-api/index.html#resolve-export
unsafe extern "C" fn resolve_export(
    module: *mut JSModule,
    export_name: *const u16,
    export_name_length: usize,
    out: *mut JSResolvedBinding,
) {
    // 1. Let record be this WebAssembly Module Record.
    // SAFETY: The runtime passes the record, the code units of the name, and an out record.
    let (record, export_name, out) = unsafe {
        (
            module_record_from_abi(module),
            Utf16FlyString::from_utf16(if export_name_length == 0 {
                &[]
            } else {
                ak::slice_from_raw_parts(export_name, export_name_length)
            }),
            &mut *out,
        )
    };

    // 2. If the export name list of record contains exportName, return { [[Module]]: record, [[BindingName]]: exportName }.
    if data_of_webassembly_module_record(record)
        .export_name_list
        .contains(&export_name)
    {
        out.module = module;
        out.r#type = JS_RESOLVED_BINDING_BINDING_NAME;
        append_to_string_sink(&out.binding_name, &export_name);
        return;
    }

    // 3. Otherwise, return null.
    out.r#type = JS_RESOLVED_BINDING_NULL;
}

// https://webassembly.github.io/esm-integration/js-api/index.html#module-declaration-environment-setup
unsafe extern "C" fn initialize_environment(module: *mut JSModule) -> JSCompletion {
    let vm = Agent::current().vm();

    // 1. Let record be this WebAssembly Module Record.
    // SAFETY: The runtime passes the record.
    let record = unsafe { module_record_from_abi(module) };

    // 2. Let env be NewModuleEnvironment(null).
    let environment = ModuleEnvironment::create(vm, None);

    // 3. Set record.[[Environment]] to env.
    record.set_environment(environment);

    // 4. For each name in the export name list of record,
    for name in &data_of_webassembly_module_record(record).export_name_list {
        // 1. Perform !env.CreateImmutableBinding(name, true).
        environment.create_immutable_binding(vm, name, true).must();
    }

    completion_into_abi(Ok(()))
}

// https://webassembly.github.io/esm-integration/js-api/index.html#module-execution
unsafe extern "C" fn execute_module(module: *mut JSModule, capability: *mut JSPromiseCapability) -> JSCompletion {
    // 1. Assert: promiseCapability was not provided.
    assert!(capability.is_null());

    // 2. Let record be this WebAssembly Module Record.
    // SAFETY: The runtime passes the record.
    let record = unsafe { module_record_from_abi(module) };
    completion_into_abi(execute_webassembly_module_record(Agent::current().vm(), record))
}

fn execute_webassembly_module_record(vm: &Vm, record: &ModuleRecord) -> ThrowCompletionOr<()> {
    let record_data = data_of_webassembly_module_record(record);
    let realm = record.realm();

    // 3. Let module be record.[[ModuleSource]].[[Module]].
    let module = record_data.module_source.compiled_module().clone();

    // 4. Let imports be « ».
    let imports = libjs_rust::api::Root::new(vm, Imports::default());
    let mut imported_function_count = 0;

    // 5. For each (importedModuleName, name, importtype) in module_imports(module),
    for import in &module.imports {
        // NOTE: The following step only applies when integrating with the JS String Builtins proposal.
        // FIXME: 1. If Find a builtin with (importedModuleName, name) and builtins module.[[BuiltinSets]] is not null, then continue.

        // 2. Let importedModule be GetImportedModule(record, importedModuleName).
        let imported_module = record
            .as_cyclic_module()
            .expect("a host module is a Cyclic Module Record")
            .get_imported_module(&ModuleRequest::new(Utf16FlyString::from_utf8(&import.module)));

        // 3. Let resolution be importedModule.ResolveExport(name).
        let resolution = imported_module.resolve_export(vm, &Utf16FlyString::from_utf8(&import.name));

        // 4. Assert: resolution is a ResolvedBinding Record, as validated during environment initialization.
        assert!(resolution.is_valid());

        // 5. Let resolvedModule be resolution.\[[Module]].
        let resolved_module = resolution.module.expect("a resolved binding has a module");

        // 6. Let resolvedName be resolution.[[BindingName]].
        let resolved_name = &resolution.export_name;

        // 7. If resolvedModule is a WebAssembly Module Record,
        if is_webassembly_module_record(&resolved_module) {
            // 1. If resolvedModule.[[Instance]] is ~empty~, throw a {LinkError} exception.
            let Some(resolved_instance) = data_of_webassembly_module_record(&resolved_module).instance.get() else {
                return link_error(vm, realm, "Module has not been instantiated");
            };

            // 2. Assert: resolvedModule.[[Instance]] is a WebAssembly Instance object.
            // 3. Assert: resolvedModule.[[ModuleSource]] is a WebAssembly Module object.
            // 4. Let module be resolvedModule.[[ModuleSource]].[[Module]].
            // 5. Let externval be instance_export(resolvedModule.[[Instance]], resolvedName).
            // https://webassembly.github.io/spec/core/appendix/embedding.html#embed-instance-export
            // 6. Assert: externval is not error.
            let externval = Agent::the(vm)
                .store()
                .instance(resolved_instance.address())
                .export(&String::from_utf16_lossy(&resolved_name.to_utf16()))
                .expect("a WebAssembly module record exports what its export name list names");

            // 7. Assert: module_exports(module) contains an element (resolvedName, type).
            // 8. Let externtype be the value of type for the element (resolvedName, type) in module_exports(module).
            // 9. If importtype is not an extern subtype of externtype, throw a LinkError exception.
            // NB: Instantiation checks the type of every import, and throws that LinkError.

            // 10. Append externval to imports.
            imports.get().push(externval);
        }
        // 8. Otherwise,
        else {
            // 1. Let env be resolvedModule.[[Environment]].
            let environment = resolved_module
                .environment()
                .expect("a linked module has an environment");

            // 2. Let v be ?env.GetBindingValue(resolvedName, true).
            let v = environment.get_binding_value(vm, resolved_name, true)?;

            let import = match &import.description {
                // 3. If importtype is of the form func functype,
                ImportDescription::Function(type_index) => {
                    // 1. If IsCallable(v) is false, throw a LinkError exception.
                    if !v.is_function() {
                        let message = ErrorType::NotAFunction.message(&[&v.to_utf16_string_without_side_effects()]);
                        return link_error(vm, realm, &String::from_utf16_lossy(&message.to_utf16()));
                    }

                    // 2. If v has a [[FunctionAddress]] internal slot, and therefore is an Exported Function,
                    //     1. Let funcaddr be the value of v’s [[FunctionAddress]] internal slot.
                    // 3. Otherwise,
                    //     1. Create a host function from v and functype, and let funcaddr be the result.
                    //     2. Let index be the number of external functions in imports, defining the index of the host function funcaddr.
                    let function = exported_function_address(v.as_object()).unwrap_or_else(|| {
                        create_host_function(
                            vm,
                            v.as_function(),
                            function_type_in_store(vm, &module, *type_index),
                            imported_function_count,
                        )
                    });

                    // 4. Let externfunc be the external value func funcaddr.
                    // 5. Append externfunc to imports.
                    ExternValue::Function(function)
                }

                // 4. If importtype is of the form global mut valtype,
                ImportDescription::Global(ty) => {
                    let GlobalType {
                        value: valtype,
                        mutable,
                    } = global_type_in_store(vm, &module, *ty);

                    // 1. Let store be the surrounding agent’s associated store.
                    // 2. If v implements Global,
                    let global = if let Some(global) = Global::from_value(v) {
                        // 1. Let globaladdr be v.[[Global]].
                        let global = global.address();

                        // 2. Let targetmut valuetype be global_type(store, globaladdr).
                        let target_is_mutable = Agent::the(vm).store().global(global).ty.mutable;

                        // 3. If mut is const and targetmut is var, throw a LinkError exception.
                        if !mutable && target_is_mutable {
                            return link_error(vm, realm, "Mutable globals are not supported for immutable imports");
                        }
                        global
                    }
                    // 3. Otherwise,
                    else {
                        // AD-HOC: If valtype is i64 and v is a Number, throw a LinkError exception.
                        if valtype == ValType::I64 && v.is_number() {
                            return link_error(
                                vm,
                                realm,
                                "Import resolution attempted to cast a Number to a BigInteger",
                            );
                        }

                        // AD-HOC: If valtype is not i64 and v is a BigInt, throw a LinkError exception.
                        if valtype != ValType::I64 && v.is_bigint() {
                            return link_error(
                                vm,
                                realm,
                                "Import resolution attempted to cast a BigInteger to a Number",
                            );
                        }

                        // 1. If valtype is v128, throw a LinkError exception.
                        if valtype == ValType::V128 {
                            return link_error(vm, realm, "V128 is not supported as a global value type");
                        }

                        // 2. If mut is var, throw a LinkError exception.
                        if mutable {
                            return link_error(vm, realm, "Variable global value types are not supported");
                        }

                        // 3. Let value be ?ToWebAssemblyValue(v, valtype).
                        let value = to_webassembly_value(vm, v, valtype)?;

                        // 4. Let(store, globaladdr) be global_alloc(store, mut valtype, value).
                        // 5. Set the surrounding agent’s associated store to store.
                        Agent::the(vm).store().allocate_global(
                            GlobalType {
                                value: valtype,
                                mutable,
                            },
                            value,
                        )
                    };

                    // 4. Let externglobal be global globaladdr.
                    // 5. Append externglobal to imports.
                    ExternValue::Global(global)
                }

                // 5. If importtype is of the form mem memtype,
                ImportDescription::Memory(_) => {
                    // 1. If v does not implement Memory, throw a LinkError exception.
                    let Some(memory) = Memory::from_value(v) else {
                        return link_error(
                            vm,
                            realm,
                            "Expected an instance of WebAssembly.Memory for a memory import",
                        );
                    };

                    // 2. Let externmem be the external value mem v.[[Memory]].
                    // 3. Append externmem to imports.
                    ExternValue::Memory(memory.address())
                }

                // 6. If importtype is of the form table tabletype,
                ImportDescription::Table(_) => {
                    // 1. If v does not implement Table, throw a LinkError exception.
                    let Some(table) = Table::from_value(v) else {
                        return link_error(
                            vm,
                            realm,
                            "Expected an instance of WebAssembly.Table for a table import",
                        );
                    };

                    // 2. Let tableaddr be v.[[Table]].
                    // 3. Let externtable be the external value table tableaddr.
                    // 4. Append externtable to imports.
                    ExternValue::Table(table.address())
                }

                // 7. If importtype is of the form tag attribute functype,
                ImportDescription::Tag(_) => {
                    // 1. Assert: attribute is exception.
                    // 2. If v does not implement Tag, throw a LinkError exception.
                    let Some(tag) = Tag::from_value(v) else {
                        return link_error(vm, realm, "Expected an instance of WebAssembly.Tag for a tag import");
                    };

                    // 3. Let tagaddr be v.[[Address]].
                    // 4. Let externtag be the external value tag tagaddr.
                    // 5. Append externtag to imports.
                    ExternValue::Tag(tag.address())
                }
            };
            imports.get().push(import);
        }

        if matches!(import.description, ImportDescription::Function(_)) {
            imported_function_count += 1;
        }
    }

    // 6. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
    let instance_address = instantiate_the_core_of_a_webassembly_module(vm, &module, imports.get())?;

    // 7. Set record.[[Instance]] to instance.
    let instance = allocate(vm, |_| Instance::initialize(vm, realm, instance_address));
    record_data.instance.set(Some(instance));

    let environment = record.environment().expect("an evaluated module has an environment");

    // 8. For each (name, externtype) of module_exports(module),
    // NB: Converting a global's value may reach the store, so nothing of it stays borrowed across the conversion.
    for export in &module.exports {
        let name = Utf16FlyString::from_utf8(&export.name);
        let externval = Agent::the(vm)
            .store()
            .instance(instance_address)
            .export(&export.name)
            .expect("an instance has every export of its module");

        // 1. If externtype is of the form global mut globaltype,
        if let ExternValue::Global(global) = externval {
            // 1. Assert: externval is of the form global globaladdr.
            // 2. Let global globaladdr be externval.
            // 3. Let global_value be global_read(store, globaladdr).
            let (global_value, globaltype) = {
                let global = Agent::the(vm).store().global(global);
                (global.get(), global.ty.value)
            };

            // 4. If globaltype is not v128,
            if globaltype != ValType::V128 {
                // NOTE: The condition above leaves unsupported JS values as uninitialized in TDZ and therefore as a
                //       reference error on access. When integrating with shared globals, they may be excluded here
                //       similarly to v128 above.

                // 1. Perform !record.[[Environment]].InitializeBinding(name, ToJSValue(global_value)).
                let value = to_js_value(vm, global_value, globaltype)?;
                environment
                    .initialize_binding(vm, &name, value, InitializeBindingHint::Normal)
                    .must();

                // FIXME: 2. If mut is var, then associate all future mutations of globaladdr with the ECMA-262 binding record
                //        for name in record.[[Environment]], such that record.[[Environment]].GetBindingValue(resolution.[[BindingName]], true)
                //        always returns ToJSValue(global_read(store, globaladdr)) for the current surrounding agent’s associated store store.
            }
        }
        // 2. Otherwise,
        else {
            // 1. Perform !record.[[Environment]].InitializeBinding(name, !Get(instance.[[Exports]], name)).
            let exports = instance
                .exports(&mut crate::bindings::CallContext::new(vm, realm))
                .ok()
                .expect("an instance has its exports");
            let value = exports
                .get(vm, &libjs_rust::api::PropertyKey::from(name.clone()))
                .must();
            environment
                .initialize_binding(vm, &name, value, InitializeBindingHint::Normal)
                .must();
        }
    }

    // NOTE: The linking semantics here for Wasm to Wasm modules are identical to the WebAssembly JS API semantics as if
    //       passing the the exports object as the imports object in instantiation. When linking Wasm module imports to
    //       JS module exports, the JS API semantics are exactly followed as well. It is only in the case of importing
    //       Wasm from JS that WebAssembly.Global unwrapping is observable on the WebAssembly Module Record Environment
    //       Record.

    Ok(())
}
