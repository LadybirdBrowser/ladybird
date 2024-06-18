/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Segmenter.h>
#include <LibJS/Runtime/Intl/SegmenterPrototype.h>
#include <LibJS/Runtime/Intl/Segments.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(SegmenterPrototype);

// 18.3 Properties of the Intl.Segmenter Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-segmenter-prototype-object
SegmenterPrototype::SegmenterPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void SegmenterPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 18.3.2 Intl.Segmenter.prototype [ @@toStringTag ], https://tc39.es/ecma402/#sec-intl.segmenter.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.Segmenter"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.resolvedOptions, resolved_options, 0, attr);
    define_native_function(realm, vm.names.segment, segment, 1, attr);
}

// 18.3.4 Intl.Segmenter.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.segmenter.prototype.resolvedoptions
JS_DEFINE_NATIVE_FUNCTION(SegmenterPrototype::resolved_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let segmenter be the this value.
    // 2. Perform ? RequireInternalSlot(segmenter, [[InitializedSegmenter]]).
    auto segmenter = TRY(typed_this_object(vm));

    // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
    auto options = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. For each row of Table 16, except the header row, in table order, do
    //     a. Let p be the Property value of the current row.
    //     b. Let v be the value of segmenter's internal slot whose name is the Internal Slot value of the current row.
    //     c. Assert: v is not undefined.
    //     d. Perform ! CreateDataPropertyOrThrow(options, p, v).
    MUST(options->create_data_property_or_throw(vm.names.locale, PrimitiveString::create(vm, segmenter->locale())));
    MUST(options->create_data_property_or_throw(vm.names.granularity, PrimitiveString::create(vm, segmenter->segmenter_granularity_string())));

    // 5. Return options.
    return options;
}

// 18.3.3 Intl.Segmenter.prototype.segment ( string ), https://tc39.es/ecma402/#sec-intl.segmenter.prototype.segment
JS_DEFINE_NATIVE_FUNCTION(SegmenterPrototype::segment)
{
    auto& realm = *vm.current_realm();

    // 1. Let segmenter be the this value.
    // 2. Perform ? RequireInternalSlot(segmenter, [[InitializedSegmenter]]).
    auto segmenter = TRY(typed_this_object(vm));

    // 3. Let string be ? ToString(string).
    auto string = TRY(vm.argument(0).to_utf16_string(vm));

    // 4. Return ! CreateSegmentsObject(segmenter, string).
    return Segments::create(realm, segmenter->segmenter(), move(string));
}

}
