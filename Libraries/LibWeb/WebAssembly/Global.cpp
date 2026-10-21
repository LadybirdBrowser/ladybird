/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Global.h>
#include <LibWeb/WebAssembly/BindingsGlue.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Global);

// https://webassembly.github.io/spec/js-api/#dom-global-global
GC::Ref<Global> Global::create(GlobalDescriptor const& descriptor, Wasm::Runtime::Value value)
{
    // 1. Let mutable be descriptor["mutable"].
    // 2. Let valuetype be ToValueType(descriptor["value"]).
    // NB: The bindings reject v128 before getting here.
    VERIFY(descriptor.value.kind != Wasm::Runtime::ValueKind::V128);

    // 6. If mutable is true, let globaltype be var valuetype; otherwise, let globaltype be const valuetype.
    // 7. Let store be the current agent’s associated store.
    // 8. Let (store, globaladdr) be global_alloc(store, globaltype, value).
    // 9. Set the current agent’s associated store to store.
    auto global = Detail::WebAssemblyCache::the().store().allocate_global(descriptor.value, descriptor.mutable_, value);

    // 10. Initialize this from globaladdr.
    return Global::create(global);
}

GC::Ref<Global> Global::create(Wasm::Runtime::Entity global)
{
    // https://webassembly.github.io/spec/js-api/#initialize-a-global-object
    auto& cache = Detail::WebAssemblyCache::the();
    VERIFY(!cache.global_object(global));
    auto object = GC::Heap::the().allocate<Global>(global);
    cache.set_global_object(global, object);
    return object;
}

Global::Global(Wasm::Runtime::Entity global)
    : m_global(global)
{
}

void Global::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global);
}

GlobalType Global::type() const
{
    // 1. Let store be the current agent’s associated store.
    // 2. Let globaladdr be global.[[Global]].
    // 3. Let globaltype be global_type(store, globaladdr).
    bool is_mutable = false;
    auto value_type = Detail::WebAssemblyCache::the().store().global_type(m_global, is_mutable);
    return GlobalType { value_type, is_mutable };
}

WebIDL::ExceptionOr<Wasm::Runtime::ValueType> Global::value_type() const
{
    // 4. If globaltype is of the form mut v128, throw a TypeError.
    auto global_type = type();
    if (global_type.value.kind == Wasm::Runtime::ValueKind::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "V128 is not supported as a global value type"_utf16 };
    return global_type.value;
}

WebIDL::ExceptionOr<Wasm::Runtime::Value> Global::value() const
{
    (void)TRY(value_type());

    // 5. Let value be global_read(store, globaladdr).
    // 6. Return value.
    return Detail::WebAssemblyCache::the().store().global_get(m_global);
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<void> Global::set_value(Wasm::Runtime::Value value)
{
    // 1. Let store be the current agent’s associated store.
    // 2. Let globaladdr be this.[[Global]].
    // 3. Let mut valuetype be global_type(store, globaladdr).
    // 4. If valuetype is v128, throw a TypeError.
    // 5. If mut is const, throw a TypeError.
    auto global_type = type();
    if (global_type.value.kind == Wasm::Runtime::ValueKind::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a V128 global"_utf16 };
    if (!global_type.mutable_)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a const global"_utf16 };

    // 7. Let store be global_write(store, globaladdr, value).
    // 8. If store is error, throw a RangeError exception.
    // 9. Set the current agent’s associated store to store.
    // Note: The store cannot fail, because we checked for mut/val above.
    Detail::WebAssemblyCache::the().store().global_set(m_global, value);
    return {};
}

}

namespace Web::Bindings {

// https://webassembly.github.io/spec/js-api/#tovaluetype
Wasm::Runtime::ValueType to_value_type(ValueType type)
{
    using Wasm::Runtime::HeapKind;
    using Wasm::Runtime::ValueKind;
    switch (type) {
    case ValueType::I32:
        return Wasm::Runtime::ValueType::scalar(ValueKind::I32);
    case ValueType::I64:
        return Wasm::Runtime::ValueType::scalar(ValueKind::I64);
    case ValueType::F32:
        return Wasm::Runtime::ValueType::scalar(ValueKind::F32);
    case ValueType::F64:
        return Wasm::Runtime::ValueType::scalar(ValueKind::F64);
    case ValueType::V128:
        return Wasm::Runtime::ValueType::scalar(ValueKind::V128);
    case ValueType::Anyfunc:
        return Wasm::Runtime::ValueType::reference(HeapKind::Function);
    case ValueType::Externref:
        return Wasm::Runtime::ValueType::reference(HeapKind::Extern);
    }

    VERIFY_NOT_REACHED();
}

static WebAssembly::GlobalDescriptor global_descriptor_from_bindings(GlobalDescriptor const& descriptor)
{
    return {
        .value = to_value_type(descriptor.value),
        .mutable_ = descriptor.mutable_,
    };
}

WebIDL::ExceptionOr<GC::Ref<WebAssembly::Global>> construct_global(JS::Realm& realm, GlobalDescriptor const& descriptor, Optional<JS::Value> value)
{
    auto global_descriptor = global_descriptor_from_bindings(descriptor);
    if (global_descriptor.value.kind == Wasm::Runtime::ValueKind::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "V128 is not supported as a global value type"_utf16 };

    // FIXME: https://github.com/WebAssembly/spec/issues/1861
    //        Is there a difference between *missing* and undefined for optional any values?
    auto wasm_value = !value.has_value()
        ? WebAssembly::Detail::default_webassembly_value(realm.vm(), global_descriptor.value)
        : TRY(WebAssembly::Detail::to_webassembly_value(realm.vm(), *value, global_descriptor.value));

    return WebAssembly::Global::create(global_descriptor, wasm_value);
}

// https://webassembly.github.io/spec/js-api/#getglobalvalue
static WebIDL::ExceptionOr<JS::Value> get_global_value(JS::Realm& realm, WebAssembly::Global& global)
{
    auto value = TRY(global.value());
    auto value_type = TRY(global.value_type());
    return TRY(WebAssembly::Detail::to_js_value(realm.vm(), value, value_type));
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<JS::Value> value(JS::Realm& realm, WebAssembly::Global& global)
{
    return get_global_value(realm, global);
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<void> set_value(JS::Realm& realm, WebAssembly::Global& global, JS::Value the_given_value)
{
    auto global_type = global.type();
    auto value_type = global_type.value;
    if (value_type.kind == Wasm::Runtime::ValueKind::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a V128 global"_utf16 };

    if (!global_type.mutable_)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a const global"_utf16 };

    auto value = TRY(WebAssembly::Detail::to_webassembly_value(realm.vm(), the_given_value, value_type));
    return global.set_value(value);
}

// https://webassembly.github.io/spec/js-api/#dom-global-valueof
WebIDL::ExceptionOr<JS::Value> value_of(JS::Realm& realm, WebAssembly::Global& global)
{
    return get_global_value(realm, global);
}

}
