#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/MediaSession/MediaMetadata.h>
#include <LibWeb/Bindings/MediaSessionPrototype.h>

namespace Web::MediaSession {

#define MediaSessionActionHandler GC::Ptr<WebIDL::CallbackType>

struct MediaSessionActionDetails {
    Bindings::MediaSessionAction action;
    double seekOffset;
    double seekTime;
    bool fastSeek;
    bool isActivating;
    Bindings::MediaSessionEnterPictureInPictureReason enterPictureInPictureReason;
};

struct MediaPositionState {
    Optional<double> duration;
    Optional<double> playback_rate;
    Optional<double> position;
};

class WEB_API MediaSession final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaSession, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaSession);

public:
    static GC::Ref<MediaSession> create(JS::Realm&);

    WebIDL::ExceptionOr<void> set_action_handler(Bindings::MediaSessionAction action, MediaSessionActionHandler handler);

    WebIDL::ExceptionOr<void> set_position_state();
    WebIDL::ExceptionOr<void> set_position_state(MediaPositionState state = {});

    GC::Ref<WebIDL::Promise> set_microphone_active(bool active) const;

    GC::Ref<WebIDL::Promise> set_camera_active(bool active) const;

    GC::Ref<WebIDL::Promise> set_screenshare_active(bool active) const;

    virtual ~MediaSession() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    GC::Ptr<MediaMetadata> metadata() const;
    WebIDL::ExceptionOr<void> set_metadata(GC::Ptr<MediaMetadata>);

    Bindings::MediaSessionPlaybackState playback_state() const;
    void set_playback_state(Bindings::MediaSessionPlaybackState);

    void handle_action(MediaSessionActionDetails);

    bool has_action_handler(Bindings::MediaSessionAction) const;

private:
    explicit MediaSession(JS::Realm&);

    // https://w3c.github.io/mediasession/#update-metadata-algorithm
    void update_metadata(GC::Ref<MediaMetadata>) const;

    GC::Ptr<MediaMetadata> m_metadata;

    Bindings::MediaSessionPlaybackState m_playback_state;

    MediaPositionState m_position_state;

    // https://w3c.github.io/mediasession/#supported-media-session-actions
    HashMap<Bindings::MediaSessionAction, MediaSessionActionHandler> m_action_handlers;
};

}
