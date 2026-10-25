/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebAssembly/BindingsGlue.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/Instance.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/Tag.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Instance);

// https://webassembly.github.io/spec/js-api/#dom-instance-instance
WebIDL::ExceptionOr<GC::Ref<Instance>> Instance::create(JS::Realm& realm, Module& module_object, GC::Ptr<JS::Object> import_object)
{
    // 1. Let builtinSetNames be moduleObject.[[BuiltinSets]].
    auto const& builtin_set_names = module_object.builtin_sets();

    // 2. Let importedStringModule be moduleObject.[[ImportedStringModule]].
    auto const& imported_string_module = module_object.imported_string_module();

    // 3. Let module be moduleObject.[[Module]].
    auto const& module = *module_object.compiled_module()->module;

    // 4. Read the imports of module with imports importObject, builtinSetNames, and importedStringModule, and let
    //    imports be the result.
    // 5. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
    auto instance = TRY(Detail::instantiate_module(realm.vm(), module, import_object, builtin_set_names, imported_string_module));

    // 6. Initialize this from module and instance.
    return Instance::create(realm, instance);
}

GC::Ref<Instance> Instance::create(JS::Realm& realm, Wasm::Runtime::Entity instance)
{
    return GC::Heap::the().allocate<Instance>(Bindings::host_defined_wrapper_world(realm), instance);
}

Instance::Instance(Bindings::WrapperWorld& wrapper_world, Wasm::Runtime::Entity instance)
    : m_instance(instance)
    , m_wrapper_world(wrapper_world)
{
}

GC::Ref<Global> Instance::global_instance(Wasm::Runtime::Entity global)
{
    if (auto object = Detail::WebAssemblyCache::the().global_object(global))
        return *object;
    return Global::create(global);
}

GC::Ref<Memory> Instance::memory_instance(Wasm::Runtime::Entity memory)
{
    auto& cache = Detail::WebAssemblyCache::the();
    if (auto object = cache.memory_object(memory))
        return *object;
    auto shared = cache.store().memory_limits(memory).shared ? Memory::Shared::Yes : Memory::Shared::No;
    return Memory::create(memory, shared);
}

GC::Ref<Table> Instance::table_instance(Wasm::Runtime::Entity table)
{
    if (auto object = Detail::WebAssemblyCache::the().table_object(table))
        return *object;
    return Table::create(table);
}

void Instance::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_instance);
    visitor.visit(m_wrapper_world);
}

}

namespace Web::Bindings {

WebIDL::ExceptionOr<GC::Ref<WebAssembly::Instance>> construct_instance(JS::Realm& realm, WebAssembly::Module& module, GC::Ptr<JS::Object> import_object)
{
    return WebAssembly::Instance::create(realm, module, import_object);
}

struct InstanceExportsCache {
    GC::Weak<WebAssembly::Instance> instance;
    GC::Weak<JS::Object> exports;
};

static Vector<InstanceExportsCache>& instance_exports_caches()
{
    static NeverDestroyed<Vector<InstanceExportsCache>> caches;
    return *caches;
}

static void prune_instance_exports_caches()
{
    instance_exports_caches().remove_all_matching([](auto const& cache) {
        return !cache.instance;
    });
}

static InstanceExportsCache& instance_exports_cache_for(WebAssembly::Instance& instance)
{
    auto& caches = instance_exports_caches();
    prune_instance_exports_caches();

    for (auto& cache : caches) {
        if (cache.instance.ptr() == GC::Ref { instance })
            return cache;
    }

    caches.append(InstanceExportsCache { instance, {} });
    return caches.last();
}

static GC::Ref<JS::Object> create_exports_object(JS::Realm& realm, WebAssembly::Instance& instance)
{
    auto exports = GC::make_root(JS::Object::create(realm, nullptr));

    // https://webassembly.github.io/spec/js-api/#create-an-exports-object
    for (auto& export_ : WebAssembly::Detail::WebAssemblyCache::the().store().instance_exports(instance.instance_address())) {
        auto name = Utf16FlyString::from_utf8(export_.name);
        auto address = export_.value.entity.ptr();

        switch (export_.value.kind) {
        case Wasm::Runtime::ExternKind::Function: {
            auto object = WebAssembly::Detail::create_native_function(realm, address);
            exports->define_direct_property(name, object, JS::default_attributes);
            break;
        }
        case Wasm::Runtime::ExternKind::Global:
            exports->define_direct_property(name, wrap(host_defined_wrapper_world(realm), realm, instance.global_instance(address)), JS::default_attributes);
            break;
        case Wasm::Runtime::ExternKind::Memory:
            exports->define_direct_property(name, wrap(host_defined_wrapper_world(realm), realm, instance.memory_instance(address)), JS::default_attributes);
            break;
        case Wasm::Runtime::ExternKind::Table:
            exports->define_direct_property(name, table(realm, instance.table_instance(address)), JS::default_attributes);
            break;
        case Wasm::Runtime::ExternKind::Tag:
            exports->define_direct_property(name, wrap(host_defined_wrapper_world(realm), realm, WebAssembly::Tag::from_address(realm, address)), JS::default_attributes);
            break;
        }
    }

    MUST(exports->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    return GC::Ref { *exports };
}

GC::Ref<JS::Object const> exports(JS::Realm& realm, GC::Ref<WebAssembly::Instance> instance)
{
    // NB: An instance's exports, like its memories and tables, belong to the world it was created in.
    VERIFY(&host_defined_wrapper_world(realm) == &instance->wrapper_world());

    auto& cache = instance_exports_cache_for(instance);
    if (cache.exports)
        return cache.exports.ptr().as_nonnull();

    auto exports = create_exports_object(realm, instance);
    cache.exports = exports;
    return exports;
}

JS::ThrowCompletionOr<void> initialize_webassembly_export_binding(JS::Realm& realm, JS::Environment& environment, Utf16FlyString const& name, GC::Ref<WebAssembly::Instance> instance)
{
    auto exports = Bindings::exports(realm, instance);
    return environment.initialize_binding(realm.vm(), name, TRY(exports->get(JS::PropertyKey { name })), JS::Environment::InitializeBindingHint::Normal);
}

}
