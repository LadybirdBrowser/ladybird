/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebAssembly namespace, https://webassembly.github.io/spec/js-api/#webassembly-namespace, and its streaming
//! functions, https://webassembly.github.io/spec/web-api/index.html#streaming-modules

use core::cell::RefCell;
use core::ffi::c_void;
use std::sync::Arc;

use libjs_rust::api::{
    Attribute, EmbedderClass, EmbedderValue, Gc, Object, PropertyAttributes, PropertyKey, RawNativeFunctionResult,
    Realm, Trace, Value, Visitor, Vm,
};

#[cfg(web_idl_bindings)]
use crate::bindings::conversions::ToJs;
use crate::bindings::conversions::{BufferSource, UsvString};
use crate::bindings::interface::{Ref, allocate, wrap};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, in_parallel, queue_a_task, react_to_promise, reject_promise,
    resolve_promise,
};
use crate::bindings::{CallContext, WebIdlResult};
use crate::web_assembly::builtins::validate_builtins_and_imported_strings;
use crate::web_assembly::errors::{WebAssemblyError, error_constructor, throw_webassembly_error};
use crate::web_assembly::instance::{Instance, instantiate_the_core_of_a_webassembly_module, read_the_imports};
use crate::web_assembly::module::Module;
use crate::web_assembly::tag::Tag;
use crate::web_assembly::{Agent, CompiledModule, compile_a_webassembly_module, stable_bytes_of};

/// https://webassembly.github.io/spec/js-api/#dictdef-webassemblyinstantiatedsource
pub(crate) struct WebAssemblyInstantiatedSource {
    pub(crate) module: Ref<Module>,
    pub(crate) instance: Ref<Instance>,
}

impl WebAssemblyInstantiatedSource {
    fn into_js(self, cx: &CallContext) -> libjs_rust::api::ThrowCompletionOr<Value> {
        // NB: The generated bindings convert the dictionary, which a build of the crate on its own leaves out.
        #[cfg(web_idl_bindings)]
        return ToJs::to_js(self, cx);
        #[cfg(not(web_idl_bindings))]
        {
            let _ = (self, cx);
            unreachable!("only the generated bindings resolve a promise with the dictionary")
        }
    }
}

/// https://webassembly.github.io/spec/js-api/#dictdef-webassemblycompileoptions
#[derive(Clone, Default, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebAssemblyCompileOptions {
    #[gc(untraced)]
    pub(crate) imported_string_constants: Option<UsvString>,
    #[gc(untraced)]
    pub(crate) builtins: Vec<UsvString>,
}

static COMPILE_OPTIONS_CLASS: EmbedderClass<WebAssemblyCompileOptions> =
    EmbedderClass::new("WebAssemblyCompileOptions");

impl EmbedderValue for WebAssemblyCompileOptions {
    const CLASS: &'static EmbedderClass<Self> = &COMPILE_OPTIONS_CLASS;
}

pub(crate) struct WebAssembly;

/// The bytes that asynchronously compiling a module compiles in parallel, the options it compiles them with, and what
/// compiling them results in.
struct CompileJob {
    bytes: RefCell<Vec<u8>>,
    options: RefCell<WebAssemblyCompileOptions>,
    result: RefCell<Option<Result<CompiledModule, Value>>>,
}

// SAFETY: The exception that compiling the bytes throws is the one cell the job holds.
unsafe impl Trace for CompileJob {
    fn trace(&self, visitor: &mut Visitor) {
        if let Some(Err(exception)) = &*self.result.borrow() {
            exception.trace(visitor);
        }
    }
}

static COMPILE_JOB_CLASS: EmbedderClass<CompileJob> = EmbedderClass::new("WebAssemblyCompileJob");

impl EmbedderValue for CompileJob {
    const CLASS: &'static EmbedderClass<Self> = &COMPILE_JOB_CLASS;
}

