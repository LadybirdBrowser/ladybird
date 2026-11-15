/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/MediaSessionCoordinator.h>
#include <string.h>

namespace WebView {

void MediaSessionCoordinator::update(MediaSessionSource source, Optional<Web::MediaSession::MediaSessionState> state, String fallback_title)
{
    if (!state.has_value()) {
        remove(source);
        return;
    }
    for (auto& entry : m_sources) {
        if (entry.source != source)
            continue;
        if (state->activation_counter > entry.state.activation_counter)
            entry.activation_sequence = ++m_activation_sequence;
        entry.state = state.release_value();
        entry.fallback_title = move(fallback_title);
        publish();
        return;
    }
    m_sources.append({ source, state.release_value(), move(fallback_title), ++m_activation_sequence });
    publish();
}

void MediaSessionCoordinator::remove(MediaSessionSource source)
{
    m_sources.remove_all_matching([&](auto const& entry) { return entry.source == source; });
    publish();
}

void MediaSessionCoordinator::remove_view(u64 view_id)
{
    m_sources.remove_all_matching([&](auto const& entry) { return entry.source.view_id == view_id; });
    publish();
}

void MediaSessionCoordinator::set_fallback_title(u64 view_id, String const& title)
{
    for (auto& entry : m_sources) {
        if (entry.source.view_id == view_id)
            entry.fallback_title = title;
    }
    publish();
}

void MediaSessionCoordinator::set_platform_backend(OwnPtr<MediaSessionPlatformBackend> backend)
{
    m_backend = move(backend);
    if (m_backend)
        m_backend->update(m_now_playing_state);
}

bool same_artwork(RefPtr<Gfx::Bitmap const> const& a, RefPtr<Gfx::Bitmap const> const& b)
{
    if (a == b)
        return true;
    if (!a || !b || a->size() != b->size() || a->format() != b->format() || a->alpha_type() != b->alpha_type())
        return false;
    auto row_bytes = Gfx::Bitmap::minimum_pitch(a->width(), a->format());
    for (int y = 0; y < a->height(); ++y) {
        if (memcmp(a->scanline_u8(y), b->scanline_u8(y), row_bytes) != 0)
            return false;
    }
    return true;
}

static bool same_state(Optional<NowPlayingState> const& a, Optional<NowPlayingState> const& b)
{
    if (!a.has_value() || !b.has_value())
        return a.has_value() == b.has_value();
    return a->title == b->title && a->artist == b->artist && a->album == b->album
        && a->playback_state == b->playback_state && a->position == b->position
        && a->supported_actions == b->supported_actions && same_artwork(a->artwork, b->artwork);
}

MediaSessionCoordinator::SourceState const* MediaSessionCoordinator::select_source(Optional<u64> view_id) const
{
    SourceState const* selected = nullptr;
    for (auto const& entry : m_sources) {
        if (view_id.has_value() && entry.source.view_id != *view_id)
            continue;
        if (!selected || entry.activation_sequence > selected->activation_sequence)
            selected = &entry;
    }
    return selected;
}

NowPlayingState MediaSessionCoordinator::now_playing_state_for(SourceState const& source)
{
    auto state = source.state;
    if (source.source.is_private) {
        state.title = "Ladybird"_string;
        state.artist = {};
        state.album = {};
        state.artwork = nullptr;
    } else if (state.title.is_empty()) {
        state.title = source.fallback_title;
    }
    return state;
}

void MediaSessionCoordinator::publish()
{
    auto const* selected = select_source({});

    Optional<NowPlayingState> state;
    if (selected)
        state = now_playing_state_for(*selected);
    auto unchanged = same_state(state, m_now_playing_state);
    m_now_playing_state = move(state);
    if (unchanged)
        return;
    if (m_backend)
        m_backend->update(m_now_playing_state);
}

void MediaSessionCoordinator::send_action(SourceState const& source, Web::Bindings::MediaSessionAction action, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek)
{
    if (!source.state.supported_actions.contains_slow(action) || !on_action)
        return;
    on_action(source.source, action, seek_offset, seek_time, fast_seek);
}

void MediaSessionCoordinator::handle_action(Web::Bindings::MediaSessionAction action, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek)
{
    if (auto const* selected = select_source({}))
        send_action(*selected, action, seek_offset, seek_time, fast_seek);
}

Optional<NowPlayingState> MediaSessionCoordinator::state_for_view(u64 view_id) const
{
    if (auto const* selected = select_source(view_id))
        return now_playing_state_for(*selected);
    return {};
}

void MediaSessionCoordinator::handle_action_for_view(u64 view_id, Web::Bindings::MediaSessionAction action, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek)
{
    if (auto const* selected = select_source(view_id))
        send_action(*selected, action, seek_offset, seek_time, fast_seek);
}

}
