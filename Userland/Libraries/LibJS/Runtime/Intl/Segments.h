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

class Segments final : public Object {
    JS_OBJECT(Segments, Object);
    JS_DECLARE_ALLOCATOR(Segments);

public:
    static NonnullGCPtr<Segments> create(Realm&, ::Locale::Segmenter const&, Utf16String);

    virtual ~Segments() override = default;

    ::Locale::Segmenter& segments_segmenter() const { return *m_segments_segmenter; }

    Utf16View segments_string() const { return m_segments_string.view(); }

private:
    Segments(Realm&, ::Locale::Segmenter const&, Utf16String);

    NonnullOwnPtr<::Locale::Segmenter> m_segments_segmenter; // [[SegmentsSegmenter]]
    Utf16String m_segments_string;                           // [[SegmentsString]]
};

}