/// https://webassembly.github.io/spec/js-api/#asynchronously-compile-a-webassembly-module
fn asynchronously_compile_a_webassembly_module(
    vm: &Vm,
    realm: Gc<Realm>,
    bytes: Vec<u8>,
    options: WebAssemblyCompileOptions,
    task_source: TaskSource,
) -> WebIdlPromise {
    // 1. Let promise be a new Promise.
    let promise = create_promise(vm, realm);

    // 2. Run the following steps in parallel:
    let job = COMPILE_JOB_CLASS.allocate(vm, |_| CompileJob {
        bytes: RefCell::new(bytes),
        options: RefCell::new(options),
        result: RefCell::new(None),
    });
    in_parallel(
        vm,
        realm,
        (promise, job, task_source),
        |vm, (promise, job, task_source)| {
            // 1. Compile the WebAssembly module bytes and store the result as module.
            let realm = vm.current_realm().expect("the steps run in the realm of the promise");
            let result = compile_a_webassembly_module(vm, realm, &job.bytes.borrow()).map_err(|throw| throw.value());
            *job.result.borrow_mut() = Some(result);

            // 2. Queue a task on taskSource, if provided, to perform the following steps:
            queue_a_task(vm, realm, task_source, (promise, job), |vm, (promise, job)| {
                let realm = crate::bindings::promise::promise_realm(promise);

                // 1. If module is error, reject promise with a CompileError exception and return.
                let module = match job.result.take().expect("the module is compiled before the task runs") {
                    Ok(module) => module,
                    Err(exception) => {
                        reject_promise(vm, promise, exception);
                        return;
                    }
                };

                let options = job.options.take();

                // 2. Let builtinSetNames be options["builtins"].
                let builtin_set_names = options.builtins;

                // 3. Let importedStringModule be options["importedStringConstants"].
                let imported_string_module = options.imported_string_constants;

                // 4. If validating builtins and imported strings for module with builtinSetNames and importedStringModule is
                //    false, reject promise with a CompileError exception.
                if !validate_builtins_and_imported_strings(
                    vm,
                    &module,
                    &builtin_set_names,
                    imported_string_module.as_ref(),
                ) {
                    let throw = throw_webassembly_error(
                        vm,
                        realm,
                        WebAssemblyError::Compile,
                        "Invalid builtin or imported string",
                    );
                    reject_promise(vm, promise, throw.value());
                }
                // 5. Otherwise,
                else {
                    // 1. Construct a WebAssembly module object from module, bytes, builtinSetNames, importedStringModule, and
                    //    let moduleObject be the result.
                    let bytes = job.bytes.take();
                    let module_object = allocate(vm, |_| {
                        Module::new(module, bytes, builtin_set_names, imported_string_module)
                    });

                    // 2. Resolve promise with moduleObject.
                    let cx = CallContext::new(vm, realm);
                    resolve_promise(vm, promise, Value::from_object(wrap(&cx, module_object)));
                }
            });
        },
    );

    // 3. Return promise.
    promise
}

/// https://webassembly.github.io/spec/js-api/#asynchronously-instantiate-a-webassembly-module
fn asynchronously_instantiate_a_webassembly_module(
    vm: &Vm,
    realm: Gc<Realm>,
    module_object: Ref<Module>,
    import_object: Option<Gc<Object>>,
) -> WebIdlPromise {
    // 1. Let promise be a new promise.
    let promise = create_promise(vm, realm);

    // 2. Let module be moduleObject.[[Module]].
    let module = module_object.compiled_module().clone();

    // 3. Let builtinSetNames be moduleObject.[[BuiltinSets]].
    let builtin_set_names = module_object.builtin_sets();

    // 4. Let importedStringModule be moduleObject.[[ImportedStringModule]].
    let imported_string_module = module_object.imported_string_module();

    // 5. Read the imports of module with imports importObject, builtinSetNames and importedStringModule, and let imports
    //    be the result. If this operation throws an exception, catch it, reject promise with the exception, and return
    //    promise.
    let imports = match read_the_imports(vm, &module, import_object, builtin_set_names, imported_string_module) {
        Ok(imports) => imports,
        Err(throw) => {
            reject_promise(vm, promise, throw.value());
            return promise;
        }
    };

    // 6. Run the following steps in parallel:
    //   1. Queue a task to perform the following steps: Note: Implementation-specific work may be performed here.
    queue_a_task(
        vm,
        realm,
        TaskSource::Unspecified,
        (promise, module_object, imports),
        |vm, (promise, module_object, imports)| {
            let realm = crate::bindings::promise::promise_realm(promise);
            let module = module_object.compiled_module().clone();

            // 1. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
            //    If this throws an exception, catch it, reject promise with the exception, and terminate these substeps.
            let instance = match instantiate_the_core_of_a_webassembly_module(vm, &module, &imports) {
                Ok(instance) => instance,
                Err(throw) => {
                    reject_promise(vm, promise, throw.value());
                    return;
                }
            };

            // 2. Let instanceObject be a new Instance.
            // 3. Initialize instanceObject from module and instance. If this throws an exception, catch it, reject promise with the exception, and terminate these substeps.
            let instance_object = allocate(vm, |_| Instance::initialize(vm, realm, instance));

            // 4. Resolve promise with instanceObject.
            let cx = CallContext::new(vm, realm);
            resolve_promise(vm, promise, Value::from_object(wrap(&cx, instance_object)));
        },
    );

    // 7. Return promise.
    promise
}

