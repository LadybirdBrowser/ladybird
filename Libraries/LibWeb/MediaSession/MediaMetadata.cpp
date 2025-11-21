#include <LibWeb/MediaSession/ChapterInformation.h>
#include <LibWeb/Bindings/MediaMetadataPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/MediaSession/MediaMetadata.h>
#include <LibWeb/MediaSession/Utils.h>

namespace Web::MediaSession {

GC_DEFINE_ALLOCATOR(MediaMetadata);

WebIDL::ExceptionOr<GC::Ref<MediaMetadata>> MediaMetadata::construct_impl(JS::Realm& realm, MediaMetadataInit const& init) {
    Vector<GC::Ref<ChapterInformation>> chapters;

    // 1. Let metadata be a new MediaMetadata object.
    // 2. Set metadata’s title to init’s title.
    // 3. Set metadata’s artist to init’s artist.
    // 4. Set metadata’s album to init’s album.
    // 5. Run the convert artwork algorithm with init’s artwork as input and set metadata’s artwork images as the result if it succeeded.
    TRY(convert_artwork(init.artwork));

    // 6. Let chapters be an empty list of type ChapterInformation.
    // 7. For each entry in init’s chapterInfo, create a ChapterInformation from entry and append it to chapters.
    for (const auto& entry : init.chapter_info) {
        auto chapter = TRY(ChapterInformation::create(realm, entry));
        chapters.append(chapter);
    }
    // 8. Set metadata’s chapter information to the result of creating a frozen array from chapters.
    // 9. Return metadata.
    return realm.create<MediaMetadata>(realm, init.title, init.artist, init.album, init.artwork, chapters);
}

MediaMetadata::MediaMetadata(JS::Realm& realm, String title, String artist, String album, Vector<MediaImage> artwork, Vector<GC::Ref<ChapterInformation>> chapters)
    : PlatformObject(realm)
    , m_title(move(title))
    , m_artist(move(artist))
    , m_album(move(album))
    , m_artwork(move(artwork))
    , m_chapter_info(move(chapters))
{
}

MediaMetadata::~MediaMetadata() = default;

void MediaMetadata::finalize()
{
}

void MediaMetadata::visit_edges(JS::Cell::Visitor& visitor) {
    Base::visit_edges(visitor);
    visitor.visit(m_chapter_info);
}

void MediaMetadata::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaMetadata);
    Base::initialize(realm);
}

String MediaMetadata::title() const {
    return m_title;
}

void MediaMetadata::set_title(String title) {
    m_title = move(title);
}

String MediaMetadata::artist() const {
    return m_artist;
}

void MediaMetadata::set_artist(String artist) {
    m_artist = move(artist);
}

String MediaMetadata::album() const {
    return m_album;
}

void MediaMetadata::set_album(String album) {
    m_album = move(album);
}

WebIDL::ExceptionOr<void> MediaMetadata::set_artwork(Vector<GC::Root<JS::Object>> const& artwork) {
    m_artwork = TRY(convert_artwork_to_cpp(artwork));

    return {};
}

WebIDL::ExceptionOr<GC::RootVector<JS::Object*>> MediaMetadata::artwork() const {
    return TRY(convert_artwork_to_js(realm(), m_artwork));
}


Vector<GC::Ref<ChapterInformation>> MediaMetadata::chapter_info() const {
    return m_chapter_info;
}

}
