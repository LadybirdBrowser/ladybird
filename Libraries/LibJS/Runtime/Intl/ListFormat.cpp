/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibUnicode/ListFormat.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(ListFormat);

// 14 ListFormat Objects, https://tc39.es/ecma402/#listformat-objects
ListFormat::ListFormat(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 14.5.2 CreatePartsFromList ( listFormat, list ), https://tc39.es/ecma402/#sec-createpartsfromlist
Vector<Unicode::ListFormat::Partition> create_parts_from_list(ListFormat const& list_format, ReadonlySpan<String> list)
{
    return list_format.formatter().format_to_parts(list);
}

// 14.5.3 FormatList ( listFormat, list ), https://tc39.es/ecma402/#sec-formatlist
String format_list(ListFormat const& list_format, ReadonlySpan<String> list)
{
    // 1. Let parts be ! CreatePartsFromList(listFormat, list).
    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    return list_format.formatter().format(list);
}

// 14.5.4 FormatListToParts ( listFormat, list ), https://tc39.es/ecma402/#sec-formatlisttoparts
GC::Ref<Array> format_list_to_parts(VM& vm, ListFormat const& list_format, ReadonlySpan<String> list)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ! CreatePartsFromList(listFormat, list).
    auto parts = create_parts_from_list(list_format, list);

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    size_t n = 0;

    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (auto& part : parts) {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // e. Increment n by 1.
        ++n;
    }

    // 5. Return result.
    return result;
}

// 14.5.5 StringListFromIterable ( iterable ), https://tc39.es/ecma402/#sec-createstringlistfromiterable
ThrowCompletionOr<Vector<String>> string_list_from_iterable(VM& vm, Value iterable)
{
    // 1. If iterable is undefined, then
    if (iterable.is_undefined()) {
        // a. Return a new empty List.
        return Vector<String> {};
    }

    // 2. Let iteratorRecord be ? GetIterator(iterable, sync).
    auto iterator_record = TRY(get_iterator(vm, iterable, IteratorHint::Sync));

    // 3. Let list be a new empty List.
    Vector<String> list;

    // 4. Repeat,
    while (true) {
        // a. Let next be ? IteratorStepValue(iteratorRecord).
        auto next = TRY(iterator_step_value(vm, iterator_record));

        // b. If next is DONE, then
        if (!next.has_value()) {
            // a. Return list.
            return list;
        }

        // c. If Type(next) is not String, then
        if (!next->is_string()) {
            // 1. Let error be ThrowCompletion(a newly created TypeError object).
            auto error = vm.throw_completion<TypeError>(ErrorType::NotAString, *next);

            // 2. Return ? IteratorClose(iteratorRecord, error).
            return iterator_close(vm, iterator_record, move(error));
        }

        // iii. Append next to list.
        list.append(next->as_string().utf8_string());
    }
}

}
