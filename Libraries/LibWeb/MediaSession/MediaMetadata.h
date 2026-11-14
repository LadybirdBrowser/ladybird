/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/MediaMetadata.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::MediaSession {

// https://w3c.github.io/mediasession/#convert-artwork-algorithm
WebIDL::ExceptionOr<Vector<Bindings::MediaImage>> convert_artwork(Vector<Bindings::MediaImage> const&);

// https://w3c.github.io/mediasession/#mediametadata
class MediaMetadata final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MediaMetadata, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MediaMetadata);

public:
    static WebIDL::ExceptionOr<GC::Ref<MediaMetadata>> create(Bindings::MediaMetadataInit const&);
    virtual ~MediaMetadata() override = default;

    Utf16String const& title() const { return m_title; }
    void set_title(Utf16String);
    Utf16String const& artist() const { return m_artist; }
    void set_artist(Utf16String);
    Utf16String const& album() const { return m_album; }
    void set_album(Utf16String);

    JS::Value artwork(JS::Realm&);
    WebIDL::ExceptionOr<void> set_artwork(JS::Realm&, GC::RootVector<GC::Ref<JS::Object>> const&);

    JS::Value chapter_info(JS::Realm&);

    void set_media_session(GC::Ptr<MediaSession>);

private:
    MediaMetadata() = default;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    void metadata_updated();

    Utf16String m_title;
    Utf16String m_artist;
    Utf16String m_album;
    Vector<Bindings::MediaImage> m_artwork_images;

    // The MediaMetadata's converted artwork images, created once for each wrapper world that gets them, and dropped
    // whenever the artwork is set.
    Bindings::WrapperWorldWeakValueCache<JS::Array> m_converted_artwork_images;
    // The cache holds its arrays weakly, so these keep them alive: Otherwise a script that keeps just an element of one
    // would get a new array, with new elements, after a collection.
    Vector<GC::Ref<JS::Array>> m_retained_converted_artwork_images;

    // https://w3c.github.io/mediasession/#mediametadata-chapter-information
    Vector<GC::Ref<ChapterInformation>> m_chapter_information;

    // The frozen array that the chapterInfo attribute returns, created once for each wrapper world that gets it.
    Bindings::WrapperWorldWeakValueCache<JS::Array> m_chapter_information_arrays;
    Vector<GC::Ref<JS::Array>> m_retained_chapter_information_arrays;

    GC::Ptr<MediaSession> m_media_session;
};

}