/// https://webassembly.github.io/spec/js-api/#instantiate-a-promise-of-a-module
fn instantiate_a_promise_of_a_module(
    vm: &Vm,
    realm: Gc<Realm>,
    promise_of_module: WebIdlPromise,
    import_object: Option<Gc<Object>>,
) -> WebIdlPromise {
    // 1. Let promise be a new Promise.
    let promise = create_promise(vm, realm);

    // FIXME: Spec should use react to promise here instead of separate upon fulfillment and upon rejection steps

    react_to_promise(
        vm,
        promise_of_module,
        (promise, import_object),
        // 2. Upon fulfillment of promiseOfModule with value module:
        Some(|cx: &CallContext, (promise, import_object), module_value: Value| {
            let module_object = Module::from_value(module_value).expect("the promise is fulfilled with a Module");

            // 1. Instantiate the WebAssembly module module importing importObject, and let innerPromise be the result.
            let realm = crate::bindings::promise::promise_realm(promise);
            let inner_promise =
                asynchronously_instantiate_a_webassembly_module(cx.vm(), realm, module_object, import_object);

            react_to_promise(
                cx.vm(),
                inner_promise,
                (promise, module_object),
                // 2. Upon fulfillment of innerPromise with value instance.
                Some(|cx: &CallContext, (promise, module_object), instance_value: Value| {
                    let instance = crate::web_assembly::implementation_of_value::<Instance>(instance_value)
                        .expect("the promise is fulfilled with an Instance");

                    // 1. Let result be the WebAssemblyInstantiatedSource value «[ "module" → module, "instance" → instance ]».
                    let result = WebAssemblyInstantiatedSource {
                        module: module_object,
                        instance,
                    }
                    .into_js(&CallContext::new(
                        cx.vm(),
                        crate::bindings::promise::promise_realm(promise),
                    ))?;

                    // 2. Resolve promise with result.
                    resolve_promise(cx.vm(), promise, result);
                    Ok(Value::UNDEFINED)
                }),
                // 3. Upon rejection of innerPromise with reason reason.
                Some(|cx: &CallContext, (promise, _), reason: Value| {
                    // 1. Reject promise with reason.
                    reject_promise(cx.vm(), promise, reason);
                    Ok(Value::UNDEFINED)
                }),
            );
            Ok(Value::UNDEFINED)
        }),
        // 3. Upon rejection of promiseOfModule with reason reason:
        Some(|cx: &CallContext, (promise, _), reason: Value| {
            // 1. Reject promise with reason.
            reject_promise(cx.vm(), promise, reason);
            Ok(Value::UNDEFINED)
        }),
    );

    // 4. Return promise.
    promise
}

unsafe extern "C" {
    /// Steps 1 to 8 of compiling a potential WebAssembly response: the checks of the Response object, then consuming
    /// its body as an ArrayBuffer, whose promise, a WebIDL::Promise, it writes. Throws the TypeError of a check that
    /// fails.
    fn web_bindings_consume_webassembly_response(
        realm: *mut c_void,
        unwrapped_source: u64,
        body_promise: *mut *mut c_void,
    ) -> RawNativeFunctionResult;
}

