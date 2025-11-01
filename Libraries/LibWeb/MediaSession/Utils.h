#pragma once

#include <LibWeb/MediaSession/ChapterInformation.h>

namespace Web::MediaSession {

// https://www.w3.org/TR/mediasession/#convert-artwork-algorithm
WebIDL::ExceptionOr<Vector<MediaImage>> convert_artwork(Vector<MediaImage> const& artwork);
WebIDL::ExceptionOr<GC::RootVector<JS::Object*>> convert_artwork_to_js(JS::Realm& realm, Vector<MediaImage> const& artwork);
WebIDL::ExceptionOr<Vector<MediaImage>> convert_artwork_to_cpp(Vector<GC::Root<JS::Object>> const& artwork);

}
