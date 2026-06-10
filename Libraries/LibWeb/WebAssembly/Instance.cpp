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
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
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
    m_cache->visit_edges(visitor);
}

}

namespace Web::Bindings {

WebIDL::ExceptionOr<GC::Ref<WebAssembly::Instance>> construct_instance(JS::Realm& realm, WebAssembly::Module& module, GC::Ptr<JS::Object> import_object)
{
    return WebAssembly::Instance::create(realm, module, import_object);
}

struct InstanceExportsCache {
    GC::Weak<WebAssembly::Instance> instance;
    GC::Weak<JS::Object> primary_exports;
    Vector<GC::Weak<JS::Object>> live_export_objects;
    HashMap<Wasm::FunctionAddress, GC::Weak<JS::FunctionObject>> function_instances;
};

static Vector<InstanceExportsCache>& instance_exports_caches()
{
    static NeverDestroyed<Vector<InstanceExportsCache>> caches;
    return *caches;
}

static void prune_instance_exports_cache(InstanceExportsCache& cache)
{
    cache.live_export_objects.remove_all_matching([](auto const& exports) { return !exports; });
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
        if (cache.instance.ptr() == &instance)
            return cache;
    }

    caches.append(InstanceExportsCache { instance, {}, {}, {} });
    return caches.last();
}

static GC::Ref<JS::FunctionObject> create_native_function_for_instance_realm(JS::Realm& realm, Wasm::FunctionAddress address, Utf16FlyString name, GC::Ref<WebAssembly::Instance> instance)
{
    Optional<Wasm::FunctionType> type;
    auto* function = instance->cache().abstract_machine().store().get(address);
    VERIFY(function);
    function->visit([&](auto const& value) { type = value.type(); });

    return WebAssembly::Detail::ExportedWasmFunction::create(
        realm,
        move(name),
        [address, type = type.release_value(), instance, realm = GC::Ref(realm)](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            Vector<Wasm::Value> values;
            values.ensure_capacity(type.parameters().size());

            size_t index = 0;
            for (auto& type : type.parameters())
                values.append(TRY(WebAssembly::Detail::to_webassembly_value(realm, vm.argument(index++), type)));

            auto result = instance->cache().abstract_machine().invoke(address, move(values));
            if (result.is_trap()) {
                if (auto ptr = result.trap().data.get_pointer<Wasm::ExternallyManagedTrap>())
                    return ptr->unsafe_external_object_as<JS::Completion>();
                auto& trap = result.trap().data.get<ByteString>();
                if (trap.ends_with(Wasm::Constants::stack_exhaustion_message))
                    return vm.throw_completion<JS::InternalError>(JS::ErrorType::CallStackSizeExceeded);
                return vm.throw_completion<WebAssembly::RuntimeError>(TRY_OR_THROW_OOM(vm, String::formatted("Wasm execution trapped (WIP): {}", trap)));
            }

            if (result.values().is_empty())
                return JS::js_undefined();

            if (result.values().size() == 1)
                return WebAssembly::Detail::to_js_value(realm, result.values().first(), type.results().first());

            GC::RootVector<JS::Value> js_result_values;
            js_result_values.ensure_capacity(result.values().size());

            for (size_t i = result.values().size(); i > 0; i--)
                js_result_values.unchecked_append(WebAssembly::Detail::to_js_value(realm, result.values().at(i - 1), type.results().at(i - 1)));

            return JS::Value(JS::Array::create_from(realm, js_result_values));
        },
        address);
}

static GC::Ref<JS::Object> create_exports_object(JS::Realm& realm, WebAssembly::Instance& instance, bool cache_function_exports)
{
    auto exports = GC::make_root(JS::Object::create(realm, nullptr));

    // https://webassembly.github.io/spec/js-api/#create-an-exports-object
    for (auto& export_ : instance.module_instance()->exports()) {
        auto name = Utf16FlyString::from_utf8(export_.name());

        export_.value().visit(
            [&](Wasm::FunctionAddress const& address) {
                GC::Ptr<JS::FunctionObject> object;
                if (cache_function_exports) {
                    auto& cache = instance_exports_cache_for(instance);
                    if (auto cached_object = cache.function_instances.get(address); cached_object.has_value())
                        object = cached_object->ptr();
                    if (!object) {
                        object = WebAssembly::Detail::create_native_function(realm, address, name, &instance);
                        cache.function_instances.set(address, *object);
                    }
                } else {
                    object = create_native_function_for_instance_realm(realm, address, name, GC::Ref { instance });
                }

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

JS::Object const* exports(JS::Realm& realm, GC::Ref<WebAssembly::Instance> instance)
{
    auto& cache = instance_exports_cache_for(instance);

    if (!cache.primary_exports) {
        auto exports = create_exports_object(realm, instance, true);
        cache.primary_exports = exports;
        return exports.ptr();
    }

    if (&realm == &cache.primary_exports->shape().realm())
        return cache.primary_exports.ptr();

    prune_instance_exports_cache(cache);
    for (auto const& exports : cache.live_export_objects) {
        if (&exports->shape().realm() == &realm)
            return exports.ptr();
    }

    auto exports = create_exports_object(realm, instance, false);
    cache.live_export_objects.append(exports);
    return exports.ptr();
}

JS::ThrowCompletionOr<void> initialize_webassembly_export_binding(JS::Realm& realm, JS::Environment& environment, Utf16FlyString const& name, GC::Ref<WebAssembly::Instance> instance)
{
    auto* exports = Bindings::exports(realm, instance);
    VERIFY(exports);
    return environment.initialize_binding(realm.vm(), name, TRY(exports->get(JS::PropertyKey { name })), JS::Environment::InitializeBindingHint::Normal);
}

}
