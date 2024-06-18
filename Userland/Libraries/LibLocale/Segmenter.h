/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace Locale {

enum class SegmenterGranularity {
    Grapheme,
    Sentence,
    Word,
};
SegmenterGranularity segmenter_granularity_from_string(StringView);
StringView segmenter_granularity_to_string(SegmenterGranularity);

class Segmenter {
public:
    static NonnullOwnPtr<Segmenter> create(SegmenterGranularity segmenter_granularity);
    static NonnullOwnPtr<Segmenter> create(StringView locale, SegmenterGranularity segmenter_granularity);
    virtual ~Segmenter() = default;

    SegmenterGranularity segmenter_granularity() const { return m_segmenter_granularity; }

    virtual NonnullOwnPtr<Segmenter> clone() const = 0;

    virtual void set_segmented_text(String) = 0;
    virtual void set_segmented_text(Utf16View const&) = 0;

    virtual size_t current_boundary() = 0;

    enum class Inclusive {
        No,
        Yes,
    };
    virtual Optional<size_t> previous_boundary(size_t index, Inclusive = Inclusive::No) = 0;
    virtual Optional<size_t> next_boundary(size_t index, Inclusive = Inclusive::No) = 0;

    using SegmentationCallback = Function<IterationDecision(size_t)>;
    virtual void for_each_boundary(String, SegmentationCallback) = 0;
    virtual void for_each_boundary(Utf16View const&, SegmentationCallback) = 0;
    virtual void for_each_boundary(Utf32View const&, SegmentationCallback) = 0;

    virtual bool is_current_boundary_word_like() const = 0;

protected:
    explicit Segmenter(SegmenterGranularity segmenter_granularity)
        : m_segmenter_granularity(segmenter_granularity)
    {
    }

    SegmenterGranularity m_segmenter_granularity { SegmenterGranularity::Grapheme };
};

}
