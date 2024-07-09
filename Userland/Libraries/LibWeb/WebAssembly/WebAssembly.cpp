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
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAssembly {

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
    // Note: There's no need to copy the bytes here as the buffer data cannot change while we're compiling the module.

    // 2. Compile stableBytes as a WebAssembly module and store the results as module.
    auto module_or_error = Detail::parse_module(vm, bytes->raw_object());

    // 3. If module is error, return false.
    if (module_or_error.is_error())
        return false;

    // 3 continued - our "compile" step is lazy with validation, explicitly do the validation.
    auto compiled_module = module_or_error.release_value();
    auto& cache = Detail::get_cache(*vm.current_realm());
    if (cache.abstract_machine().validate(compiled_module->module).is_error())
        return false;

    // 4. Return true.
    return true;
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-compile
WebIDL::ExceptionOr<JS::Value> compile(JS::VM& vm, JS::Handle<WebIDL::BufferSource>& bytes)
{
    auto& realm = *vm.current_realm();

    // FIXME: This shouldn't block!
    auto compiled_module_or_error = Detail::parse_module(vm, bytes->raw_object());
    auto promise = JS::Promise::create(realm);

    if (compiled_module_or_error.is_error()) {
        promise->reject(*compiled_module_or_error.release_error().value());
    } else {
        auto module_object = vm.heap().allocate<Module>(realm, realm, compiled_module_or_error.release_value());
        promise->fulfill(module_object);
    }

    return promise;
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate
WebIDL::ExceptionOr<JS::Value> instantiate(JS::VM& vm, JS::Handle<WebIDL::BufferSource>& bytes, Optional<JS::Handle<JS::Object>>& import_object)
{
    // FIXME: Implement the importObject parameter.
    (void)import_object;

    auto& realm = *vm.current_realm();

    // FIXME: This shouldn't block!
    auto compiled_module_or_error = Detail::parse_module(vm, bytes->raw_object());
    auto promise = JS::Promise::create(realm);

    if (compiled_module_or_error.is_error()) {
        promise->reject(*compiled_module_or_error.release_error().value());
        return promise;
    }

    auto compiled_module = compiled_module_or_error.release_value();
    auto result = Detail::instantiate_module(vm, compiled_module->module);

    if (result.is_error()) {
        promise->reject(*result.release_error().value());
    } else {
        auto module_object = vm.heap().allocate<Module>(realm, realm, move(compiled_module));
        auto instance_object = vm.heap().allocate<Instance>(realm, realm, result.release_value());

        auto object = JS::Object::create(realm, nullptr);
        object->define_direct_property("module", module_object, JS::default_attributes);
        object->define_direct_property("instance", instance_object, JS::default_attributes);
        promise->fulfill(object);
    }

    return promise;
}

// https://webassembly.github.io/spec/js-api/#dom-webassembly-instantiate-moduleobject-importobject
WebIDL::ExceptionOr<JS::Value> instantiate(JS::VM& vm, Module const& module_object, Optional<JS::Handle<JS::Object>>& import_object)
{
    // FIXME: Implement the importObject parameter.
    (void)import_object;

    auto& realm = *vm.current_realm();
    auto promise = JS::Promise::create(realm);

    auto const& compiled_module = module_object.compiled_module();
    auto result = Detail::instantiate_module(vm, compiled_module->module);

    if (result.is_error()) {
        promise->reject(*result.release_error().value());
    } else {
        auto instance_object = vm.heap().allocate<Instance>(realm, realm, result.release_value());
        promise->fulfill(instance_object);
    }

    return promise;
}

namespace Detail {

JS::ThrowCompletionOr<NonnullOwnPtr<Wasm::ModuleInstance>> instantiate_module(JS::VM& vm, Wasm::Module const& module)
{
    Wasm::Linker linker { module };
    HashMap<Wasm::Linker::Name, Wasm::ExternValue> resolved_imports;
    auto import_argument = vm.argument(1);
    auto& cache = get_cache(*vm.current_realm());
    if (!import_argument.is_undefined()) {
        auto import_object = TRY(import_argument.to_object(vm));
        dbgln("Trying to resolve stuff because import object was specified");
        for (Wasm::Linker::Name const& import_name : linker.unresolved_imports()) {
            dbgln("Trying to resolve {}::{}", import_name.module, import_name.name);
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
                    dbgln("Trying to resolve a function {}::{}, type index {}", import_name.module, import_name.name, index.value());
                    auto& type = module.type(index);
                    // FIXME: IsCallable()
                    if (!import_.is_function())
                        return {};
                    auto& function = import_.as_function();
                    // FIXME: If this is a function created by create_native_function(),
                    //        just extract its address and resolve to that.
                    Wasm::HostFunction host_function {
                        [&](auto&, auto& arguments) -> Wasm::Result {
                            JS::MarkedVector<JS::Value> argument_values { vm.heap() };
                            for (auto& entry : arguments)
                                argument_values.append(to_js_value(vm, entry));

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
                    dbgln("Resolved to {}", address->value());
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

JS::ThrowCompletionOr<NonnullRefPtr<CompiledWebAssemblyModule>> parse_module(JS::VM& vm, JS::Object* buffer_object)
{
    ReadonlyBytes data;
    if (is<JS::ArrayBuffer>(buffer_object)) {
        auto& buffer = static_cast<JS::ArrayBuffer&>(*buffer_object);
        data = buffer.buffer();
    } else if (is<JS::TypedArrayBase>(buffer_object)) {
        auto& buffer = static_cast<JS::TypedArrayBase&>(*buffer_object);

        auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(buffer, JS::ArrayBuffer::Order::SeqCst);
        if (JS::is_typed_array_out_of_bounds(typed_array_record))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::BufferOutOfBounds, "TypedArray"sv);

        data = buffer.viewed_array_buffer()->buffer().span().slice(buffer.byte_offset(), JS::typed_array_byte_length(typed_array_record));
    } else if (is<JS::DataView>(buffer_object)) {
        auto& buffer = static_cast<JS::DataView&>(*buffer_object);

        auto view_record = JS::make_data_view_with_buffer_witness_record(buffer, JS::ArrayBuffer::Order::SeqCst);
        if (JS::is_view_out_of_bounds(view_record))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::BufferOutOfBounds, "DataView"sv);

        data = buffer.viewed_array_buffer()->buffer().span().slice(buffer.byte_offset(), JS::get_view_byte_length(view_record));
    } else {
        return vm.throw_completion<JS::TypeError>("Not a BufferSource"sv);
    }
    FixedMemoryStream stream { data };
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
                return to_js_value(vm, result.values().first());

            return JS::Value(JS::Array::create_from<Wasm::Value>(realm, result.values(), [&](Wasm::Value value) {
                return to_js_value(vm, value);
            }));
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
            return Wasm::Value { Wasm::ValueType(Wasm::ValueType::FunctionReference), 0ull };

        if (value.is_function()) {
            auto& function = value.as_function();
            auto& cache = get_cache(*vm.current_realm());
            for (auto& entry : cache.function_instances()) {
                if (entry.value == &function)
                    return Wasm::Value { Wasm::Reference { Wasm::Reference::Func { entry.key } } };
            }
        }

        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Exported function");
    }
    case Wasm::ValueType::ExternReference:
        TODO();
    case Wasm::ValueType::V128:
        return vm.throw_completion<JS::TypeError>("Cannot convert a vector value to a javascript value"sv);
    }

    VERIFY_NOT_REACHED();
}

JS::Value to_js_value(JS::VM& vm, Wasm::Value& wasm_value)
{
    auto& realm = *vm.current_realm();
    switch (wasm_value.type().kind()) {
    case Wasm::ValueType::I64:
        return realm.heap().allocate<JS::BigInt>(realm, ::Crypto::SignedBigInteger { wasm_value.to<i64>().value() });
    case Wasm::ValueType::I32:
        return JS::Value(wasm_value.to<i32>().value());
    case Wasm::ValueType::F64:
        return JS::Value(wasm_value.to<double>().value());
    case Wasm::ValueType::F32:
        return JS::Value(static_cast<double>(wasm_value.to<float>().value()));
    case Wasm::ValueType::FunctionReference: {
        auto address = wasm_value.to<Wasm::Reference::Func>().value().address;
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
    case Wasm::ValueType::V128:
    case Wasm::ValueType::ExternReference:
        TODO();
    }
    VERIFY_NOT_REACHED();
}

}

}
