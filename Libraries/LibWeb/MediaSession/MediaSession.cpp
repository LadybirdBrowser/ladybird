#include <LibWeb/MediaSession/MediaSession.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/Bindings/MediaSessionPrototype.h>

namespace Web::MediaSession {

GC_DEFINE_ALLOCATOR(MediaSession);

// void MediaSession::update_metadata(GC::Ref<MediaMetadata> metadata) const {
//     // const auto& navigator = as<HTML::Navigator>(*this);
// }

GC::Ref<MediaSession> MediaSession::create(HTML::Window& window) {
    return window.realm().create<MediaSession>(window);
}

MediaSession::MediaSession(HTML::Window& window)
    : DOM::EventTarget(window.realm())
    , m_window(window)
{
}

MediaSession::~MediaSession() = default;

WebIDL::ExceptionOr<void> MediaSession::set_action_handler(Bindings::MediaSessionAction action, MediaSessionActionHandler handler) {
    if (!handler)
        m_action_handlers.remove(action);
    else
        m_action_handlers.set(action, handler);
    return {};
}

bool MediaSession::handle_action(Bindings::MediaSessionAction action) {
    if (!m_action_handlers.contains(action))
        return false;

    dbgln("MediaSession::handle_action is unimplemented");

    return true;
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
    if (!state.duration.has_value() || state.duration.release_value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "duration should be positive"sv };
    double duration = state.duration.release_value();
    // - If state’s position is not present, set it to zero.
    double position = state.position.value_or(0);
    // - If state’s position is negative or greater than duration, throw a TypeError.
    if (position < 0 || position > duration)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "position should be 0 <= position <= duration"sv };
    // - If state’s playbackRate is not present, set it to 1.0.
    double playback_rate = state.playback_rate.value_or(1.0);
    // - If state’s playbackRate is zero, throw a TypeError.
    if (playback_rate == .0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "playback rate should be positive"sv };

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

    // TODO
    // 4. In parallel, run the update metadata algorithm.
    // Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), update_metadata(metadata)));
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
    visitor.visit(m_window);
    visitor.visit(m_metadata);
}


}
