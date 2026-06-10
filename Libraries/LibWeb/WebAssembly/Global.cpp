/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWasm/Types.h>
#include <LibWeb/Bindings/Global.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Global);

// https://webassembly.github.io/spec/js-api/#dom-global-global
WebIDL::ExceptionOr<GC::Ref<Global>> Global::create(NonnullRefPtr<Detail::WebAssemblyCache> cache, GlobalDescriptor const& descriptor, Wasm::Value value)
{
    // 1. Let mutable be descriptor["mutable"].
    auto mutable_ = descriptor.mutable_;

    // 2. Let valuetype be ToValueType(descriptor["value"]).
    auto value_type = descriptor.value;

    // 3. If valuetype is v128,
    // 3.1 Throw a TypeError exception.
    if (value_type.kind() == Wasm::ValueType::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "V128 is not supported as a global value type"sv };

    // 6. If mutable is true, let globaltype be var valuetype; otherwise, let globaltype be const valuetype.
    auto global_type = Wasm::GlobalType { value_type, mutable_ };

    // 7. Let store be the current agent’s associated store.
    // 8. Let (store, globaladdr) be global_alloc(store, globaltype, value).
    // 9. Set the current agent’s associated store to store.
    // 10. Initialize this from globaladdr.

    auto address = cache->abstract_machine().store().allocate(global_type, value);
    if (!address.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Wasm Global allocation failed"sv };

    return Global::create(cache, *address);
}

GC::Ref<Global> Global::create(NonnullRefPtr<Detail::WebAssemblyCache> cache, Wasm::GlobalAddress address)
{
    auto global = GC::Heap::the().allocate<Global>(cache, address);

    // https://webassembly.github.io/spec/js-api/#initialize-a-global-object
    auto exists = cache->global_instances().contains(address);
    VERIFY(!exists);
    cache->add_global_instance(address, global);

    return global;
}

Global::Global(NonnullRefPtr<Detail::WebAssemblyCache> cache, Wasm::GlobalAddress address)
    : m_cache(move(cache))
    , m_address(address)
{
}

void Global::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_cache->visit_edges(visitor);
}

WebIDL::ExceptionOr<Wasm::GlobalType> Global::type() const
{
    // 1. Let store be the current agent’s associated store.
    // 2. Let globaladdr be global.[[Global]].
    // 3. Let globaltype be global_type(store, globaladdr).
    // 4. If globaltype is of the form mut v128, throw a TypeError.

    auto* global_instance = cache().abstract_machine().store().get(address());
    if (!global_instance)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the global instance"sv };

    return global_instance->type();
}

WebIDL::ExceptionOr<Wasm::ValueType> Global::value_type() const
{
    auto global_type = TRY(type());
    auto value_type = global_type.type();
    if (value_type.kind() == Wasm::ValueType::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "V128 is not supported as a global value type"sv };

    return value_type;
}

WebIDL::ExceptionOr<Wasm::Value> Global::value() const
{
    auto* global_instance = cache().abstract_machine().store().get(address());
    if (!global_instance)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the global instance"sv };

    (void)TRY(value_type());

    // 5. Let value be global_read(store, globaladdr).
    // 6. Return value.
    return global_instance->value();
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<void> Global::set_value(Wasm::Value value)
{
    // 1. Let store be the current agent’s associated store.
    // 2. Let globaladdr be this.[[Global]].
    // 3. Let mut valuetype be global_type(store, globaladdr).
    // 4. If valuetype is v128, throw a TypeError.
    // 5. If mut is const, throw a TypeError.

    auto* global_instance = cache().abstract_machine().store().get(address());
    if (!global_instance)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the global instance"sv };

    auto mut_value_type = TRY(type());
    if (mut_value_type.type().kind() == Wasm::ValueType::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a V128 global"sv };

    if (!mut_value_type.is_mutable())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a const global"sv };

    // 7. Let store be global_write(store, globaladdr, value).
    // 8. If store is error, throw a RangeError exception.
    // 9. Set the current agent’s associated store to store.
    // Note: The store cannot fail, because we checked for mut/val above.

    global_instance->set_value(value);

    return {};
}

}

namespace Web::Bindings {

// https://webassembly.github.io/spec/js-api/#tovaluetype
static Wasm::ValueType to_value_type(ValueType type)
{
    switch (type) {
    case ValueType::I32:
        return Wasm::ValueType { Wasm::ValueType::I32 };
    case ValueType::I64:
        return Wasm::ValueType { Wasm::ValueType::I64 };
    case ValueType::F32:
        return Wasm::ValueType { Wasm::ValueType::F32 };
    case ValueType::F64:
        return Wasm::ValueType { Wasm::ValueType::F64 };
    case ValueType::V128:
        return Wasm::ValueType { Wasm::ValueType::V128 };
    case ValueType::Anyfunc:
        return Wasm::ValueType { Wasm::ValueType::FunctionReference };
    case ValueType::Externref:
        return Wasm::ValueType { Wasm::ValueType::ExternReference };
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
    auto cache = WebAssembly::Detail::get_cache(realm);
    auto global_descriptor = global_descriptor_from_bindings(descriptor);

    // FIXME: https://github.com/WebAssembly/spec/issues/1861
    //        Is there a difference between *missing* and undefined for optional any values?
    auto wasm_value = !value.has_value()
        ? WebAssembly::Detail::default_webassembly_value(realm, global_descriptor.value)
        : TRY(WebAssembly::Detail::to_webassembly_value(realm, *value, global_descriptor.value));

    return WebAssembly::Global::create(move(cache), global_descriptor, wasm_value);
}

// https://webassembly.github.io/spec/js-api/#getglobalvalue
static WebIDL::ExceptionOr<JS::Value> get_global_value(JS::Realm& realm, WebAssembly::Global& global)
{
    auto value = TRY(global.value());
    auto value_type = TRY(global.value_type());
    return WebAssembly::Detail::to_js_value(realm, value, value_type);
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<JS::Value> value(JS::Realm& realm, WebAssembly::Global& global)
{
    return get_global_value(realm, global);
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<void> set_value(JS::Realm& realm, WebAssembly::Global& global, JS::Value the_given_value)
{
    auto global_type = TRY(global.type());
    auto value_type = global_type.type();
    if (value_type.kind() == Wasm::ValueType::V128)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a V128 global"sv };

    if (!global_type.is_mutable())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set the value of a const global"sv };

    auto value = TRY(WebAssembly::Detail::to_webassembly_value(realm, the_given_value, value_type));
    return global.set_value(value);
}

// https://webassembly.github.io/spec/js-api/#dom-global-valueof
WebIDL::ExceptionOr<JS::Value> value_of(JS::Realm& realm, WebAssembly::Global& global)
{
    return get_global_value(realm, global);
}

}
