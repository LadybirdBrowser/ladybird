#include <LibWeb/MediaSession/ChapterInformation.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/Bindings/ChapterInformationPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/MediaSession/Utils.h>

namespace Web::MediaSession {

GC_DEFINE_ALLOCATOR(ChapterInformation);

WebIDL::ExceptionOr<GC::Ref<ChapterInformation>> ChapterInformation::create(JS::Realm& realm, ChapterInformationInit const& init) {
    // 1. Let chapterInfo be a new ChapterInformation object.
    // 2. Set chapterInfo’s title to init’s title.
    // 3. Set chapterInfo’s startTime to init’s startTime. If the startTime is negative or greater than duration, throw a TypeError.
    if (init.start_time < 0) /* FIXME: || init.startTime > duration) */
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "startTime must be 0 <= startTime <= duration"sv };
    // 4. Let artwork be the result of running the convert artwork algorithm with init’s artwork as input.
    auto artwork = TRY(convert_artwork(init.artwork));
    // 5. Set chapterInfo’s artwork images to the result of creating a frozen array from artwork.
    // 6. Return chapterInfo.
    return realm.create<ChapterInformation>(realm, init.title, init.start_time, artwork);
}

ChapterInformation::ChapterInformation(JS::Realm& realm, String title, double start_time, Vector<MediaImage> artwork)
    : PlatformObject(realm)
    , m_title(move(title))
    , m_start_time(start_time)
    , m_artwork(move(artwork))
{
}

ChapterInformation::~ChapterInformation() = default;

void ChapterInformation::finalize()
{
}

void ChapterInformation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ChapterInformation);
    Base::initialize(realm);
}


String ChapterInformation::title() const {
    return m_title;
}

double ChapterInformation::start_time() const {
    return m_start_time;
}

// GC::RootVector<JS::Object> ChapterInformation::artwork() const {
Vector<MediaImage> ChapterInformation::artwork() const {
    return m_artwork;
}

}
