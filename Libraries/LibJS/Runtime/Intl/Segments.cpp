/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Segments.h>
#include <LibJS/Runtime/Intl/SegmentsPrototype.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(Segments);

// 19.5.1 CreateSegmentsObject ( segmenter, string ), https://tc39.es/ecma402/#sec-createsegmentsobject
GC::Ref<Segments> Segments::create(Realm& realm, Unicode::Segmenter const& segmenter, Utf16String string)
{
    // 1. Let internalSlotsList be « [[SegmentsSegmenter]], [[SegmentsString]] ».
    // 2. Let segments be OrdinaryObjectCreate(%SegmentsPrototype%, internalSlotsList).
    // 3. Set segments.[[SegmentsSegmenter]] to segmenter.
    // 4. Set segments.[[SegmentsString]] to string.
    // 5. Return segments.
    return realm.create<Segments>(realm, segmenter, move(string));
}

// 19.5 Segments Objects, https://tc39.es/ecma402/#sec-segments-objects
Segments::Segments(Realm& realm, Unicode::Segmenter const& segmenter, Utf16String string)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().intl_segments_prototype())
    , m_segments_segmenter(segmenter.clone())
    , m_segments_string(move(string))
{
    m_segments_segmenter->set_segmented_text(m_segments_string.view());
}

}
