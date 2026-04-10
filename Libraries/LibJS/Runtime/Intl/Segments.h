/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibJS/Runtime/Intl/Segmenter.h>
#include <LibJS/Runtime/Object.h>
#include <LibUnicode/Segmenter.h>

namespace JS::Intl {

class Segments final : public Object {
    JS_OBJECT(Segments, Object);
    GC_DECLARE_ALLOCATOR(Segments);

public:
    static GC::Ref<Segments> create(Realm&, Unicode::Segmenter const&, GC::Ref<PrimitiveString>);

    virtual ~Segments() override = default;

    Unicode::Segmenter& segments_segmenter() const { return *m_segments_segmenter; }

    PrimitiveString const& segments_primitive_string() const { return m_segments_string_value; }
    Utf16String const& segments_string() const { return m_segments_string; }

private:
    Segments(Realm&, Unicode::Segmenter const&, GC::Ref<PrimitiveString>);

    virtual void visit_edges(Visitor&) override;

    NonnullOwnPtr<Unicode::Segmenter> m_segments_segmenter; // [[SegmentsSegmenter]]
    GC::Ref<PrimitiveString> m_segments_string_value;       // [[SegmentsStringValue]]
    Utf16String m_segments_string;                          // [[SegmentsString]]
};

}
