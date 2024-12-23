/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWasm/Types.h>
#include <LibWeb/Bindings/GlobalPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Global);

// https://webassembly.github.io/spec/js-api/#tovaluetype
static Wasm::ValueType to_value_type(Bindings::ValueType type)
{
    switch (type) {
    case Bindings::ValueType::I32:
        return Wasm::ValueType { Wasm::ValueType::I32 };
    case Bindings::ValueType::I64:
        return Wasm::ValueType { Wasm::ValueType::I64 };
    case Bindings::ValueType::F32:
        return Wasm::ValueType { Wasm::ValueType::F32 };
    case Bindings::ValueType::F64:
        return Wasm::ValueType { Wasm::ValueType::F64 };
    case Bindings::ValueType::V128:
        return Wasm::ValueType { Wasm::ValueType::V128 };
    case Bindings::ValueType::Anyfunc:
        return Wasm::ValueType { Wasm::ValueType::FunctionReference };
    case Bindings::ValueType::Externref:
        return Wasm::ValueType { Wasm::ValueType::ExternReference };
    }

    VERIFY_NOT_REACHED();
}

// https://webassembly.github.io/spec/js-api/#dom-global-global
WebIDL::ExceptionOr<GC::Ref<Global>> Global::construct_impl(JS::Realm& realm, GlobalDescriptor& descriptor, JS::Value v)
{
    auto& vm = realm.vm();

    // 1. Let mutable be descriptor["mutable"].
    auto mutable_ = descriptor.mutable_;

    // 2. Let valuetype be ToValueType(descriptor["value"]).
    auto value_type = to_value_type(descriptor.value);

    // 3. If valuetype is v128,
    // 3.1 Throw a TypeError exception.
    if (value_type.kind() == Wasm::ValueType::V128)
        return vm.throw_completion<JS::TypeError>("V128 is not supported as a global value type"sv);

    // 4. If v is missing,
    // 4.1 Let value be DefaultValue(valuetype).
    // 5. Otherwise,
    // 5.1 Let value be ToWebAssemblyValue(v, valuetype).
    // FIXME: https://github.com/WebAssembly/spec/issues/1861
    //        Is there a difference between *missing* and undefined for optional any values?
    auto value = v.is_undefined()
        ? Detail::default_webassembly_value(vm, value_type)
        : TRY(Detail::to_webassembly_value(vm, v, value_type));

    // 6. If mutable is true, let globaltype be var valuetype; otherwise, let globaltype be const valuetype.
    auto global_type = Wasm::GlobalType { value_type, mutable_ };

    // 7. Let store be the current agent’s associated store.
    // 8. Let (store, globaladdr) be global_alloc(store, globaltype, value).
    // 9. Set the current agent’s associated store to store.
    // 10. Initialize this from globaladdr.

    auto& cache = Detail::get_cache(realm);
    auto address = cache.abstract_machine().store().allocate(global_type, value);
    if (!address.has_value())
        return vm.throw_completion<JS::TypeError>("Wasm Global allocation failed"sv);

    return realm.create<Global>(realm, *address);
}

Global::Global(JS::Realm& realm, Wasm::GlobalAddress address)
    : Bindings::PlatformObject(realm)
    , m_address(address)
{
}

// https://webassembly.github.io/spec/js-api/#initialize-a-global-object
void Global::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Global, WebAssembly.Global);

    // 1. Let map be the surrounding agent's associated Global object cache.
    // 2. Assert: map[globaladdr] doesn’t exist.
    auto& cache = Detail::get_cache(realm);
    auto exists = cache.global_instances().contains(m_address);
    VERIFY(!exists);

    // 3. Set global.[[Global]] to globaladdr.
    // 4. Set map[globaladdr] to global.
    cache.add_global_instance(m_address, *this);
}

// https://webassembly.github.io/spec/js-api/#getglobalvalue
static WebIDL::ExceptionOr<JS::Value> get_global_value(Global const& global)
{
    // 1. Let store be the current agent’s associated store.
    // 2. Let globaladdr be global.[[Global]].
    // 3. Let globaltype be global_type(store, globaladdr).
    // 4. If globaltype is of the form mut v128, throw a TypeError.

    auto& cache = Detail::get_cache(global.realm());
    auto* global_instance = cache.abstract_machine().store().get(global.address());
    if (!global_instance)
        return global.vm().throw_completion<JS::RangeError>("Could not find the global instance"sv);

    auto value_type = global_instance->type().type();
    if (value_type.kind() == Wasm::ValueType::V128)
        return global.vm().throw_completion<JS::TypeError>("V128 is not supported as a global value type"sv);

    // 5. Let value be global_read(store, globaladdr).
    auto value = global_instance->value();

    // 6. Return ToJSValue(value).
    return Detail::to_js_value(global.vm(), value, value_type);
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<JS::Value> Global::value() const
{
    return get_global_value(*this);
}

// https://webassembly.github.io/spec/js-api/#dom-global-valueof
WebIDL::ExceptionOr<JS::Value> Global::value_of() const
{
    return get_global_value(*this);
}

// https://webassembly.github.io/spec/js-api/#dom-global-value
WebIDL::ExceptionOr<void> Global::set_value(JS::Value the_given_value)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // 1. Let store be the current agent’s associated store.
    // 2. Let globaladdr be this.[[Global]].
    // 3. Let mut valuetype be global_type(store, globaladdr).
    // 4. If valuetype is v128, throw a TypeError.
    // 5. If mut is const, throw a TypeError.

    auto& cache = Detail::get_cache(realm);
    auto* global_instance = cache.abstract_machine().store().get(address());
    if (!global_instance)
        return vm.throw_completion<JS::RangeError>("Could not find the global instance"sv);

    auto mut_value_type = global_instance->type();
    if (mut_value_type.type().kind() == Wasm::ValueType::V128)
        return vm.throw_completion<JS::TypeError>("Cannot set the value of a V128 global"sv);

    if (!mut_value_type.is_mutable())
        return vm.throw_completion<JS::TypeError>("Cannot set the value of a const global"sv);

    // 6. Let value be ToWebAssemblyValue(the given value, valuetype).
    auto value = TRY(Detail::to_webassembly_value(vm, the_given_value, mut_value_type.type()));

    // 7. Let store be global_write(store, globaladdr, value).
    // 8. If store is error, throw a RangeError exception.
    // 9. Set the current agent’s associated store to store.
    // Note: The store cannot fail, because we checked for mut/val above.

    global_instance->set_value(value);

    return {};
}

}
