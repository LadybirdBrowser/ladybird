/*
 * Copyright (c) 2021, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GeneratorFunctionConstructor.h>
#include <LibJS/Runtime/GeneratorFunctionPrototype.h>
#include <LibJS/Runtime/GeneratorPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(GeneratorFunctionPrototype);

GeneratorFunctionPrototype::GeneratorFunctionPrototype(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().function_prototype())
{
}

void GeneratorFunctionPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 27.3.3.2 GeneratorFunction.prototype.prototype, https://tc39.es/ecma262/#sec-generatorfunction.prototype.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().generator_prototype(), Attribute::Configurable);
    // 27.3.3.3 GeneratorFunction.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-generatorfunction.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "GeneratorFunction"_string), Attribute::Configurable);
}

}
