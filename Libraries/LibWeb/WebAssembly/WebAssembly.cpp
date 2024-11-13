/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWeb/Bindings/ResponsePrototype.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAssembly {

static JS::NonnullGCPtr<WebIDL::Promise> asynchronously_compile_webassembly_module(JS::VM&, ByteBuffer, HTML::Task::Source = HTML::Task::Source::Unspecified);
static JS::NonnullGCPtr<WebIDL::Promise> instantiate_promise_of_module(JS::VM&, JS::NonnullGCPtr<WebIDL::Promise>, JS::GCPtr<JS::Object> import_object);
static JS::NonnullGCPtr<WebIDL::Promise> asynchronously_instantiate_webassembly_module(JS::VM&, JS::NonnullGCPtr<Module>, JS::GCPtr<JS::Object> import_object);
static JS::NonnullGCPtr<WebIDL::Promise> compile_potential_webassembly_response(JS::VM&, JS::NonnullGCPtr<WebIDL::Promise>);

namespace Detail {

HashMap<JS::GCPtr<JS::Object>, WebAssemblyCache> s_caches;

WebAssemblyCache& get_cache(JS::Realm& realm)
{
    return s_caches.ensure(realm.global_object());
}

}

void visit_edges(JS::Object& object, JS::Cell::Visitor& visitor)
{
    auto& global_object = HTML::relevant_global_object(object);
    if (auto maybe_cache = Detail::s_caches.get(global_object); maybe_cache.has_value()) {
        auto& cache = maybe_cache.release_value();
        visitor.visit(cache.function_instances());
        visitor.visit(cache.imported_objects());
        visitor.visit(cache.extern_values());
    }
}

void finalize(JS::Object& object)
{
    auto& global_object = HTML::relevant_global_object(object);
    Detail::s_caches.remove(global_object);
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-validate
bool validate(JS::VM& vm, JS::Handle<WebIDL::BufferSource>& bytes)
{
    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes = WebIDL::get_buffer_source_copy(*bytes->raw_object());
    if (stable_bytes.is_error()) {
        VERIFY(stable_bytes.error().code() == ENOMEM);
        return false;
    }

    // 2. Compile stableBytes as a WebAssembly module and store the results as module.
    auto module_or_error = Detail::compile_a_webassembly_module(vm, stable_bytes.release_value());

    // 3. If module is error, return false.
    if (module_or_error.is_error())
        return false;

    // 4. Return true.
    return true;
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-compile
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> compile(JS::VM& vm, JS::Handle<WebIDL::BufferSource>& bytes)
{
    auto& realm = *vm.current_realm();

    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes = WebIDL::get_buffer_source_copy(*bytes->raw_object());
    if (stable_bytes.is_error()) {
        VERIFY(stable_bytes.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }

    // 2. Asynchronously compile a WebAssembly module from stableBytes and return the result.
    return asynchronously_compile_webassembly_module(vm, stable_bytes.release_value());
}

// https://webassembly.github.io/spec/web-api/index.html#dom-webassembly-compilestreaming
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> compile_streaming(JS::VM& vm, JS::Handle<WebIDL::Promise> source)
{
    //  The compileStreaming(source) method, when invoked, returns the result of compiling a potential WebAssembly response with source.
    return compile_potential_webassembly_response(vm, *source);
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> instantiate(JS::VM& vm, JS::Handle<WebIDL::BufferSource>& bytes, Optional<JS::Handle<JS::Object>>& import_object_handle)
{
    auto& realm = *vm.current_realm();

    // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
    auto stable_bytes = WebIDL::get_buffer_source_copy(*bytes->raw_object());
    if (stable_bytes.is_error()) {
        VERIFY(stable_bytes.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }

    // 2. Asynchronously compile a WebAssembly module from stableBytes and let promiseOfModule be the result.
    auto promise_of_module = asynchronously_compile_webassembly_module(vm, stable_bytes.release_value());

    // 3. Instantiate promiseOfModule with imports importObject and return the result.
    JS::GCPtr<JS::Object> const import_object = import_object_handle.has_value() ? import_object_handle.value().ptr() : nullptr;
    return instantiate_promise_of_module(vm, promise_of_module, import_object);
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate-moduleobject-importobject
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> instantiate(JS::VM& vm, Module const& module_object, Optional<JS::Handle<JS::Object>>& import_object)
{
    // 1. Asynchronously instantiate the WebAssembly module moduleObject importing importObject, and return the result.
    JS::NonnullGCPtr<Module> module { const_cast<Module&>(module_object) };
    JS::GCPtr<JS::Object> const imports = import_object.has_value() ? import_object.value().ptr() : nullptr;
    return asynchronously_instantiate_webassembly_module(vm, module, imports);
}

// https://webassembly.github.io/spec/web-api/index.html#dom-webassembly-instantiatestreaming
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> instantiate_streaming(JS::VM& vm, JS::Handle<WebIDL::Promise> source, Optional<JS::Handle<JS::Object>>& import_object)
{
    // The instantiateStreaming(source, importObject) method, when invoked, performs the following steps:

    // 1. Let promiseOfModule be the result of compiling a potential WebAssembly response with source.
    auto promise_of_module = compile_potential_webassembly_response(vm, *source);

    // 2. Return the result of instantiating the promise of a module promiseOfModule with imports importObject.
    auto imports = JS::GCPtr { import_object.has_value() ? import_object.value().ptr() : nullptr };
    return instantiate_promise_of_module(vm, promise_of_module, imports);
}

namespace Detail {

JS::ThrowCompletionOr<NonnullOwnPtr<Wasm::ModuleInstance>> instantiate_module(JS::VM& vm, Wasm::Module const& module, JS::GCPtr<JS::Object> import_object)
{
    Wasm::Linker linker { module };
    HashMap<Wasm::Linker::Name, Wasm::ExternValue> resolved_imports;
    auto& cache = get_cache(*vm.current_realm());
    if (import_object) {
        dbgln_if(LIBWEB_WASM_DEBUG, "Trying to resolve stuff because import object was specified");
        for (Wasm::Linker::Name const& import_name : linker.unresolved_imports()) {
            dbgln_if(LIBWEB_WASM_DEBUG, "Trying to resolve {}::{}", import_name.module, import_name.name);
            auto value_or_error = import_object->get(import_name.module);
            if (value_or_error.is_error())
                break;
            auto value = value_or_error.release_value();
            auto object_or_error = value.to_object(vm);
            if (object_or_error.is_error())
                break;
            auto object = object_or_error.release_value();
            auto import_or_error = object->get(import_name.name);
            if (import_or_error.is_error())
                break;
            auto import_ = import_or_error.release_value();
            TRY(import_name.type.visit(
                [&](Wasm::TypeIndex index) -> JS::ThrowCompletionOr<void> {
                    dbgln_if(LIBWEB_WASM_DEBUG, "Trying to resolve a function {}::{}, type index {}", import_name.module, import_name.name, index.value());
                    auto& type = module.type_section().types()[index.value()];
                    // FIXME: IsCallable()
                    if (!import_.is_function())
                        return {};
                    auto& function = import_.as_function();
                    cache.add_imported_object(function);
                    // FIXME: If this is a function created by create_native_function(),
                    //        just extract its address and resolve to that.
                    Wasm::HostFunction host_function {
                        [&](auto&, auto& arguments) -> Wasm::Result {
                            JS::MarkedVector<JS::Value> argument_values { vm.heap() };
                            size_t index = 0;
                            for (auto& entry : arguments) {
                                argument_values.append(to_js_value(vm, entry, type.parameters()[index]));
                                ++index;
                            }

                            auto result = TRY(JS::call(vm, function, JS::js_undefined(), argument_values.span()));
                            if (type.results().is_empty())
                                return Wasm::Result { Vector<Wasm::Value> {} };

                            if (type.results().size() == 1)
                                return Wasm::Result { Vector<Wasm::Value> { TRY(to_webassembly_value(vm, result, type.results().first())) } };

                            auto method = TRY(result.get_method(vm, vm.names.iterator));
                            if (method == JS::js_undefined())
                                return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotIterable, result.to_string_without_side_effects());

                            auto values = TRY(JS::iterator_to_list(vm, TRY(JS::get_iterator_from_method(vm, result, *method))));

                            if (values.size() != type.results().size())
                                return vm.throw_completion<JS::TypeError>(ByteString::formatted("Invalid number of return values for multi-value wasm return of {} objects", type.results().size()));

                            Vector<Wasm::Value> wasm_values;
                            TRY_OR_THROW_OOM(vm, wasm_values.try_ensure_capacity(values.size()));

                            size_t i = 0;
                            for (auto& value : values)
                                wasm_values.append(TRY(to_webassembly_value(vm, value, type.results()[i++])));

                            return Wasm::Result { move(wasm_values) };
                        },
                        type,
                        ByteString::formatted("func{}", resolved_imports.size()),
                    };
                    auto address = cache.abstract_machine().store().allocate(move(host_function));
                    dbgln_if(LIBWEB_WASM_DEBUG, "Resolved to {}", address->value());
                    // FIXME: LinkError instead.
                    VERIFY(address.has_value());

                    resolved_imports.set(import_name, Wasm::ExternValue { Wasm::FunctionAddress { *address } });
                    return {};
                },
                [&](Wasm::GlobalType const& type) -> JS::ThrowCompletionOr<void> {
                    Optional<Wasm::GlobalAddress> address;
                    // https://webassembly.github.io/spec/js-api/#read-the-imports step 5.1
                    if (import_.is_number() || import_.is_bigint()) {
                        if (import_.is_number() && type.type().kind() == Wasm::ValueType::I64) {
                            // FIXME: Throw a LinkError instead.
                            return vm.throw_completion<JS::TypeError>("LinkError: Import resolution attempted to cast a Number to a BigInteger"sv);
                        }
                        if (import_.is_bigint() && type.type().kind() != Wasm::ValueType::I64) {
                            // FIXME: Throw a LinkError instead.
                            return vm.throw_completion<JS::TypeError>("LinkError: Import resolution attempted to cast a BigInteger to a Number"sv);
                        }
                        auto cast_value = TRY(to_webassembly_value(vm, import_, type.type()));
                        address = cache.abstract_machine().store().allocate({ type.type(), false }, cast_value);
                    } else {
                        // FIXME: https://webassembly.github.io/spec/js-api/#read-the-imports step 5.2
                        //        if v implements Global
                        //            let globaladdr be v.[[Global]]

                        // FIXME: Throw a LinkError instead
                        return vm.throw_completion<JS::TypeError>("LinkError: Invalid value for global type"sv);
                    }

                    resolved_imports.set(import_name, Wasm::ExternValue { *address });
                    return {};
                },
                [&](Wasm::MemoryType const&) -> JS::ThrowCompletionOr<void> {
                    if (!import_.is_object() || !is<WebAssembly::Memory>(import_.as_object())) {
                        // FIXME: Throw a LinkError instead
                        return vm.throw_completion<JS::TypeError>("LinkError: Expected an instance of WebAssembly.Memory for a memory import"sv);
                    }
                    auto address = static_cast<WebAssembly::Memory const&>(import_.as_object()).address();
                    resolved_imports.set(import_name, Wasm::ExternValue { address });
                    return {};
                },
                [&](Wasm::TableType const&) -> JS::ThrowCompletionOr<void> {
                    if (!import_.is_object() || !is<WebAssembly::Table>(import_.as_object())) {
                        // FIXME: Throw a LinkError instead
                        return vm.throw_completion<JS::TypeError>("LinkError: Expected an instance of WebAssembly.Table for a table import"sv);
                    }
                    auto address = static_cast<WebAssembly::Table const&>(import_.as_object()).address();
                    resolved_imports.set(import_name, Wasm::ExternValue { address });
                    return {};
                },
                [&](auto const&) -> JS::ThrowCompletionOr<void> {
                    // FIXME: Implement these.
                    dbgln("Unimplemented import of non-function attempted");
                    return vm.throw_completion<JS::TypeError>("LinkError: Not Implemented"sv);
                }));
        }
    }

    linker.link(resolved_imports);
    auto link_result = linker.finish();
    if (link_result.is_error()) {
        // FIXME: Throw a LinkError.
        StringBuilder builder;
        builder.append("LinkError: Missing "sv);
        builder.join(' ', link_result.error().missing_imports);
        return vm.throw_completion<JS::TypeError>(MUST(builder.to_string()));
    }

    auto instance_result = cache.abstract_machine().instantiate(module, link_result.release_value());
    if (instance_result.is_error()) {
        // FIXME: Throw a LinkError instead.
        return vm.throw_completion<JS::TypeError>(instance_result.error().error);
    }

    return instance_result.release_value();
}

// // https://webassembly.github.io/spec/js-api/#compile-a-webassembly-module
JS::ThrowCompletionOr<NonnullRefPtr<CompiledWebAssemblyModule>> compile_a_webassembly_module(JS::VM& vm, ByteBuffer data)
{
    FixedMemoryStream stream { data.bytes() };
    auto module_result = Wasm::Module::parse(stream);
    if (module_result.is_error()) {
        // FIXME: Throw CompileError instead.
        return vm.throw_completion<JS::TypeError>(Wasm::parse_error_to_byte_string(module_result.error()));
    }

    auto& cache = get_cache(*vm.current_realm());
    if (auto validation_result = cache.abstract_machine().validate(module_result.value()); validation_result.is_error()) {
        // FIXME: Throw CompileError instead.
        return vm.throw_completion<JS::TypeError>(validation_result.error().error_string);
    }
    auto compiled_module = make_ref_counted<CompiledWebAssemblyModule>(module_result.release_value());
    cache.add_compiled_module(compiled_module);
    return compiled_module;
}

JS::NativeFunction* create_native_function(JS::VM& vm, Wasm::FunctionAddress address, ByteString const& name, Instance* instance)
{
    auto& realm = *vm.current_realm();
    Optional<Wasm::FunctionType> type;
    auto& cache = get_cache(realm);
    cache.abstract_machine().store().get(address)->visit([&](auto const& value) { type = value.type(); });
    if (auto entry = cache.get_function_instance(address); entry.has_value())
        return *entry;

    auto function = JS::NativeFunction::create(
        realm,
        name,
        [address, type = type.release_value(), instance](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            (void)instance;
            auto& realm = *vm.current_realm();
            Vector<Wasm::Value> values;
            values.ensure_capacity(type.parameters().size());

            // Grab as many values as needed and convert them.
            size_t index = 0;
            for (auto& type : type.parameters())
                values.append(TRY(to_webassembly_value(vm, vm.argument(index++), type)));

            auto& cache = get_cache(realm);
            auto result = cache.abstract_machine().invoke(address, move(values));
            // FIXME: Use the convoluted mapping of errors defined in the spec.
            if (result.is_trap())
                return vm.throw_completion<JS::TypeError>(TRY_OR_THROW_OOM(vm, String::formatted("Wasm execution trapped (WIP): {}", result.trap().reason)));

            if (result.values().is_empty())
                return JS::js_undefined();

            if (result.values().size() == 1)
                return to_js_value(vm, result.values().first(), type.results().first());

            // Put result values into a JS::Array in reverse order.
            auto js_result_values = JS::MarkedVector<JS::Value> { realm.heap() };
            js_result_values.ensure_capacity(result.values().size());

            for (size_t i = result.values().size(); i > 0; i--) {
                // Safety: ensure_capacity is called just before this.
                js_result_values.unchecked_append(to_js_value(vm, result.values().at(i - 1), type.results().at(i - 1)));
            }

            return JS::Value(JS::Array::create_from(realm, js_result_values));
        });

    cache.add_function_instance(address, function);
    return function;
}

JS::ThrowCompletionOr<Wasm::Value> to_webassembly_value(JS::VM& vm, JS::Value value, Wasm::ValueType const& type)
{
    static ::Crypto::SignedBigInteger two_64 = "1"_sbigint.shift_left(64);

    switch (type.kind()) {
    case Wasm::ValueType::I64: {
        auto bigint = TRY(value.to_bigint(vm));
        auto value = bigint->big_integer().divided_by(two_64).remainder;
        VERIFY(value.unsigned_value().trimmed_length() <= 2);
        i64 integer = static_cast<i64>(value.unsigned_value().to_u64());
        if (value.is_negative())
            integer = -integer;
        return Wasm::Value { integer };
    }
    case Wasm::ValueType::I32: {
        auto _i32 = TRY(value.to_i32(vm));
        return Wasm::Value { static_cast<i32>(_i32) };
    }
    case Wasm::ValueType::F64: {
        auto number = TRY(value.to_double(vm));
        return Wasm::Value { static_cast<double>(number) };
    }
    case Wasm::ValueType::F32: {
        auto number = TRY(value.to_double(vm));
        return Wasm::Value { static_cast<float>(number) };
    }
    case Wasm::ValueType::FunctionReference: {
        if (value.is_null())
            return Wasm::Value(Wasm::ValueType { Wasm::ValueType::Kind::FunctionReference });

        if (value.is_function()) {
            auto& function = value.as_function();
            auto& cache = get_cache(*vm.current_realm());
            for (auto& entry : cache.function_instances()) {
                if (entry.value == &function)
                    return Wasm::Value { Wasm::Reference { Wasm::Reference::Func { entry.key, cache.abstract_machine().store().get_module_for(entry.key) } } };
            }
        }

        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Exported function");
    }
    case Wasm::ValueType::ExternReference: {
        if (value.is_null())
            return Wasm::Value(Wasm::ValueType { Wasm::ValueType::Kind::ExternReference });
        auto& cache = get_cache(*vm.current_realm());
        for (auto& entry : cache.extern_values()) {
            if (entry.value == value)
                return Wasm::Value { Wasm::Reference { Wasm::Reference::Extern { entry.key } } };
        }
        Wasm::ExternAddress extern_addr = cache.extern_values().size();
        cache.add_extern_value(extern_addr, value);
        return Wasm::Value { Wasm::Reference { Wasm::Reference::Extern { extern_addr } } };
    }
    case Wasm::ValueType::V128:
        return vm.throw_completion<JS::TypeError>("Cannot convert a vector value to a javascript value"sv);
    }

    VERIFY_NOT_REACHED();
}

Wasm::Value default_webassembly_value(JS::VM& vm, Wasm::ValueType type)
{
    switch (type.kind()) {
    case Wasm::ValueType::I32:
    case Wasm::ValueType::I64:
    case Wasm::ValueType::F32:
    case Wasm::ValueType::F64:
    case Wasm::ValueType::V128:
    case Wasm::ValueType::FunctionReference:
        return Wasm::Value(type);
    case Wasm::ValueType::ExternReference:
        return MUST(to_webassembly_value(vm, JS::js_undefined(), type));
    }
    VERIFY_NOT_REACHED();
}

// https://webassembly.github.io/spec/js-api/#tojsvalue
JS::Value to_js_value(JS::VM& vm, Wasm::Value& wasm_value, Wasm::ValueType type)
{
    auto& realm = *vm.current_realm();
    switch (type.kind()) {
    case Wasm::ValueType::I64:
        return realm.create<JS::BigInt>(::Crypto::SignedBigInteger { wasm_value.to<i64>() });
    case Wasm::ValueType::I32:
        return JS::Value(wasm_value.to<i32>());
    case Wasm::ValueType::F64:
        return JS::Value(wasm_value.to<double>());
    case Wasm::ValueType::F32:
        return JS::Value(static_cast<double>(wasm_value.to<float>()));
    case Wasm::ValueType::FunctionReference: {
        auto ref_ = wasm_value.to<Wasm::Reference>();
        if (ref_.ref().has<Wasm::Reference::Null>())
            return JS::js_null();
        auto address = ref_.ref().get<Wasm::Reference::Func>().address;
        auto& cache = get_cache(realm);
        auto* function = cache.abstract_machine().store().get(address);
        auto name = function->visit(
            [&](Wasm::WasmFunction& wasm_function) {
                auto index = *wasm_function.module().functions().find_first_index(address);
                return ByteString::formatted("func{}", index);
            },
            [](Wasm::HostFunction& host_function) {
                return host_function.name();
            });
        return create_native_function(vm, address, name);
    }
    case Wasm::ValueType::ExternReference: {
        auto ref_ = wasm_value.to<Wasm::Reference>();
        if (ref_.ref().has<Wasm::Reference::Null>())
            return JS::js_null();
        auto address = ref_.ref().get<Wasm::Reference::Extern>().address;
        auto& cache = get_cache(realm);
        auto value = cache.get_extern_value(address);
        return value.release_value();
    }
    case Wasm::ValueType::V128:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

}

// https://webassembly.github.io/spec/js-api/#asynchronously-compile-a-webassembly-module
JS::NonnullGCPtr<WebIDL::Promise> asynchronously_compile_webassembly_module(JS::VM& vm, ByteBuffer bytes, HTML::Task::Source task_source)
{
    auto& realm = *vm.current_realm();

    // 1. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(JS::create_heap_function(vm.heap(), [&vm, &realm, bytes = move(bytes), promise, task_source]() mutable {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 1. Compile the WebAssembly module bytes and store the result as module.
        auto module_or_error = Detail::compile_a_webassembly_module(vm, move(bytes));

        // 2. Queue a task to perform the following steps. If taskSource was provided, queue the task on that task source.
        HTML::queue_a_task(task_source, nullptr, nullptr, JS::create_heap_function(vm.heap(), [&realm, promise, module_or_error = move(module_or_error)]() mutable {
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto& realm = HTML::relevant_realm(*promise->promise());

            // 1. If module is error, reject promise with a CompileError exception.
            if (module_or_error.is_error()) {
                WebIDL::reject_promise(realm, promise, module_or_error.error_value());
            }

            // 2. Otherwise,
            else {
                // 1. Construct a WebAssembly module object from module and bytes, and let moduleObject be the result.
                // FIXME: Save bytes to the Module instance instead of moving into compile_a_webassembly_module
                auto module_object = realm.create<Module>(realm, module_or_error.release_value());

                // 2. Resolve promise with moduleObject.
                WebIDL::resolve_promise(realm, promise, module_object);
            }
        }));
    }));

    // 3. Return promise.
    return promise;
}

// https://webassembly.github.io/spec/js-api/#asynchronously-instantiate-a-webassembly-module
JS::NonnullGCPtr<WebIDL::Promise> asynchronously_instantiate_webassembly_module(JS::VM& vm, JS::NonnullGCPtr<Module> module_object, JS::GCPtr<JS::Object> import_object)
{
    auto& realm = *vm.current_realm();

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Let module be moduleObject.[[Module]].
    auto module = module_object->compiled_module();

    // 3. Read the imports of module with imports importObject, and let imports be the result.
    //    If this operation throws an exception, catch it, reject promise with the exception, and return promise.
    // Note: We do this at the same time as instantiation in instantiate_module.

    // 4. Run the following steps in parallel:
    //   1. Queue a task to perform the following steps: Note: Implementation-specific work may be performed here.
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, JS::create_heap_function(vm.heap(), [&vm, &realm, promise, module, import_object]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        auto& realm = HTML::relevant_realm(*promise->promise());

        // 1. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
        //    If this throws an exception, catch it, reject promise with the exception, and terminate these substeps.
        auto result = Detail::instantiate_module(vm, module->module, import_object);
        if (result.is_error()) {
            WebIDL::reject_promise(realm, promise, result.error_value());
            return;
        }
        auto instance = result.release_value();

        // 2. Let instanceObject be a new Instance.
        // 3. Initialize instanceObject from module and instance. If this throws an exception, catch it, reject promise with the exception, and terminate these substeps.
        // FIXME: Investigate whether we are doing all the proper steps for "initialize an instance object"
        auto instance_object = realm.create<Instance>(realm, move(instance));

        // 4. Resolve promise with instanceObject.
        WebIDL::resolve_promise(realm, promise, instance_object);
    }));

    // 5. Return promise.
    return promise;
}

// https://webassembly.github.io/spec/js-api/#instantiate-a-promise-of-a-module
JS::NonnullGCPtr<WebIDL::Promise> instantiate_promise_of_module(JS::VM& vm, JS::NonnullGCPtr<WebIDL::Promise> promise_of_module, JS::GCPtr<JS::Object> import_object)
{
    auto& realm = *vm.current_realm();

    // 1. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // FIXME: Spec should use react to promise here instead of separate upon fulfillment and upon rejection steps

    // 2. Upon fulfillment of promiseOfModule with value module:
    auto fulfillment_steps = JS::create_heap_function(vm.heap(), [&vm, promise, import_object](JS::Value module_value) -> WebIDL::ExceptionOr<JS::Value> {
        VERIFY(module_value.is_object() && is<Module>(module_value.as_object()));
        auto module = JS::NonnullGCPtr { static_cast<Module&>(module_value.as_object()) };

        // 1. Instantiate the WebAssembly module module importing importObject, and let innerPromise be the result.
        auto inner_promise = asynchronously_instantiate_webassembly_module(vm, module, import_object);

        // 2. Upon fulfillment of innerPromise with value instance.
        auto instantiate_fulfillment_steps = JS::create_heap_function(vm.heap(), [promise, module](JS::Value instance_value) -> WebIDL::ExceptionOr<JS::Value> {
            auto& realm = HTML::relevant_realm(*promise->promise());

            VERIFY(instance_value.is_object() && is<Instance>(instance_value.as_object()));
            auto instance = JS::NonnullGCPtr { static_cast<Instance&>(instance_value.as_object()) };

            // 1. Let result be the WebAssemblyInstantiatedSource value «[ "module" → module, "instance" → instance ]».
            auto result = JS::Object::create(realm, nullptr);
            result->define_direct_property("module", module, JS::default_attributes);
            result->define_direct_property("instance", instance, JS::default_attributes);

            // 2. Resolve promise with result.
            WebIDL::resolve_promise(realm, promise, result);

            return JS::js_undefined();
        });

        // 3. Upon rejection of innerPromise with reason reason.
        auto instantiate_rejection_steps = JS::create_heap_function(vm.heap(), [promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            auto& realm = HTML::relevant_realm(*promise->promise());

            // 1. Reject promise with reason.
            WebIDL::reject_promise(realm, promise, reason);

            return JS::js_undefined();
        });

        WebIDL::react_to_promise(inner_promise, instantiate_fulfillment_steps, instantiate_rejection_steps);

        return JS::js_undefined();
    });

    // 3. Upon rejection of promiseOfModule with reason reason:
    auto rejection_steps = JS::create_heap_function(vm.heap(), [promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        auto& realm = HTML::relevant_realm(*promise->promise());

        // 1. Reject promise with reason.
        WebIDL::reject_promise(realm, promise, reason);

        return JS::js_undefined();
    });

    WebIDL::react_to_promise(promise_of_module, fulfillment_steps, rejection_steps);

    // 4. Return promise.
    return promise;
}

// https://webassembly.github.io/spec/web-api/index.html#compile-a-potential-webassembly-response
JS::NonnullGCPtr<WebIDL::Promise> compile_potential_webassembly_response(JS::VM& vm, JS::NonnullGCPtr<WebIDL::Promise> source)
{
    auto& realm = *vm.current_realm();

    // Note: This algorithm accepts a Response object, or a promise for one, and compiles and instantiates the resulting bytes of the response.
    //       This compilation can be performed in the background and in a streaming manner.
    //       If the Response is not CORS-same-origin, does not represent an ok status, or does not match the `application/wasm` MIME type,
    //       the returned promise will be rejected with a TypeError; if compilation or instantiation fails,
    //       the returned promise will be rejected with a CompileError or other relevant error type, depending on the cause of failure.

    // 1. Let returnValue be a new promise
    auto return_value = WebIDL::create_promise(realm);

    // 2. Upon fulfillment of source with value unwrappedSource:
    auto fulfillment_steps = JS::create_heap_function(vm.heap(), [&vm, return_value](JS::Value unwrapped_source) -> WebIDL::ExceptionOr<JS::Value> {
        auto& realm = HTML::relevant_realm(*return_value->promise());

        // 1. Let response be unwrappedSource’s response.
        if (!unwrapped_source.is_object() || !is<Fetch::Response>(unwrapped_source.as_object())) {
            WebIDL::reject_promise(realm, return_value, *vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Response").value());
            return JS::js_undefined();
        }
        auto& response_object = static_cast<Fetch::Response&>(unwrapped_source.as_object());
        auto response = response_object.response();

        // 2. Let mimeType be the result of getting `Content-Type` from response’s header list.
        // 3. If mimeType is null, reject returnValue with a TypeError and abort these substeps.
        // 4. Remove all HTTP tab or space byte from the start and end of mimeType.
        // 5. If mimeType is not a byte-case-insensitive match for `application/wasm`, reject returnValue with a TypeError and abort these substeps.
        // Note: extra parameters are not allowed, including the empty `application/wasm;`.
        // FIXME: Validate these extra constraints that are not checked by extract_mime_type()
        if (auto mime = response->header_list()->extract_mime_type(); !mime.has_value() || mime.value().essence() != "application/wasm"sv) {
            WebIDL::reject_promise(realm, return_value, *vm.throw_completion<JS::TypeError>("Response does not match the application/wasm MIME type"sv).value());
            return JS::js_undefined();
        }

        // 6. If response is not CORS-same-origin, reject returnValue with a TypeError and abort these substeps.
        // https://html.spec.whatwg.org/#cors-same-origin
        auto type = response_object.type();
        if (type != Bindings::ResponseType::Basic && type != Bindings::ResponseType::Cors && type != Bindings::ResponseType::Default) {
            WebIDL::reject_promise(realm, return_value, *vm.throw_completion<JS::TypeError>("Response is not CORS-same-origin"sv).value());
            return JS::js_undefined();
        }

        // 7. If response’s status is not an ok status, reject returnValue with a TypeError and abort these substeps.
        if (!response_object.ok()) {
            WebIDL::reject_promise(realm, return_value, *vm.throw_completion<JS::TypeError>("Response does not represent an ok status"sv).value());
            return JS::js_undefined();
        }

        // 8. Consume response’s body as an ArrayBuffer, and let bodyPromise be the result.
        auto body_promise_or_error = response_object.array_buffer();
        if (body_promise_or_error.is_error()) {
            auto throw_completion = Bindings::dom_exception_to_throw_completion(realm.vm(), body_promise_or_error.release_error());
            WebIDL::reject_promise(realm, return_value, *throw_completion.value());
            return JS::js_undefined();
        }
        auto body_promise = body_promise_or_error.release_value();

        // 9. Upon fulfillment of bodyPromise with value bodyArrayBuffer:
        auto body_fulfillment_steps = JS::create_heap_function(vm.heap(), [&vm, return_value](JS::Value body_array_buffer) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Let stableBytes be a copy of the bytes held by the buffer bodyArrayBuffer.
            VERIFY(body_array_buffer.is_object());
            auto stable_bytes = WebIDL::get_buffer_source_copy(body_array_buffer.as_object());
            if (stable_bytes.is_error()) {
                VERIFY(stable_bytes.error().code() == ENOMEM);
                WebIDL::reject_promise(HTML::relevant_realm(*return_value->promise()), return_value, *vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)).value());
                return JS::js_undefined();
            }

            // 2. Asynchronously compile the WebAssembly module stableBytes using the networking task source and resolve returnValue with the result.
            auto result = asynchronously_compile_webassembly_module(vm, stable_bytes.release_value(), HTML::Task::Source::Networking);

            // Need to manually convert WebIDL promise to an ECMAScript value here to resolve
            WebIDL::resolve_promise(HTML::relevant_realm(*return_value->promise()), return_value, result->promise());

            return JS::js_undefined();
        });

        // 10. Upon rejection of bodyPromise with reason reason:
        auto body_rejection_steps = JS::create_heap_function(vm.heap(), [return_value](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Reject returnValue with reason.
            WebIDL::reject_promise(HTML::relevant_realm(*return_value->promise()), return_value, reason);
            return JS::js_undefined();
        });

        WebIDL::react_to_promise(body_promise, body_fulfillment_steps, body_rejection_steps);

        return JS::js_undefined();
    });

    // 3. Upon rejection of source with reason reason:
    auto rejection_steps = JS::create_heap_function(vm.heap(), [return_value](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Reject returnValue with reason.
        WebIDL::reject_promise(HTML::relevant_realm(*return_value->promise()), return_value, reason);

        return JS::js_undefined();
    });

    WebIDL::react_to_promise(source, fulfillment_steps, rejection_steps);

    // 4. Return returnValue.
    return return_value;
}

}
