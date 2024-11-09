/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FinalizationRegistryPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(FinalizationRegistryPrototype);

FinalizationRegistryPrototype::FinalizationRegistryPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void FinalizationRegistryPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.cleanupSome, cleanup_some, 0, attr);
    define_native_function(realm, vm.names.register_, register_, 2, attr);
    define_native_function(realm, vm.names.unregister, unregister, 1, attr);

    // 26.2.3.4 FinalizationRegistry.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-finalization-registry.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.FinalizationRegistry.as_string()), Attribute::Configurable);
}

// @STAGE 2@ FinalizationRegistry.prototype.cleanupSome ( [ callback ] ), https://github.com/tc39/proposal-cleanup-some/blob/master/spec/finalization-registry.html
JS_DEFINE_NATIVE_FUNCTION(FinalizationRegistryPrototype::cleanup_some)
{
    auto callback = vm.argument(0);

    // 1. Let finalizationRegistry be the this value.
    // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
    auto finalization_registry = TRY(typed_this_object(vm));

    // 3. If callback is present and IsCallable(callback) is false, throw a TypeError exception.
    if (vm.argument_count() > 0 && !callback.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback.to_string_without_side_effects());

    // IMPLEMENTATION DEFINED: The specification for this function hasn't been updated to accommodate for JobCallback records.
    //                         This just follows how the constructor immediately converts the callback to a JobCallback using HostMakeJobCallback.
    // 4. Perform ? CleanupFinalizationRegistry(finalizationRegistry, callback).
    TRY(finalization_registry->cleanup(callback.is_undefined() ? GCPtr<JobCallback> {} : vm.host_make_job_callback(callback.as_function())));

    // 5. Return undefined.
    return js_undefined();
}

// 26.2.3.2 FinalizationRegistry.prototype.register ( target, heldValue [ , unregisterToken ] ), https://tc39.es/ecma262/#sec-finalization-registry.prototype.register
JS_DEFINE_NATIVE_FUNCTION(FinalizationRegistryPrototype::register_)
{
    auto target = vm.argument(0);
    auto held_value = vm.argument(1);
    auto unregister_token = vm.argument(2);

    // 1. Let finalizationRegistry be the this value.
    // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
    auto finalization_registry = TRY(typed_this_object(vm));

    // 3. If target is not an Object, throw a TypeError exception.
    if (!can_be_held_weakly(target))
        return vm.throw_completion<TypeError>(ErrorType::CannotBeHeldWeakly, target.to_string_without_side_effects());

    // 4. If SameValue(target, heldValue) is true, throw a TypeError exception.
    if (same_value(target, held_value))
        return vm.throw_completion<TypeError>(ErrorType::FinalizationRegistrySameTargetAndValue);

    // 5. If unregisterToken is not an Object, then
    //     a. If unregisterToken is not undefined, throw a TypeError exception.
    //     b. Set unregisterToken to empty.
    if (!can_be_held_weakly(unregister_token) && !unregister_token.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::CannotBeHeldWeakly, unregister_token.to_string_without_side_effects());

    // 6. Let cell be the Record { [[WeakRefTarget]]: target, [[HeldValue]]: heldValue, [[UnregisterToken]]: unregisterToken }.
    // 7. Append cell to finalizationRegistry.[[Cells]].
    finalization_registry->add_finalization_record(target.as_cell(), held_value, unregister_token.is_undefined() ? nullptr : &unregister_token.as_cell());

    // 8. Return undefined.
    return js_undefined();
}

// 26.2.3.3 FinalizationRegistry.prototype.unregister ( unregisterToken ), https://tc39.es/ecma262/#sec-finalization-registry.prototype.unregister
JS_DEFINE_NATIVE_FUNCTION(FinalizationRegistryPrototype::unregister)
{
    auto unregister_token = vm.argument(0);

    // 1. Let finalizationRegistry be the this value.
    // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
    auto finalization_registry = TRY(typed_this_object(vm));

    // 3. If unregisterToken is not an Object, throw a TypeError exception.
    if (!can_be_held_weakly(unregister_token))
        return vm.throw_completion<TypeError>(ErrorType::CannotBeHeldWeakly, unregister_token.to_string_without_side_effects());

    // 4-6.
    return Value(finalization_registry->remove_by_token(unregister_token.as_cell()));
}

}
