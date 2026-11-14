/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/MediaSession/ChapterInformation.h>
#include <LibWeb/MediaSession/MediaMetadata.h>
#include <LibWeb/MediaSession/MediaSession.h>

namespace Web::MediaSession {

GC_DEFINE_ALLOCATOR(MediaMetadata);

// https://w3c.github.io/mediasession/#convert-artwork-algorithm
WebIDL::ExceptionOr<Vector<Bindings::MediaImage>> convert_artwork(Vector<Bindings::MediaImage> const& input)
{
    // 1. Let output be an empty list of type MediaImage.
    Vector<Bindings::MediaImage> output;

    // 2. For each entry in input (which is a MediaImage list), perform the following steps:
    for (auto const& entry : input) {
        // 1. Let image be a new MediaImage.
        Bindings::MediaImage image;

        // 2. Let baseURL be the API base URL specified by the entry settings object.
        // AD-HOC: The spec resolves against the entry settings object's base URL, but we use the current settings object
        //         instead, as the web-platform-tests do: A script that builds artwork with another realm's constructor
        //         gets URLs resolved against that realm's base URL.
        auto base_url = HTML::current_settings_object().api_base_url();

        // 3. Parse entry's src using baseURL. If it does not return failure, set image's src to the return value.
        //    Otherwise, throw a TypeError and abort these steps.
        auto url = DOMURL::parse(entry.src, base_url);
        if (!url.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Artwork src could not be parsed as a URL"_utf16 };
        image.src = utf16_string_from_url_ascii(url->serialize());

        // 4. Set image's sizes to entry's sizes.
        image.sizes = entry.sizes;

        // 5. Set image's type to entry's type.
        image.type = entry.type;

        // 6. Append image to the output.
        output.append(move(image));
    }

    // 3. Return output as result.
    return output;
}

// https://w3c.github.io/mediasession/#dom-mediametadata-mediametadata
WebIDL::ExceptionOr<GC::Ref<MediaMetadata>> MediaMetadata::create(Bindings::MediaMetadataInit const& init)
{
    // 1. Let metadata be a new MediaMetadata object.
    auto metadata = GC::Heap::the().allocate<MediaMetadata>();

    // 2. Set metadata's title to init's title.
    metadata->m_title = init.title;

    // 3. Set metadata's artist to init's artist.
    metadata->m_artist = init.artist;

    // 4. Set metadata's album to init's album.
    metadata->m_album = init.album;

    // 5. Run the convert artwork algorithm with init's artwork as input and set metadata's artwork images as the
    //    result if it succeeded.
    metadata->m_artwork_images = TRY(convert_artwork(init.artwork));

    // 6. Let chapters be an empty list of type ChapterInformation.
    // 7. For each entry in init's chapterInfo, create a ChapterInformation from entry and append it to chapters.
    // 8. Set metadata's chapter information to the result of creating a frozen array from chapters.
    // NB: The chapters go straight into metadata's chapter information, which keeps them alive. The frozen array is
    //     created when the chapterInfo attribute is first read in a wrapper world.
    for (auto const& entry : init.chapter_info)
        metadata->m_chapter_information.append(TRY(ChapterInformation::create(entry)));

    // 9. Return metadata.
    return metadata;
}

void MediaMetadata::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_retained_converted_artwork_images);
    visitor.visit(m_chapter_information);
    visitor.visit(m_retained_chapter_information_arrays);
    visitor.visit(m_media_session);
}

void MediaMetadata::set_media_session(GC::Ptr<MediaSession> session)
{
    m_media_session = session;
}

// https://w3c.github.io/mediasession/#dom-mediametadata-title
void MediaMetadata::set_title(Utf16String value)
{
    // On setting, it MUST set the MediaMetadata's title to the given value.
    m_title = move(value);
    metadata_updated();
}

// https://w3c.github.io/mediasession/#dom-mediametadata-artist
void MediaMetadata::set_artist(Utf16String value)
{
    // On setting, it MUST set the MediaMetadata's artist to the given value.
    m_artist = move(value);
    metadata_updated();
}

// https://w3c.github.io/mediasession/#dom-mediametadata-album
void MediaMetadata::set_album(Utf16String value)
{
    // On setting, it MUST set the MediaMetadata's album to the given value.
    m_album = move(value);
    metadata_updated();
}

