/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/SegmentIterator.h>
#include <LibJS/Runtime/Intl/SegmentsPrototype.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(SegmentsPrototype);

// 18.5.2 The %SegmentsPrototype% Object, https://tc39.es/ecma402/#sec-%segmentsprototype%-object
SegmentsPrototype::SegmentsPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void SegmentsPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.well_known_symbol_iterator(), symbol_iterator, 0, attr);
    define_native_function(realm, vm.names.containing, containing, 1, attr);
}

// 18.5.2.1 %SegmentsPrototype%.containing ( index ), https://tc39.es/ecma402/#sec-%segmentsprototype%.containing
JS_DEFINE_NATIVE_FUNCTION(SegmentsPrototype::containing)
{
    // 1. Let segments be the this value.
    // 2. Perform ? RequireInternalSlot(segments, [[SegmentsSegmenter]]).
    auto segments = TRY(typed_this_object(vm));

    // 3. Let segmenter be segments.[[SegmentsSegmenter]].
    auto& segmenter = segments->segments_segmenter();

    // 4. Let string be segments.[[SegmentsString]].
    auto const& string = segments->segments_string();

    // 5. Let len be the length of string.
    auto length = string.length_in_code_units();

    // 6. Let n be ? ToIntegerOrInfinity(index).
    auto n = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 7. If n < 0 or n ≥ len, return undefined.
    if (n < 0 || n >= static_cast<double>(length))
        return js_undefined();

    // 8. Let startIndex be FindBoundary(segmenter, string, n, before).
    auto start_index = find_boundary(segmenter, string, static_cast<size_t>(n), Direction::Before);

    // 9. Let endIndex be FindBoundary(segmenter, string, n, after).
    auto end_index = find_boundary(segmenter, string, static_cast<size_t>(n), Direction::After);

    // 10. Return CreateSegmentDataObject(segmenter, string, startIndex, endIndex).
    return TRY(create_segment_data_object(vm, segmenter, string, start_index, end_index));
}

// 18.5.2.2 %SegmentsPrototype% [ @@iterator ] ( ), https://tc39.es/ecma402/#sec-%segmentsprototype%-@@iterator
JS_DEFINE_NATIVE_FUNCTION(SegmentsPrototype::symbol_iterator)
{
    auto& realm = *vm.current_realm();

    // 1. Let segments be the this value.
    // 2. Perform ? RequireInternalSlot(segments, [[SegmentsSegmenter]]).
    auto segments = TRY(typed_this_object(vm));

    // 3. Let segmenter be segments.[[SegmentsSegmenter]].
    auto& segmenter = segments->segments_segmenter();

    // 4. Let string be segments.[[SegmentsString]].
    auto string = segments->segments_string();

    // 5. Return ! CreateSegmentIterator(segmenter, string).
    return SegmentIterator::create(realm, segmenter, string, segments);
}

}
