#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/MediaSession/ChapterInformation.h>

namespace Web::MediaSession {

struct MediaMetadataInit {
    String title;
    String artist;
    String album;
    Vector<MediaImage> artwork;
    Vector<ChapterInformationInit> chapter_info;
};

class WEB_API MediaMetadata final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaMetadata, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaMetadata);

public:
    static WebIDL::ExceptionOr<GC::Ref<MediaMetadata>> construct_impl(JS::Realm&, MediaMetadataInit const& init = {});

    virtual ~MediaMetadata() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    String title() const;
    void set_title(String title);

    String artist() const;
    void set_artist(String artist);

    String album() const;
    void set_album(String album);
    WebIDL::ExceptionOr<GC::RootVector<JS::Object*>> artwork() const;
    WebIDL::ExceptionOr<void> set_artwork(Vector<GC::Root<JS::Object>> const& artwork);

    Vector<GC::Ref<ChapterInformation>> chapter_info() const;

private:
    MediaMetadata(JS::Realm&, String, String, String, Vector<MediaImage>, Vector<GC::Ref<ChapterInformation>>);

    String m_title;
    String m_artist;
    String m_album;
    Vector<MediaImage> m_artwork;
    Vector<GC::Ref<ChapterInformation>> m_chapter_info;
};

}
