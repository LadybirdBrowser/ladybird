/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibUnicode/FullwidthMapping.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Segmenter.h>
#include <unicode/translit.h>

namespace Unicode {

FullwidthMappingResult apply_fullwidth_mapping(Utf16String const& string)
{
    UErrorCode status = U_ZERO_ERROR;
    auto const transliterator = adopt_own_if_nonnull(icu::Transliterator::createInstance("Halfwidth-Fullwidth", UTRANS_FORWARD, status));
    verify_icu_success(status);

    auto text = icu_string(string);
    auto segmenter = Segmenter::create(SegmenterGranularity::Grapheme);
    segmenter->set_segmented_text(string);

    Vector<FullwidthMappingEdit> edits;
    size_t source_offset = 0;
    size_t destination_offset = 0;
    while (source_offset < string.length_in_code_units()) {
        auto const source_end_offset = segmenter->next_boundary(source_offset).value_or(string.length_in_code_units());
        auto const source_length = source_end_offset - source_offset;
        auto const destination_end_offset = transliterator->transliterate(text, destination_offset, destination_offset + source_length);
        auto const destination_length = destination_end_offset - destination_offset;
        if (source_length != destination_length) {
            edits.append({
                source_offset,
                source_length,
                destination_offset,
                destination_length,
            });
        }
        source_offset = source_end_offset;
        destination_offset = destination_end_offset;
    }

    return { icu_string_to_utf16_string(text), move(edits) };
}

}
