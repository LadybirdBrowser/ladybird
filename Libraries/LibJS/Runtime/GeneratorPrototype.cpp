/*
 * Copyright (c) 2021, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GeneratorPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(GeneratorPrototype);

GeneratorPrototype::GeneratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void GeneratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.next, next, 1, attr);
    define_native_function(realm, vm.names.return_, return_, 1, attr);
    define_native_function(realm, vm.names.throw_, throw_, 1, attr);

    // 27.5.1.5 Generator.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-generator.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Generator"_string), Attribute::Configurable);
}

// 27.5.1.2 Generator.prototype.next ( value ), https://tc39.es/ecma262/#sec-generator.prototype.next
JS_DEFINE_NATIVE_FUNCTION(GeneratorPrototype::next)
{
    // 1. Return ? GeneratorResume(this value, value, empty).
    auto generator_object = TRY(typed_this_object(vm));
    return generator_object->resume(vm, vm.argument(0), {});
}

// 27.5.1.3 Generator.prototype.return ( value ), https://tc39.es/ecma262/#sec-generator.prototype.return
JS_DEFINE_NATIVE_FUNCTION(GeneratorPrototype::return_)
{
    // 1. Let g be the this value.
    auto generator_object = TRY(typed_this_object(vm));

    // 2. Let C be Completion Record { [[Type]]: return, [[Value]]: value, [[Target]]: empty }.
    auto completion = Completion(Completion::Type::Return, vm.argument(0));

    // 3. Return ? GeneratorResumeAbrupt(g, C, empty).
    return generator_object->resume_abrupt(vm, completion, {});
}

// 27.5.1.4 Generator.prototype.throw ( exception ), https://tc39.es/ecma262/#sec-generator.prototype.throw
JS_DEFINE_NATIVE_FUNCTION(GeneratorPrototype::throw_)
{
    // 1. Let g be the this value.
    auto generator_object = TRY(typed_this_object(vm));

    // 2. Let C be ThrowCompletion(exception).
    auto completion = throw_completion(vm.argument(0));

    // 3. Return ? GeneratorResumeAbrupt(g, C, empty).
    return generator_object->resume_abrupt(vm, completion, {});
}

}