// https://w3c.github.io/mediasession/#dom-mediametadata-artwork
JS::Value MediaMetadata::artwork(JS::Realm& realm)
{
    // 1. If the MediaMetadata's converted artwork images is undefined, run the following steps:
    // NB: We keep the converted artwork images for each wrapper world that reads them, so that no world gets another's
    //     array.
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto converted_artwork_images = m_converted_artwork_images.get(wrapper_world);
    if (!converted_artwork_images) {
        // 1. Let frozenArtwork be a JavaScript Array value.
        auto frozen_artwork = MUST(JS::Array::create(realm, 0));

        // 2. For each entry in the MediaMetadata's artwork images, perform the following steps:
        for (size_t index = 0; index < m_artwork_images.size(); ++index) {
            // 1. Let image be the result of converting to a JavaScript object entry.
            auto image = Bindings::media_image_to_value(realm, m_artwork_images[index]);

            // 2. Perform SetIntegrityLevel(image, "frozen"), to prevent accidental mutation by scripts.
            MUST(image.as_object().set_integrity_level(JS::Object::IntegrityLevel::Frozen));

            // 3. Push image to frozenArtwork.
            MUST(frozen_artwork->create_data_property_or_throw(index, image));
        }

        // 3. Perform SetIntegrityLevel(frozenArtwork, "frozen").
        MUST(frozen_artwork->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

        // 4. Set the MediaMetadata's converted artwork images to frozenArtwork.
        m_converted_artwork_images.set(wrapper_world, frozen_artwork);
        m_retained_converted_artwork_images.append(frozen_artwork);
        converted_artwork_images = frozen_artwork;
    }

    // 2. Return the MediaMetadata's converted artwork images.
    return converted_artwork_images.ptr();
}

// https://w3c.github.io/mediasession/#dom-mediametadata-chapterinfo
JS::Value MediaMetadata::chapter_info(JS::Realm& realm)
{
    // On getting, it MUST return the MediaMetadata's chapter information.
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    if (auto array = m_chapter_information_arrays.get(wrapper_world))
        return array.ptr();

    auto array = MUST(JS::Array::create(realm, 0));
    for (size_t index = 0; index < m_chapter_information.size(); ++index) {
        auto wrapper = Bindings::wrap(wrapper_world, realm, m_chapter_information[index]);

        // AD-HOC: The spec freezes only the array, but each ChapterInformation object is frozen too, as the
        //         web-platform-tests expect.
        MUST(wrapper->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
        MUST(array->create_data_property_or_throw(index, wrapper));
    }
    MUST(array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    m_chapter_information_arrays.set(wrapper_world, array);
    m_retained_chapter_information_arrays.append(array);
    return array;
}

// https://w3c.github.io/mediasession/#dom-mediametadata-artwork
WebIDL::ExceptionOr<void> MediaMetadata::set_artwork(JS::Realm& realm, GC::RootVector<GC::Ref<JS::Object>> const& value)
{
    // 1. Let convertedArtwork be the result of converting value to a sequence of type MediaImage.
    Vector<Bindings::MediaImage> converted_artwork;
    for (auto const& entry : value)
        converted_artwork.append(TRY(Bindings::convert_to_idl_value_for_media_image(realm.vm(), entry)));

    // 2. Run convert artwork algorithm with convertedArtwork, and set the MediaMetadata's artwork images as the
    //    result if it succeeds.
    m_artwork_images = TRY(convert_artwork(converted_artwork));

    // 3. Set the MediaMetadata's converted artwork images to undefined.
    m_converted_artwork_images.clear();
    m_retained_converted_artwork_images.clear();
    metadata_updated();
    return {};
}

// https://w3c.github.io/mediasession/#the-mediametadata-interface
void MediaMetadata::metadata_updated()
{
    // 1. If the instance has no associated media session, abort these steps.
    if (!m_media_session)
        return;

    // FIXME: 2. Otherwise, queue a task to run the following substeps:
    //           1. If the instance no longer has an associated media session, abort these steps.
    //           2. Otherwise, in parallel, run the update metadata algorithm.
}

}
