/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Root.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Instance.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Instance);

static void prune_live_export_objects(Vector<GC::Weak<JS::Object>>& live_export_objects)
{
    live_export_objects.remove_all_matching([](auto const& exports) { return !exports; });
}

static GC::Ref<JS::FunctionObject> create_native_function_for_instance_realm(JS::Realm& realm, Wasm::FunctionAddress address, Utf16FlyString name, GC::Ref<Instance> instance)
{
    Optional<Wasm::FunctionType> type;
    auto& instance_cache = Detail::get_cache(instance->realm());
    auto* function = instance_cache.abstract_machine().store().get(address);
    VERIFY(function);
    function->visit([&](auto const& value) { type = value.type(); });

    return Detail::ExportedWasmFunction::create(
        realm,
        move(name),
        [address, type = type.release_value(), instance](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto& realm = *vm.current_realm();
            Vector<Wasm::Value> values;
            values.ensure_capacity(type.parameters().size());

            size_t index = 0;
            for (auto& type : type.parameters())
                values.append(TRY(Detail::to_webassembly_value(vm, vm.argument(index++), type)));

            auto& instance_cache = Detail::get_cache(instance->realm());
            auto result = instance_cache.abstract_machine().invoke(address, move(values));
            if (result.is_trap()) {
                if (auto ptr = result.trap().data.get_pointer<Wasm::ExternallyManagedTrap>())
                    return ptr->unsafe_external_object_as<JS::Completion>();
                auto& trap = result.trap().data.get<ByteString>();
                if (trap.ends_with(Wasm::Constants::stack_exhaustion_message))
                    return vm.throw_completion<JS::InternalError>(JS::ErrorType::CallStackSizeExceeded);
                return vm.throw_completion<RuntimeError>(TRY_OR_THROW_OOM(vm, String::formatted("Wasm execution trapped (WIP): {}", trap)));
            }

            if (result.values().is_empty())
                return JS::js_undefined();

            if (result.values().size() == 1)
                return Detail::to_js_value(vm, result.values().first(), type.results().first());

            GC::RootVector<JS::Value> js_result_values;
            js_result_values.ensure_capacity(result.values().size());

            for (size_t i = result.values().size(); i > 0; i--)
                js_result_values.unchecked_append(Detail::to_js_value(vm, result.values().at(i - 1), type.results().at(i - 1)));

            return JS::Value(JS::Array::create_from(realm, js_result_values));
        },
        address);
}

WebIDL::ExceptionOr<GC::Ref<Instance>> Instance::construct_impl(JS::Realm& realm, Module& module, GC::Ptr<JS::Object> import_object)
{
    auto& vm = realm.vm();

    auto module_instance = TRY(Detail::instantiate_module(vm, module.compiled_module()->module, import_object));
    return Instance::create(realm, move(module_instance));
}

GC::Ref<Instance> Instance::create(JS::Realm& realm, NonnullRefPtr<Wasm::ModuleInstance> module_instance)
{
    auto instance = GC::make_root(realm.create<Instance>(realm, move(module_instance)));
    instance->m_exports = instance->create_exports_object(realm);
    return GC::Ref { *instance };
}

Instance::Instance(JS::Realm& realm, NonnullRefPtr<Wasm::ModuleInstance> module_instance)
    : Bindings::Wrappable(realm)
    , m_exports(JS::Object::create(realm, nullptr))
    , m_module_instance(move(module_instance))
{
}

GC::Ref<JS::Object> Instance::create_exports_object(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto exports = GC::make_root(JS::Object::create(realm, nullptr));

    auto& object_cache = Detail::get_cache(this->realm());
    auto const should_cache_function_exports = &realm == &this->realm();

    // https://webassembly.github.io/spec/js-api/#create-an-exports-object
    for (auto& export_ : m_module_instance->exports()) {
        auto name = Utf16FlyString::from_utf8(export_.name());

        export_.value().visit(
            [&](Wasm::FunctionAddress const& address) {
                GC::Ptr<JS::FunctionObject> object;
                if (should_cache_function_exports) {
                    object = m_function_instances.get(address).value_or(nullptr);
                    if (!object) {
                        object = Detail::create_native_function(vm, address, name, this);
                        m_function_instances.set(address, object);
                    }
                } else {
                    object = create_native_function_for_instance_realm(realm, address, name, GC::Ref { *this });
                }

                auto object_root = GC::make_root(*object);
                exports->define_direct_property(name, object_root.ptr(), JS::default_attributes);
            },
            [&](Wasm::GlobalAddress const& address) {
                Optional<GC::Ptr<Global>> object = object_cache.get_global_instance(address);
                if (!object.has_value()) {
                    object = Global::create(this->realm(), address);
                }

                auto wrapper = GC::make_root(Bindings::wrap(realm, GC::Ref { **object }));
                exports->define_direct_property(name, wrapper.ptr(), JS::default_attributes);
            },
            [&](Wasm::MemoryAddress const& address) {
                Optional<GC::Ptr<Memory>> object = object_cache.get_memory_instance(address);
                if (!object.has_value()) {
                    // FIXME: Once LibWasm implements the threads/atomics proposal, the shared-ness should be
                    //        obtained from the Wasm::MemoryInstance's type.
                    object = Memory::create(this->realm(), address, Memory::Shared::No);
                }

                auto wrapper = GC::make_root(Bindings::wrap(realm, GC::Ref { **object }));
                exports->define_direct_property(name, wrapper.ptr(), JS::default_attributes);
            },
            [&](Wasm::TableAddress const& address) {
                Optional<GC::Ptr<Table>> object = object_cache.get_table_instance(address);
                if (!object.has_value())
                    object = this->realm().create<Table>(this->realm(), address);

                auto wrapper = GC::make_root(Bindings::wrap(realm, GC::Ref { **object }));
                exports->define_direct_property(name, wrapper.ptr(), JS::default_attributes);
            },
            [&](Wasm::TagAddress const&) { dbgln("Not yet implemented: tags"); });
    }

    MUST(exports->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    return GC::Ref { *exports };
}

JS::Object const* Instance::exports()
{
    return exports_for_realm(*this->realm().vm().current_realm());
}

JS::Object const* Instance::exports_for_realm(JS::Realm& realm)
{
    if (&realm == &this->realm())
        return m_exports.ptr();

    prune_live_export_objects(m_live_export_objects);
    for (auto const& exports : m_live_export_objects) {
        if (&exports->shape().realm() == &realm)
            return exports.ptr();
    }

    auto exports = create_exports_object(realm);
    m_live_export_objects.append(exports);
    return exports.ptr();
}

void Instance::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_exports);
    visitor.visit(m_function_instances);
}

}
