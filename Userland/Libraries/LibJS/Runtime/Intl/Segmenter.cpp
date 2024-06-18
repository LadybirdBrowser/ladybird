/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Segmenter.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(Segmenter);

// 18 Segmenter Objects, https://tc39.es/ecma402/#segmenter-objects
Segmenter::Segmenter(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 18.7.1 CreateSegmentDataObject ( segmenter, string, startIndex, endIndex ), https://tc39.es/ecma402/#sec-createsegmentdataobject
ThrowCompletionOr<NonnullGCPtr<Object>> create_segment_data_object(VM& vm, ::Locale::Segmenter const& segmenter, Utf16View const& string, size_t start_index, size_t end_index)
{
    auto& realm = *vm.current_realm();

    // 1. Let len be the length of string.
    auto length = string.length_in_code_units();

    // 2. Assert: startIndex ≥ 0.
    // NOTE: This is always true because the type is size_t.

    // 3. Assert: endIndex ≤ len.
    VERIFY(end_index <= length);

    // 4. Assert: startIndex < endIndex.
    VERIFY(start_index < end_index);

    // 5. Let result be OrdinaryObjectCreate(%Object.prototype%).
    auto result = Object::create(realm, realm.intrinsics().object_prototype());

    // 6. Let segment be the substring of string from startIndex to endIndex.
    auto segment = string.substring_view(start_index, end_index - start_index);

    // 7. Perform ! CreateDataPropertyOrThrow(result, "segment", segment).
    MUST(result->create_data_property_or_throw(vm.names.segment, PrimitiveString::create(vm, Utf16String::create(segment))));

    // 8. Perform ! CreateDataPropertyOrThrow(result, "index", 𝔽(startIndex)).
    MUST(result->create_data_property_or_throw(vm.names.index, Value(start_index)));

    // 9. Perform ! CreateDataPropertyOrThrow(result, "input", string).
    MUST(result->create_data_property_or_throw(vm.names.input, PrimitiveString::create(vm, Utf16String::create(string))));

    // 10. Let granularity be segmenter.[[SegmenterGranularity]].
    auto granularity = segmenter.segmenter_granularity();

    // 11. If granularity is "word", then
    if (granularity == ::Locale::SegmenterGranularity::Word) {
        // a. Let isWordLike be a Boolean value indicating whether the segment in string is "word-like" according to locale segmenter.[[Locale]].
        auto is_word_like = segmenter.is_current_boundary_word_like();

        // b. Perform ! CreateDataPropertyOrThrow(result, "isWordLike", isWordLike).
        MUST(result->create_data_property_or_throw(vm.names.isWordLike, Value(is_word_like)));
    }

    // 12. Return result.
    return result;
}

// 18.8.1 FindBoundary ( segmenter, string, startIndex, direction ), https://tc39.es/ecma402/#sec-findboundary
size_t find_boundary(::Locale::Segmenter& segmenter, Utf16View const& string, size_t start_index, Direction direction)
{
    // 1. Let len be the length of string.
    auto length = string.length_in_code_units();

    // 2. Assert: startIndex < len.
    VERIFY(start_index < length);

    // 3. Let locale be segmenter.[[Locale]].
    // 4. Let granularity be segmenter.[[SegmenterGranularity]].

    // 5. If direction is before, then
    if (direction == Direction::Before) {
        // a. Search string for the last segmentation boundary that is preceded by at most startIndex code units from
        //    the beginning, using locale locale and text element granularity granularity.
        auto boundary = segmenter.previous_boundary(start_index, ::Locale::Segmenter::Inclusive::Yes);

        // b. If a boundary is found, return the count of code units in string preceding it.
        if (boundary.has_value())
            return *boundary;

        // c. Return 0.
        return 0;
    }

    // 6. Assert: direction is after.
    // 7. Search string for the first segmentation boundary that follows the code unit at index startIndex, using locale
    //    locale and text element granularity granularity.
    auto boundary = segmenter.next_boundary(start_index);

    // 8. If a boundary is found, return the count of code units in string preceding it.
    if (boundary.has_value())
        return *boundary;

    // 9. Return len.
    return length;
}

}
