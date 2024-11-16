/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Wtf16ByteView.h>
#include <LibJS/Runtime/Intl/Segmenter.h>
#include <LibJS/Runtime/Object.h>
#include <LibUnicode/Segmenter.h>

namespace JS::Intl {

class SegmentIterator final : public Object {
    JS_OBJECT(SegmentIterator, Object);
    GC_DECLARE_ALLOCATOR(SegmentIterator);

public:
    static GC::Ref<SegmentIterator> create(Realm&, Unicode::Segmenter const&, Wtf16ByteView const&, Segments const&);

    virtual ~SegmentIterator() override = default;

    Unicode::Segmenter& iterating_segmenter() { return *m_iterating_segmenter; }
    Wtf16ByteView const& iterated_string() const { return m_iterated_string; }
    size_t iterated_string_next_segment_code_unit_index() const { return m_iterating_segmenter->current_boundary(); }

    Segments const& segments() { return m_segments; }

private:
    SegmentIterator(Realm&, Unicode::Segmenter const&, Wtf16ByteView const&, Segments const&);

    virtual void visit_edges(Cell::Visitor&) override;

    NonnullOwnPtr<Unicode::Segmenter> m_iterating_segmenter; // [[IteratingSegmenter]]
    Wtf16ByteView m_iterated_string;                         // [[IteratedString]]

    GC::Ref<Segments const> m_segments;
};

}