/// https://webassembly.github.io/spec/web-api/index.html#compile-a-potential-webassembly-response
fn compile_a_potential_webassembly_response(
    vm: &Vm,
    realm: Gc<Realm>,
    source: WebIdlPromise,
    options: WebAssemblyCompileOptions,
) -> WebIdlPromise {
    // Note: This algorithm accepts a Response object, or a promise for one, and compiles and instantiates the resulting bytes of the response.
    //       This compilation can be performed in the background and in a streaming manner.
    //       If the Response is not CORS-same-origin, does not represent an ok status, or does not match the `application/wasm` MIME type,
    //       the returned promise will be rejected with a TypeError; if compilation or instantiation fails,
    //       the returned promise will be rejected with a CompileError or other relevant error type, depending on the cause of failure.

    // 1. Let returnValue be a new promise
    let return_value = create_promise(vm, realm);

    // NB: The reactions hold the options in a cell of their own, as what they capture is traced.
    let options = COMPILE_OPTIONS_CLASS.allocate(vm, |_| options);

    react_to_promise(
        vm,
        source,
        (return_value, options),
        // 2. Upon fulfillment of source with value unwrappedSource:
        Some(|cx: &CallContext, (return_value, options), unwrapped_source: Value| {
            let realm = crate::bindings::promise::promise_realm(return_value);
            let mut body_promise = core::ptr::null_mut();
            // SAFETY: The realm is live, and the promise is written before the completion is normal.
            let consumed = unsafe {
                web_bindings_consume_webassembly_response(
                    realm.as_ptr().cast(),
                    unwrapped_source.to_raw(),
                    &raw mut body_promise,
                )
            }
            .into_completion();
            if let Err(throw) = consumed {
                reject_promise(cx.vm(), return_value, throw.value());
                return Ok(Value::UNDEFINED);
            }
            // SAFETY: LibWeb wrote the WebIDL::Promise of the body.
            let body_promise = unsafe { crate::bindings::promise::promise_from_abi(body_promise) };

            react_to_promise(
                cx.vm(),
                body_promise,
                (return_value, options),
                // 9. Upon fulfillment of bodyPromise with value bodyArrayBuffer:
                Some(|cx: &CallContext, (return_value, options), body_array_buffer: Value| {
                    // 1. Let stableBytes be a copy of the bytes held by the buffer bodyArrayBuffer.
                    let bytes = crate::bindings::conversions::buffer_source_type_of(body_array_buffer)
                        .expect("a body consumed as an ArrayBuffer is one");
                    let stable_bytes = stable_bytes_of(bytes);

                    // 2. Asynchronously compile the WebAssembly module stableBytes using the networking task source and options
                    //    and resolve returnValue with the result.
                    let realm = crate::bindings::promise::promise_realm(return_value);
                    let result = asynchronously_compile_a_webassembly_module(
                        cx.vm(),
                        realm,
                        stable_bytes,
                        (*options).clone(),
                        TaskSource::Networking,
                    );
                    resolve_promise(cx.vm(), return_value, Value::from_object(result.promise()));
                    Ok(Value::UNDEFINED)
                }),
                // 10. Upon rejection of bodyPromise with reason reason:
                Some(|cx: &CallContext, (return_value, _), reason: Value| {
                    // 1. Reject returnValue with reason.
                    reject_promise(cx.vm(), return_value, reason);
                    Ok(Value::UNDEFINED)
                }),
            );
            Ok(Value::UNDEFINED)
        }),
        // 3. Upon rejection of source with reason reason:
        Some(|cx: &CallContext, (return_value, _), reason: Value| {
            // 1. Reject returnValue with reason.
            reject_promise(cx.vm(), return_value, reason);
            Ok(Value::UNDEFINED)
        }),
    );

    // 4. Return returnValue.
    return_value
}

impl WebAssembly {
    // https://webassembly.github.io/spec/js-api/#error-objects
    pub(crate) fn initialize(cx: &mut CallContext, namespace_object: Gc<Object>) {
        let vm = cx.vm();

        // 1. Let namespaceObject be the namespace object.
        // 2. For each error of « "CompileError", "LinkError", "RuntimeError" »,
        // 2.1. Let constructor be a new object, implementing the NativeError Object Structure, with NativeError set to error.
        // 2.2. ! DefineMethodProperty(namespaceObject, error, constructor, false).
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        for error in WebAssemblyError::ALL {
            let constructor = error_constructor(vm, cx.realm(), error);
            namespace_object.define_direct_property(
                vm,
                &PropertyKey::from_utf8(error.name()),
                Value::from_object(constructor),
                attributes,
            );
        }
    }

    // https://webassembly.github.io/spec/js-api/#dom-webassembly-jstag
    pub(crate) fn js_tag(cx: &mut CallContext) -> WebIdlResult<Ref<Tag>> {
        // 1. Let JSTagAddr be the result of getting the JavaScript exception tag.
        // 2. Let JSTagObject be the result of creating a Tag object from JSTagAddr.
        // 3. Return JSTagObject.
        let tag = Agent::the(cx.vm()).js_tag();
        Ok(Tag::from_address(cx.vm(), cx.realm(), tag))
    }

