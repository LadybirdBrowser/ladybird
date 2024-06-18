/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibJS/Runtime/Intl/Segmenter.h>
#include <LibJS/Runtime/Object.h>
#include <LibLocale/Segmenter.h>

namespace JS::Intl {

class SegmentIterator final : public Object {
    JS_OBJECT(SegmentIterator, Object);
    JS_DECLARE_ALLOCATOR(SegmentIterator);

public:
    static NonnullGCPtr<SegmentIterator> create(Realm&, ::Locale::Segmenter const&, Utf16View const&, Segments const&);

    virtual ~SegmentIterator() override = default;

    ::Locale::Segmenter& iterating_segmenter() { return *m_iterating_segmenter; }
    Utf16View const& iterated_string() const { return m_iterated_string; }
    size_t iterated_string_next_segment_code_unit_index() const { return m_iterating_segmenter->current_boundary(); }

    Segments const& segments() { return m_segments; }

private:
    SegmentIterator(Realm&, ::Locale::Segmenter const&, Utf16View const&, Segments const&);

    virtual void visit_edges(Cell::Visitor&) override;

    NonnullOwnPtr<::Locale::Segmenter> m_iterating_segmenter; // [[IteratingSegmenter]]
    Utf16View m_iterated_string;                              // [[IteratedString]]

    NonnullGCPtr<Segments const> m_segments;
};

}
