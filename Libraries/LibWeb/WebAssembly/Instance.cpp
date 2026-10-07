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
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Instance);

WebIDL::ExceptionOr<GC::Ref<Instance>> Instance::create(JS::Realm& realm, Module& module, GC::Ptr<JS::Object> import_object)
{
    auto module_instance = TRY(Detail::instantiate_module(realm, module.compiled_module()->module, import_object));
    return Instance::create(Detail::get_cache(realm), move(module_instance));
}

GC::Ref<Instance> Instance::create(NonnullRefPtr<Detail::WebAssemblyCache> cache, NonnullRefPtr<Wasm::ModuleInstance> module_instance)
{
    return GC::Heap::the().allocate<Instance>(move(cache), move(module_instance));
}

Instance::Instance(NonnullRefPtr<Detail::WebAssemblyCache> cache, NonnullRefPtr<Wasm::ModuleInstance> module_instance)
    : m_cache(move(cache))
    , m_module_instance(move(module_instance))
{
}

GC::Ref<Global> Instance::global_instance(Wasm::GlobalAddress address)
{
    Optional<GC::Ptr<Global>> object = cache().get_global_instance(address);
    if (!object.has_value())
        object = Global::create(m_cache, address);
    return GC::Ref { **object };
}

GC::Ref<Memory> Instance::memory_instance(Wasm::MemoryAddress address)
{
    Optional<GC::Ptr<Memory>> object = cache().get_memory_instance(address);
    if (!object.has_value()) {
        // FIXME: Once LibWasm implements the threads/atomics proposal, the shared-ness should be
        //        obtained from the Wasm::MemoryInstance's type.
        object = Memory::create(m_cache, address, Memory::Shared::No);
    }
    return GC::Ref { **object };
}

GC::Ref<Table> Instance::table_instance(Wasm::TableAddress address)
{
    Optional<GC::Ptr<Table>> object = cache().get_table_instance(address);
    if (!object.has_value())
        object = GC::Heap::the().allocate<Table>(m_cache, address);
    return GC::Ref { **object };
}

void Instance::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    // NOTE: m_cache is already visited by the realm's HostDefined, which owns it.
    visitor.ignore(m_cache);
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
    for (auto& export_ : instance.module_instance()->exports()) {
        auto name = Utf16FlyString::from_utf8(export_.name());

        export_.value().visit(
            [&](Wasm::FunctionAddress const& address) {
                auto object = WebAssembly::Detail::create_native_function(realm, address, name, &instance);
                auto object_root = GC::make_root(*object);
                exports->define_direct_property(name, object_root.ptr(), JS::default_attributes);
            },
            [&](Wasm::GlobalAddress const& address) {
                exports->define_direct_property(name, wrap(host_defined_wrapper_world(realm), realm, instance.global_instance(address)), JS::default_attributes);
            },
            [&](Wasm::MemoryAddress const& address) {
                exports->define_direct_property(name, wrap(host_defined_wrapper_world(realm), realm, instance.memory_instance(address)), JS::default_attributes);
            },
            [&](Wasm::TableAddress const& address) {
                exports->define_direct_property(name, table(realm, instance.table_instance(address)), JS::default_attributes);
            },
            [&](Wasm::TagAddress const&) { dbgln("Not yet implemented: tags"); });
    }

    MUST(exports->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    return GC::Ref { *exports };
}

GC::Ref<JS::Object const> exports(JS::Realm& realm, GC::Ref<WebAssembly::Instance> instance)
{
    VERIFY(host_defined_of(realm).wasm_cache.ptr() == &instance->cache());

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
