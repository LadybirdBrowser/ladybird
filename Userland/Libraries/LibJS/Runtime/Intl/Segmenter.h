/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Runtime/Object.h>
#include <LibUnicode/Segmenter.h>

namespace JS::Intl {

class Segmenter final : public Object {
    JS_OBJECT(Segmenter, Object);
    JS_DECLARE_ALLOCATOR(Segmenter);

public:
    virtual ~Segmenter() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    Unicode::SegmenterGranularity segmenter_granularity() const { return m_segmenter_granularity; }
    void set_segmenter_granularity(StringView segmenter_granularity) { m_segmenter_granularity = Unicode::segmenter_granularity_from_string(segmenter_granularity); }
    StringView segmenter_granularity_string() const { return Unicode::segmenter_granularity_to_string(m_segmenter_granularity); }

    Unicode::Segmenter const& segmenter() const { return *m_segmenter; }
    void set_segmenter(NonnullOwnPtr<Unicode::Segmenter> segmenter) { m_segmenter = move(segmenter); }

private:
    explicit Segmenter(Object& prototype);

    String m_locale;                                                                                   // [[Locale]]
    Unicode::SegmenterGranularity m_segmenter_granularity { Unicode::SegmenterGranularity::Grapheme }; // [[SegmenterGranularity]]

    // Non-standard. Stores the ICU segmenter for the Intl object's segmentation options.
    OwnPtr<Unicode::Segmenter> m_segmenter;
};

ThrowCompletionOr<NonnullGCPtr<Object>> create_segment_data_object(VM&, Unicode::Segmenter const&, Utf16View const&, size_t start_index, size_t end_index);

enum class Direction {
    Before,
    After,
};
size_t find_boundary(Unicode::Segmenter&, Utf16View const&, size_t start_index, Direction);

}
