/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ShadowRealm.h>
#include <LibJS/Runtime/ShadowRealmPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ShadowRealmPrototype);

// 3.4 Properties of the ShadowRealm Prototype Object, https://tc39.es/proposal-shadowrealm/#sec-properties-of-the-shadowrealm-prototype-object
ShadowRealmPrototype::ShadowRealmPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void ShadowRealmPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.evaluate, evaluate, 1, attr);
    define_native_function(realm, vm.names.importValue, import_value, 2, attr);

    // 3.4.3 ShadowRealm.prototype [ @@toStringTag ], https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.ShadowRealm.as_string()), Attribute::Configurable);
}

// 3.4.1 ShadowRealm.prototype.evaluate ( sourceText ), https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype.evaluate
JS_DEFINE_NATIVE_FUNCTION(ShadowRealmPrototype::evaluate)
{
    auto source_text = vm.argument(0);

    // 1. Let O be this value.
    // 2. Perform ? ValidateShadowRealmObject(O).
    auto object = TRY(typed_this_object(vm));

    // 3. If Type(sourceText) is not String, throw a TypeError exception.
    if (!source_text.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, source_text);

    // 4. Let callerRealm be the current Realm Record.
    auto* caller_realm = vm.current_realm();

    // 5. Let evalRealm be O.[[ShadowRealm]].
    auto& eval_realm = object->shadow_realm();

    // 6. Return ? PerformShadowRealmEval(sourceText, callerRealm, evalRealm).
    return perform_shadow_realm_eval(vm, source_text.as_string().byte_string(), *caller_realm, eval_realm);
}

// 3.4.2 ShadowRealm.prototype.importValue ( specifier, exportName ), https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype.importvalue
JS_DEFINE_NATIVE_FUNCTION(ShadowRealmPrototype::import_value)
{
    auto specifier = vm.argument(0);
    auto export_name = vm.argument(1);

    // 1. Let O be this value.
    // 2. Perform ? ValidateShadowRealmObject(O).
    auto object = TRY(typed_this_object(vm));

    // 3. Let specifierString be ? ToString(specifier).
    auto specifier_string = TRY(specifier.to_byte_string(vm));

    // 4. If Type(exportName) is not String, throw a TypeError exception.
    if (!export_name.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, export_name.to_string_without_side_effects());

    // 5. Let callerRealm be the current Realm Record.
    auto* caller_realm = vm.current_realm();

    // 6. Let evalRealm be O.[[ShadowRealm]].
    auto& eval_realm = object->shadow_realm();

    // 7. Return ShadowRealmImportValue(specifierString, exportName, callerRealm, evalRealm).
    return shadow_realm_import_value(vm, move(specifier_string), export_name.as_string().byte_string(), *caller_realm, eval_realm);
}

}
