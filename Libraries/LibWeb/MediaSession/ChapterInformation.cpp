/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/MediaSession/ChapterInformation.h>
#include <LibWeb/MediaSession/MediaMetadata.h>

namespace Web::MediaSession {

GC_DEFINE_ALLOCATOR(ChapterInformation);

// https://w3c.github.io/mediasession/#create-a-chapterinformation
WebIDL::ExceptionOr<GC::Ref<ChapterInformation>> ChapterInformation::create(Bindings::ChapterInformationInit const& init)
{
    // 1. Let chapterInfo be a new ChapterInformation object.
    auto chapter_info = GC::Heap::the().allocate<ChapterInformation>();

    // 2. Set chapterInfo's title to init's title.
    chapter_info->m_title = init.title;

    // 3. Set chapterInfo's startTime to init's startTime. If the startTime is negative or greater than duration, throw
    //    a TypeError.
    // NB: Nothing here has a duration, so only a negative startTime is rejected.
    if (init.start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Chapter startTime must not be negative"_utf16 };
    chapter_info->m_start_time = init.start_time;

    // 4. Let artwork be the result of running the convert artwork algorithm with init's artwork as input.
    // 5. Set chapterInfo's artwork images to the result of creating a frozen array from artwork.
    // NB: The frozen array is created when the artwork attribute is first read in a wrapper world.
    chapter_info->m_artwork_images = TRY(convert_artwork(init.artwork));

    // 6. Return chapterInfo.
    return chapter_info;
}

void ChapterInformation::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_retained_artwork_arrays);
}

// https://w3c.github.io/mediasession/#dom-chapterinformation-artwork
JS::Value ChapterInformation::artwork(JS::Realm& realm)
{
    // On getting, it MUST return the ChapterInformation's artwork images.
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    if (auto array = m_artwork_arrays.get(wrapper_world))
        return array.ptr();

    auto array = MUST(JS::Array::create(realm, 0));
    for (size_t index = 0; index < m_artwork_images.size(); ++index)
        MUST(array->create_data_property_or_throw(index, Bindings::media_image_to_value(realm, m_artwork_images[index])));
    MUST(array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    m_artwork_arrays.set(wrapper_world, array);
    m_retained_artwork_arrays.append(array);
    return array;
}

}
