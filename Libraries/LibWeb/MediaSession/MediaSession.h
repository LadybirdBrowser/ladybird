#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Bindings/PlatformObject.h>
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

class WEB_API MediaSession final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaSession, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaSession);

public:
    static GC::Ref<MediaSession> create(HTML::Window& window);

    // TODO: make `handler` Optional
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

private:
    explicit MediaSession(HTML::Window&);

    // https://w3c.github.io/mediasession/#update-metadata-algorithm
    void update_metadata(GC::Ref<MediaMetadata>) const;

    GC::Ref<HTML::Window> m_window;

    GC::Ptr<MediaMetadata> m_metadata;

    Bindings::MediaSessionPlaybackState m_playback_state;

    MediaPositionState m_position_state;
};

}