    // https://webassembly.github.io/spec/js-api/#dom-webassembly-validate
    pub(crate) fn validate(
        cx: &mut CallContext,
        bytes: BufferSource,
        options: WebAssemblyCompileOptions,
    ) -> WebIdlResult<bool> {
        // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
        let stable_bytes = stable_bytes_of(bytes);

        // 2. Compile stableBytes as a WebAssembly module and store the results as module.
        let module = libwasm_engine::compile(&stable_bytes);

        // 3. If module is error, return false.
        let Ok(module) = module else {
            return Ok(false);
        };
        let module: CompiledModule = Arc::new(module);

        // 4. Let builtinSetNames be options["builtins"].
        let builtin_set_names = &options.builtins;

        // 5. Let importedStringModule be options["importedStringConstants"].
        let imported_string_module = options.imported_string_constants.as_ref();

        // 6. If validating builtins and imported strings for module with builtinSetNames and importedStringModule returns
        //    false, return false.
        if !validate_builtins_and_imported_strings(cx.vm(), &module, builtin_set_names, imported_string_module) {
            return Ok(false);
        }

        // 7. Return true.
        Ok(true)
    }

    // https://webassembly.github.io/spec/js-api/#dom-webassembly-compile
    pub(crate) fn compile(
        cx: &mut CallContext,
        bytes: BufferSource,
        options: WebAssemblyCompileOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
        let stable_bytes = stable_bytes_of(bytes);

        // 2. Asynchronously compile a WebAssembly module from stableBytes using options and return the result.
        Ok(asynchronously_compile_a_webassembly_module(
            cx.vm(),
            cx.realm(),
            stable_bytes,
            options,
            TaskSource::Unspecified,
        ))
    }

    // https://webassembly.github.io/spec/web-api/index.html#dom-webassembly-compilestreaming
    pub(crate) fn compile_streaming(
        cx: &mut CallContext,
        source: WebIdlPromise,
        options: WebAssemblyCompileOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        // The compileStreaming(source, options) method, when invoked, returns the result of compiling a potential
        // WebAssembly response with source using options.
        Ok(compile_a_potential_webassembly_response(
            cx.vm(),
            cx.realm(),
            source,
            options,
        ))
    }

    // https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate
    // https://webassembly.github.io/content-security-policy/js-api/#dom-webassembly-instantiate
    pub(crate) fn instantiate_bytes(
        cx: &mut CallContext,
        bytes: BufferSource,
        import_object: Option<Gc<Object>>,
        options: WebAssemblyCompileOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
        let stable_bytes = stable_bytes_of(bytes);

        // 2. Perform HostEnsureCanCompileWasmBytes()
        crate::web_assembly::host_ensure_can_compile_wasm_bytes(realm)?;

        // 3. Asynchronously compile a WebAssembly module from stableBytes using options and let promiseOfModule be the
        //    result.
        let promise_of_module =
            asynchronously_compile_a_webassembly_module(vm, realm, stable_bytes, options, TaskSource::Unspecified);

        // 4. Instantiate promiseOfModule with imports importObject and return the result.
        Ok(instantiate_a_promise_of_a_module(
            vm,
            realm,
            promise_of_module,
            import_object,
        ))
    }

    // https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate-moduleobject-importobject
    pub(crate) fn instantiate_module(
        cx: &mut CallContext,
        module_object: Ref<Module>,
        import_object: Option<Gc<Object>>,
        _: Option<Value>,
    ) -> WebIdlResult<WebIdlPromise> {
        // 1. Asynchronously instantiate the WebAssembly module moduleObject importing importObject, and return the result.
        Ok(asynchronously_instantiate_a_webassembly_module(
            cx.vm(),
            cx.realm(),
            module_object,
            import_object,
        ))
    }

    // https://webassembly.github.io/spec/web-api/index.html#dom-webassembly-instantiatestreaming
    pub(crate) fn instantiate_streaming(
        cx: &mut CallContext,
        source: WebIdlPromise,
        import_object: Option<Gc<Object>>,
        options: WebAssemblyCompileOptions,
    ) -> WebIdlResult<WebIdlPromise> {
        // The instantiateStreaming(source, importObject, options) method, when invoked, performs the following steps:

        // 1. Let promiseOfModule be the result of compiling a potential WebAssembly response with source using options.
        let promise_of_module = compile_a_potential_webassembly_response(cx.vm(), cx.realm(), source, options);

        // 2. Return the result of instantiating the promise of a module promiseOfModule with imports importObject.
        Ok(instantiate_a_promise_of_a_module(
            cx.vm(),
            cx.realm(),
            promise_of_module,
            import_object,
        ))
    }
}
