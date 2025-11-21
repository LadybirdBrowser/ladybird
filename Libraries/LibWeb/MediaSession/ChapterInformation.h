#pragma once

#include <LibGC/Root.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::MediaSession {

struct MediaImage {
    Optional<String> src;
    String sizes;
    String type;
};

struct ChapterInformationInit {
    String title;
    double start_time;
    Vector<MediaImage> artwork;
};

class WEB_API ChapterInformation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ChapterInformation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ChapterInformation);

public:
    static WebIDL::ExceptionOr<GC::Ref<ChapterInformation>> create(JS::Realm&, ChapterInformationInit const& init);

    virtual ~ChapterInformation() override;
    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;

    String title() const;
    double start_time() const;
    Vector<MediaImage> artwork() const;

private:
    explicit ChapterInformation(JS::Realm&, String, double, Vector<MediaImage> artwork);

    String m_title;
    double m_start_time;
    Vector<MediaImage> m_artwork;
};

}
