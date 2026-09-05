/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibUnicode/FullwidthMapping.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Segmenter.h>
#include <LibUnicode/TextMapping.h>
#include <unicode/translit.h>

namespace Unicode {

static void apply_fullwidth_mapping_to(Utf16View const& string, UnicodeTextMappingOutput output)
{
    UErrorCode status = U_ZERO_ERROR;
    auto const transliterator = adopt_own_if_nonnull(icu::Transliterator::createInstance("Halfwidth-Fullwidth", UTRANS_FORWARD, status));
    verify_icu_success(status);

    auto text = icu_string(string);
    auto segmenter = Segmenter::create(SegmenterGranularity::Grapheme);
    segmenter->set_segmented_text(string);

    size_t source_offset = 0;
    size_t destination_offset = 0;
    while (source_offset < string.length_in_code_units()) {
        auto const source_end_offset = segmenter->next_boundary(source_offset).value_or(string.length_in_code_units());
        auto const source_length = source_end_offset - source_offset;
        auto const destination_end_offset = transliterator->transliterate(text, destination_offset, destination_offset + source_length);
        auto const destination_length = destination_end_offset - destination_offset;
        if (source_length != destination_length) {
            output.append_edit(output.context,
                source_offset,
                source_length,
                destination_offset,
                destination_length);
        }
        source_offset = source_end_offset;
        destination_offset = destination_end_offset;
    }

    auto* destination = output.allocate_text(output.context, text.length());
    text.extract(0, text.length(), reinterpret_cast<char16_t*>(destination));
}

FullwidthMappingResult apply_fullwidth_mapping(Utf16String const& string)
{
    struct Output {
        Vector<u16> text;
        Vector<FullwidthMappingEdit> edits;
    } output;
    UnicodeTextMappingOutput sink {
        &output,
        [](void* context, size_t length) {
            auto& output = *static_cast<Output*>(context);
            output.text.resize(length);
            return output.text.data();
        },
        [](void* context, size_t start, size_t length, size_t mapped_start, size_t mapped_length) {
            static_cast<Output*>(context)->edits.append({ start, length, mapped_start, mapped_length });
        },
    };
    apply_fullwidth_mapping_to(string, sink);
    return { Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(output.text.data()), output.text.size() }), move(output.edits) };
}

}

extern "C" void unicode_apply_fullwidth_mapping(u16 const* text, size_t length, UnicodeTextMappingOutput output)
{
    Unicode::apply_fullwidth_mapping_to({ reinterpret_cast<char16_t const*>(text), length }, output);
}
