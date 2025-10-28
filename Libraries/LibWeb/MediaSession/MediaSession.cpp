#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/MediaSession/MediaSession.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/Bindings/MediaSessionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::MediaSession {

GC_DEFINE_ALLOCATOR(MediaSession);

GC::Ref<MediaSession> MediaSession::create(JS::Realm& realm) {
    return realm.create<MediaSession>(realm);
}

MediaSession::MediaSession(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

MediaSession::~MediaSession() = default;

// https://w3c.github.io/mediasession/#dom-mediasession-setactionhandler
WebIDL::ExceptionOr<void> MediaSession::set_action_handler(Bindings::MediaSessionAction action, MediaSessionActionHandler handler) {
    // https://w3c.github.io/mediasession/#update-action-handler-algorithm
    if (!handler)
        m_supported_media_session_actions.remove(action);
    else
        m_supported_media_session_actions.set(action, handler);

    // TODO: run `media_session_actions_update`
    return {};
}

// https://w3c.github.io/mediasession/#handle-media-session-action
void MediaSession::handle_media_session_action(MediaSessionActionDetails details) {
    // When the user agent is notified by a media session action source named source that a media session action named action has been triggered,
    // the user agent MUST queue a task, using the user interaction task source,
    // to run the following handle media session action steps:
    HTML::queue_a_task(HTML::Task::Source::UserInteraction, nullptr, nullptr, GC::create_function(realm().heap(), [this, details_copy = details] {
        // 1. Let session be source’s target.
        // 2. If session is null, set session to the active media session.
        // 3. If session is null, abort these steps.
        // 4. Let actions be session’s supported media session actions.
        // 5. If actions does not contain the key action, abort these steps.
        if (!m_supported_media_session_actions.contains(details_copy.action))
            return;
        auto& realm = this->realm();
        // 6. Let handler be the MediaSessionActionHandler associated with the key action in actions.
        auto const& handler = m_supported_media_session_actions.get(details_copy.action).value();

        auto details_js = JS::Object::create(realm, nullptr);
        details_js->define_direct_property("action"_utf16, JS::Value(static_cast<i32>(details_copy.action)), JS::default_attributes);
        details_js->define_direct_property("seekOffset"_utf16, JS::Value(details_copy.seekOffset), JS::default_attributes);
        details_js->define_direct_property("seekTime"_utf16, JS::Value(details_copy.seekTime), JS::default_attributes);
        details_js->define_direct_property("fastSeek"_utf16, JS::Value(details_copy.fastSeek), JS::default_attributes);
        details_js->define_direct_property("isActivating"_utf16, JS::Value(details_copy.isActivating), JS::default_attributes);
        details_js->define_direct_property("enterPictureInPictureReason"_utf16, JS::Value(static_cast<i32>(details_copy.enterPictureInPictureReason)), JS::default_attributes);

        // 7. Run handler with the details parameter set to: MediaSessionActionDetails.
        MUST(WebIDL::invoke_callback(*handler, {}, { { details_js } }));
        // 8. Run the activation notification steps in the browsing context associated with session.
        // TODO: Currently not implemented: https://github.com/LadybirdBrowser/ladybird/blob/9312a9f86f63a7f693ee1a9663154a69fbf53462/Libraries/LibWeb/DOM/EventTarget.cpp#L848
    }));
}

bool MediaSession::has_action_handler(Bindings::MediaSessionAction action) const {
    return m_supported_media_session_actions.contains(action);
}

WebIDL::ExceptionOr<void> MediaSession::set_position_state() {
    m_position_state = MediaPositionState { m_position_state.duration, 0, 0 };
    return {};
}

WebIDL::ExceptionOr<void> MediaSession::set_position_state(MediaPositionState state) {
    // The setPositionState(state) method, when invoked MUST perform the following steps:
    // - If state is an empty dictionary, clear the position state and abort these steps.
    if (!state.duration.has_value() && !state.playback_rate.has_value() && !state.position.has_value()) {
        m_position_state = MediaPositionState { 0.0, 0.0, 0.0 };
        return {};
    }

    // - If state’s duration is not present, throw a TypeError.
    // - If state’s duration is negative or NaN, throw a TypeError.
    if (!state.duration.has_value() || state.duration.value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "duration should be positive"sv };
    double duration = state.duration.value();
    // - If state’s position is not present, set it to zero.
    double position = state.position.value_or(0);
    // - If state’s position is negative or greater than duration, throw a TypeError.
    if (position < 0 || position > duration)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "position should be 0 <= position <= duration"sv };
    // - If state’s playbackRate is not present, set it to 1.0.
    double playback_rate = state.playback_rate.value_or(1.0);
    // - If state’s playbackRate is zero, throw a TypeError.
    if (playback_rate == .0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "playback rate should not be zero"sv };

    // TODO: update last position updated time?
    // - Update the position state and last position updated time.
    m_position_state.duration = duration;
    m_position_state.position = position;
    m_position_state.playback_rate = playback_rate;

    return {};
}

GC::Ref<WebIDL::Promise> MediaSession::set_microphone_active(bool active) const {
    // auto& document = HTML::relevant_global_object(m_window->associated_document());
    // TODO: currently no mic access
    dbgln("MediaSession::set_microphone_active({}): no microphone support", active);
    auto promise = WebIDL::create_promise(realm());
    return promise;
}
GC::Ref<WebIDL::Promise> MediaSession::set_camera_active(bool active) const {
    dbgln("MediaSession::set_camera_active({}): no camera support", active);
    auto promise = WebIDL::create_promise(realm());
    return promise;
}

GC::Ref<WebIDL::Promise> MediaSession::set_screenshare_active(bool active) const {
    dbgln("MediaSession::set_screenshare_active({}): no screenshare support", active);
    auto promise = WebIDL::create_promise(realm());
    return promise;
}

GC::Ptr<MediaMetadata> MediaSession::metadata() const {
    return m_metadata;
}

WebIDL::ExceptionOr<void> MediaSession::set_metadata(GC::Ptr<MediaMetadata> value) {
    // 1. If the MediaSession’s metadata is not null, set its media session to null.
    // 2. Set the MediaSession’s metadata to value.
    // 3. If the MediaSession’s metadata is not null, set its media session to the current MediaSession.
    m_metadata = value;

    // TODO: need to implement `update_metadata`
    // 4. In parallel, run the update metadata algorithm.
    return {};
}

Bindings::MediaSessionPlaybackState MediaSession::playback_state() const {
    return m_playback_state;
}


void MediaSession::set_playback_state(Bindings::MediaSessionPlaybackState state) {
    m_playback_state = state;
}

void MediaSession::finalize()
{
}

void MediaSession::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaSession);
    Base::initialize(realm);
}


void MediaSession::visit_edges(JS::Cell::Visitor& visitor) {
    Base::visit_edges(visitor);
    visitor.visit(m_metadata);
}


}
