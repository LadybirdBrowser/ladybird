/*
 * Copyright (c) 2021-2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/Intl/ListFormatPrototype.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(ListFormatPrototype);

// 13.3 Properties of the Intl.ListFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-listformat-prototype-object
ListFormatPrototype::ListFormatPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void ListFormatPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 13.3.2 Intl.ListFormat.prototype [ @@toStringTag ], https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype-toStringTag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.ListFormat"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.format, format, 1, attr);
    define_native_function(realm, vm.names.formatToParts, format_to_parts, 1, attr);
    define_native_function(realm, vm.names.resolvedOptions, resolved_options, 0, attr);
}

// 13.3.3 Intl.ListFormat.prototype.format ( list ), https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype.format
JS_DEFINE_NATIVE_FUNCTION(ListFormatPrototype::format)
{
    auto list = vm.argument(0);

    // 1. Let lf be the this value.
    // 2. Perform ? RequireInternalSlot(lf, [[InitializedListFormat]]).
    auto list_format = TRY(typed_this_object(vm));

    // 3. Let stringList be ? StringListFromIterable(list).
    auto string_list = TRY(string_list_from_iterable(vm, list));

    // 4. Return ! FormatList(lf, stringList).
    auto formatted = format_list(list_format, string_list);
    return PrimitiveString::create(vm, move(formatted));
}

// 13.3.4 Intl.ListFormat.prototype.formatToParts ( list ), https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype.formatToParts
JS_DEFINE_NATIVE_FUNCTION(ListFormatPrototype::format_to_parts)
{
    auto list = vm.argument(0);

    // 1. Let lf be the this value.
    // 2. Perform ? RequireInternalSlot(lf, [[InitializedListFormat]]).
    auto list_format = TRY(typed_this_object(vm));

    // 3. Let stringList be ? StringListFromIterable(list).
    auto string_list = TRY(string_list_from_iterable(vm, list));

    // 4. Return ! FormatListToParts(lf, stringList).
    return format_list_to_parts(vm, list_format, string_list);
}

// 13.3.5 Intl.ListFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-Intl.ListFormat.prototype.resolvedoptions
JS_DEFINE_NATIVE_FUNCTION(ListFormatPrototype::resolved_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let lf be the this value.
    // 2. Perform ? RequireInternalSlot(lf, [[InitializedListFormat]]).
    auto list_format = TRY(typed_this_object(vm));

    // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
    auto options = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. For each row of Table 10, except the header row, in table order, do
    //     a. Let p be the Property value of the current row.
    //     b. Let v be the value of lf's internal slot whose name is the Internal Slot value of the current row.
    //     c. Assert: v is not undefined.
    //     d. Perform ! CreateDataPropertyOrThrow(options, p, v).
    MUST(options->create_data_property_or_throw(vm.names.locale, PrimitiveString::create(vm, list_format->locale())));
    MUST(options->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, list_format->type_string())));
    MUST(options->create_data_property_or_throw(vm.names.style, PrimitiveString::create(vm, list_format->style_string())));

    // 5. Return options.
    return options;
}

}
