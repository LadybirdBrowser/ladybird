/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/InstancePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Instance);

WebIDL::ExceptionOr<GC::Ref<Instance>> Instance::construct_impl(JS::Realm& realm, Module& module, Optional<GC::Root<JS::Object>>& import_object_handle)
{
    GC::Ptr<JS::Object> import_object = import_object_handle.has_value() ? import_object_handle.value().ptr() : nullptr;

    auto& vm = realm.vm();

    auto module_instance = TRY(Detail::instantiate_module(vm, module.compiled_module()->module, import_object));
    return realm.create<Instance>(realm, move(module_instance));
}

Instance::Instance(JS::Realm& realm, NonnullOwnPtr<Wasm::ModuleInstance> module_instance)
    : Bindings::PlatformObject(realm)
    , m_exports(Object::create(realm, nullptr))
    , m_module_instance(move(module_instance))
{
}

void Instance::initialize(JS::Realm& realm)
{
    auto& vm = this->vm();

    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Instance, WebAssembly.Instance);
    Base::initialize(realm);

    auto& cache = Detail::get_cache(realm);

    // https://webassembly.github.io/spec/js-api/#create-an-exports-object
    for (auto& export_ : m_module_instance->exports()) {
        auto name = Utf16FlyString::from_utf8(export_.name());

        export_.value().visit(
            [&](Wasm::FunctionAddress const& address) {
                Optional<GC::Ref<JS::FunctionObject>> object = m_function_instances.get(address);
                if (!object.has_value()) {
                    object = Detail::create_native_function(vm, address, name, this);
                    m_function_instances.set(address, *object);
                }

                m_exports->define_direct_property(name, *object, JS::default_attributes);
            },
            [&](Wasm::GlobalAddress const& address) {
                Optional<GC::Ref<Global>> object = cache.get_global_instance(address);
                if (!object.has_value()) {
                    object = realm.create<Global>(realm, address);
                }

                m_exports->define_direct_property(name, *object, JS::default_attributes);
            },
            [&](Wasm::MemoryAddress const& address) {
                Optional<GC::Ref<Memory>> object = cache.get_memory_instance(address);
                if (!object.has_value()) {
                    // FIXME: Once LibWasm implements the threads/atomics proposal, the shared-ness should be
                    //        obtained from the Wasm::MemoryInstance's type.
                    object = realm.create<Memory>(realm, address, Memory::Shared::No);
                }

                m_exports->define_direct_property(name, *object, JS::default_attributes);
            },
            [&](Wasm::TableAddress const& address) {
                Optional<GC::Ref<Table>> object = m_table_instances.get(address);
                if (!object.has_value()) {
                    object = realm.create<Table>(realm, address);
                    m_table_instances.set(address, *object);
                }

                m_exports->define_direct_property(name, *object, JS::default_attributes);
            });
    }

    MUST(m_exports->set_integrity_level(IntegrityLevel::Frozen));
}

void Instance::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_exports);
    visitor.visit(m_function_instances);
    visitor.visit(m_table_instances);
}

}
