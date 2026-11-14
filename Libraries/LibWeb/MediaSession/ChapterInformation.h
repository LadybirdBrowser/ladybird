/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/ChapterInformation.h>
#include <LibWeb/Bindings/MediaMetadata.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::MediaSession {

// https://w3c.github.io/mediasession/#chapterinformation
class ChapterInformation final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(ChapterInformation, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(ChapterInformation);

public:
    static WebIDL::ExceptionOr<GC::Ref<ChapterInformation>> create(Bindings::ChapterInformationInit const&);
    virtual ~ChapterInformation() override = default;

    Utf16String const& title() const { return m_title; }
    double start_time() const { return m_start_time; }
    JS::Value artwork(JS::Realm&);

private:
    ChapterInformation() = default;

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Utf16String m_title;
    double m_start_time { 0 };
    Vector<Bindings::MediaImage> m_artwork_images;

    // The frozen array that the artwork attribute returns, created once for each wrapper world that gets it.
    Bindings::WrapperWorldWeakValueCache<JS::Array> m_artwork_arrays;
    // The cache holds its arrays weakly, so these keep them alive: Otherwise a script that keeps just an element of one
    // would get a new array, with new elements, after a collection.
    Vector<GC::Ref<JS::Array>> m_retained_artwork_arrays;
};

}
