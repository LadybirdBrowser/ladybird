/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebView/MediaSessionCoordinator.h>

using Web::Bindings::MediaSessionAction;
using Web::Bindings::MediaSessionPlaybackState;
using WebView::MediaSessionSource;
using WebView::NowPlayingState;

static constexpr MediaSessionSource source_a { 1, Web::PageId { 1 } };
static constexpr MediaSessionSource source_b { 2, Web::PageId { 2 } };

class RecordingBackend final : public WebView::MediaSessionPlatformBackend {
public:
    explicit RecordingBackend(Vector<Optional<NowPlayingState>>& updates)
        : m_updates(updates)
    {
    }

    virtual void update(Optional<NowPlayingState> const& state) override { m_updates.append(state); }

private:
    Vector<Optional<NowPlayingState>>& m_updates;
};

struct CoordinatorFixture {
    Vector<Optional<NowPlayingState>> updates;
    WebView::MediaSessionCoordinator coordinator;

    CoordinatorFixture()
    {
        coordinator.set_platform_backend(make<RecordingBackend>(updates));
        updates.clear();
    }
};

static NowPlayingState playing_state(String title = {})
{
    NowPlayingState state;
    state.activation_counter = 1;
    state.title = move(title);
    state.playback_state = MediaSessionPlaybackState::Playing;
    state.supported_actions = { MediaSessionAction::Play, MediaSessionAction::Pause, MediaSessionAction::Seekto };
    return state;
}

static void fill_bitmap(Gfx::Bitmap& bitmap, Gfx::Color color)
{
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x)
            bitmap.set_pixel(x, y, color);
    }
}

TEST_CASE(media_session_selection_follows_activation_and_retains_paused_sources)
{
    CoordinatorFixture fixture;
    auto& coordinator = fixture.coordinator;
    auto a = playing_state("A"_string);
    auto b = playing_state("B"_string);
    coordinator.update(source_a, a, {});
    coordinator.update(source_b, b, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);

    a.artist = "updated artist"_string;
    coordinator.update(source_a, a, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
    EXPECT_EQ(fixture.updates.size(), 2u);

    b.playback_state = MediaSessionPlaybackState::Paused;
    coordinator.update(source_b, b, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
    EXPECT(coordinator.now_playing_state()->playback_state == MediaSessionPlaybackState::Paused);
    coordinator.remove(source_b);
    EXPECT_EQ(coordinator.now_playing_state()->title, "A"sv);
    EXPECT_EQ(coordinator.now_playing_state()->artist, "updated artist"sv);

    coordinator.update(source_b, b, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
    a.playback_state = MediaSessionPlaybackState::Paused;
    coordinator.update(source_a, a, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
    a.playback_state = MediaSessionPlaybackState::Playing;
    ++a.activation_counter;
    coordinator.update(source_a, a, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "A"sv);
    coordinator.update(source_a, {}, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
    coordinator.remove(source_b);
    EXPECT(!coordinator.now_playing_state().has_value());
    EXPECT(!fixture.updates.last().has_value());
}

TEST_CASE(media_session_playback_state_does_not_change_routing)
{
    CoordinatorFixture fixture;
    auto& coordinator = fixture.coordinator;
    auto a = playing_state("A"_string);
    a.activation_counter = 100;
    a.playback_state = MediaSessionPlaybackState::Paused;
    auto b = playing_state("B"_string);
    coordinator.update(source_a, a, {});
    coordinator.update(source_b, b, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);

    MediaSessionSource expected_source = source_b;
    size_t actions = 0;
    coordinator.on_action = [&](MediaSessionSource source, auto, auto, auto, auto) {
        ++actions;
        EXPECT(source == expected_source);
    };
    a.playback_state = MediaSessionPlaybackState::Playing;
    coordinator.update(source_a, a, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
    EXPECT_EQ(fixture.updates.size(), 2u);
    coordinator.handle_action(MediaSessionAction::Pause);
    EXPECT_EQ(actions, 1u);

    ++a.activation_counter;
    coordinator.update(source_a, a, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "A"sv);
    EXPECT_EQ(coordinator.now_playing_state()->activation_counter, 101u);
    expected_source = source_a;
    coordinator.handle_action(MediaSessionAction::Pause);
    EXPECT_EQ(actions, 2u);

    b.playback_state = MediaSessionPlaybackState::Paused;
    coordinator.update(source_b, b, {});
    ++b.activation_counter;
    coordinator.update(source_b, b, {});
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
    EXPECT(coordinator.now_playing_state()->playback_state == MediaSessionPlaybackState::Paused);
    coordinator.remove(source_b);
    EXPECT_EQ(coordinator.now_playing_state()->title, "A"sv);
}

TEST_CASE(media_session_title_fallback_and_duplicate_publication)
{
    CoordinatorFixture fixture;
    auto& coordinator = fixture.coordinator;
    auto state = playing_state();
    state.artist = "artist"_string;
    state.album = "album"_string;
    coordinator.update(source_a, state, "tab title"_string);
    EXPECT_EQ(coordinator.now_playing_state()->title, "tab title"sv);
    EXPECT_EQ(coordinator.now_playing_state()->artist, "artist"sv);
    EXPECT_EQ(coordinator.now_playing_state()->album, "album"sv);
    coordinator.update(source_a, state, "tab title"_string);
    EXPECT_EQ(fixture.updates.size(), 1u);
    coordinator.set_fallback_title(source_a.view_id, "changed title"_string);
    EXPECT_EQ(coordinator.now_playing_state()->title, "changed title"sv);
    coordinator.set_fallback_title(source_a.view_id, "changed title"_string);
    EXPECT_EQ(fixture.updates.size(), 2u);
    state.title = "metadata title"_string;
    coordinator.update(source_a, state, "changed title"_string);
    coordinator.set_fallback_title(source_a.view_id, "another title"_string);
    EXPECT_EQ(coordinator.now_playing_state()->title, "metadata title"sv);
    EXPECT_EQ(fixture.updates.size(), 3u);
}

TEST_CASE(private_media_session_hides_metadata_and_preserves_controls)
{
    CoordinatorFixture fixture;
    MediaSessionSource private_source { 1, Web::PageId { 1 }, true };
    auto state = playing_state("private title"_string);
    state.artist = "private artist"_string;
    state.album = "private album"_string;
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 2, 2 }));
    fill_bitmap(*bitmap, Gfx::Color::Red);
    state.artwork = bitmap;
    state.position = Web::MediaSession::MediaSessionPositionState { 12.0, 1.5, 4.0, MonotonicTime::now() };
    fixture.coordinator.update(private_source, state, "private tab title"_string);

    auto const& published = *fixture.updates.last();
    EXPECT_EQ(published.title, "Ladybird"sv);
    EXPECT(published.artist.is_empty());
    EXPECT(published.album.is_empty());
    EXPECT(!published.artwork);
    EXPECT(published.playback_state == state.playback_state);
    EXPECT(published.position == state.position);
    EXPECT(published.supported_actions == state.supported_actions);
    EXPECT_EQ(published.activation_counter, state.activation_counter);

    size_t actions = 0;
    fixture.coordinator.on_action = [&](MediaSessionSource source, auto, auto, auto, auto) {
        EXPECT(source == private_source);
        ++actions;
    };
    for (auto action : state.supported_actions)
        fixture.coordinator.handle_action(action);
    EXPECT_EQ(actions, state.supported_actions.size());

    fixture.coordinator.update(source_b, state, "public tab title"_string);
    EXPECT(*fixture.updates.last() == state);
    fixture.coordinator.remove(source_b);
    EXPECT_EQ(fixture.updates.last()->title, "Ladybird"sv);
    fixture.coordinator.remove(private_source);
    EXPECT(!fixture.updates.last().has_value());
}

TEST_CASE(private_media_session_does_not_publish_fallback_title)
{
    CoordinatorFixture fixture;
    MediaSessionSource private_source { 1, Web::PageId { 1 }, true };
    fixture.coordinator.update(private_source, playing_state(), "private tab title"_string);
    EXPECT_EQ(fixture.updates.last()->title, "Ladybird"sv);
    fixture.coordinator.set_fallback_title(private_source.view_id, "changed private title"_string);
    EXPECT_EQ(fixture.updates.size(), 1u);
    EXPECT_EQ(fixture.coordinator.now_playing_state()->title, "Ladybird"sv);
}

TEST_CASE(media_session_actions_follow_the_selected_source_and_supported_actions)
{
    WebView::MediaSessionCoordinator coordinator;
    size_t actions = 0;
    coordinator.on_action = [&](MediaSessionSource source, MediaSessionAction action, Optional<double> offset, Optional<double> time, Optional<bool> fast) {
        ++actions;
        EXPECT(source == source_b);
        EXPECT(action == MediaSessionAction::Seekto);
        EXPECT(offset == 4.0);
        EXPECT(time == 7.0);
        EXPECT(fast == true);
    };
    coordinator.handle_action(MediaSessionAction::Seekto);
    EXPECT_EQ(actions, 0u);
    coordinator.update(source_a, playing_state("A"_string), {});
    coordinator.update(source_b, playing_state("B"_string), {});
    coordinator.handle_action(MediaSessionAction::Nexttrack);
    EXPECT_EQ(actions, 0u);
    coordinator.handle_action(MediaSessionAction::Seekto, 4.0, 7.0, true);
    EXPECT_EQ(actions, 1u);
    coordinator.remove(source_a);
    coordinator.remove(source_b);
    coordinator.handle_action(MediaSessionAction::Seekto);
    EXPECT_EQ(actions, 1u);
}

TEST_CASE(media_session_state_is_available_without_a_backend_and_on_backend_installation)
{
    Vector<Optional<NowPlayingState>> updates;
    WebView::MediaSessionCoordinator coordinator;
    coordinator.update(source_a, playing_state(), "fallback"_string);
    EXPECT_EQ(coordinator.now_playing_state()->title, "fallback"sv);
    coordinator.set_platform_backend(make<RecordingBackend>(updates));
    EXPECT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates.last()->title, "fallback"sv);
    coordinator.set_platform_backend(nullptr);
    coordinator.remove(source_a);
    EXPECT(!coordinator.now_playing_state().has_value());
    EXPECT_EQ(updates.size(), 1u);
}

TEST_CASE(media_session_pages_share_their_view_title_and_are_removed_with_the_view)
{
    CoordinatorFixture fixture;
    auto& coordinator = fixture.coordinator;
    MediaSessionSource other_page { source_a.view_id, Web::PageId { 3 } };
    coordinator.update(source_b, playing_state("B"_string), {});
    coordinator.update(source_a, playing_state(), "tab title"_string);
    coordinator.update(other_page, playing_state(), "tab title"_string);
    coordinator.set_fallback_title(source_a.view_id, "changed title"_string);
    EXPECT_EQ(coordinator.now_playing_state()->title, "changed title"sv);
    coordinator.remove(other_page);
    EXPECT_EQ(coordinator.now_playing_state()->title, "changed title"sv);
    coordinator.remove_view(source_a.view_id);
    EXPECT_EQ(coordinator.now_playing_state()->title, "B"sv);
}

TEST_CASE(media_session_identical_artwork_does_not_publish_again)
{
    CoordinatorFixture fixture;
    auto state = playing_state();
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 2, 2 }));
    fill_bitmap(*bitmap, Gfx::Color::Red);
    state.artwork = bitmap;
    fixture.coordinator.update(source_a, state, {});
    auto copy = MUST(bitmap->clone());
    state.artwork = copy;
    fixture.coordinator.update(source_a, state, {});
    EXPECT_EQ(fixture.updates.size(), 1u);
    auto changed = MUST(bitmap->clone());
    fill_bitmap(*changed, Gfx::Color::Blue);
    state.artwork = changed;
    fixture.coordinator.update(source_a, state, {});
    EXPECT_EQ(fixture.updates.size(), 2u);
}

TEST_CASE(media_session_artwork_comparison_uses_size_format_and_pixels)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 2, 2 }));
    fill_bitmap(*bitmap, Gfx::Color::Red);
    auto copy = MUST(bitmap->clone());
    EXPECT(WebView::same_artwork(bitmap, copy));
    copy->set_pixel(1, 1, Gfx::Color::Blue);
    EXPECT(!WebView::same_artwork(bitmap, copy));

    auto other_size = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 2, 3 }));
    fill_bitmap(*other_size, Gfx::Color::Red);
    EXPECT(!WebView::same_artwork(bitmap, other_size));
    auto other_format = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { 2, 2 }));
    fill_bitmap(*other_format, Gfx::Color::Red);
    EXPECT(!WebView::same_artwork(bitmap, other_format));
    EXPECT(!WebView::same_artwork(bitmap, nullptr));
    EXPECT(!WebView::same_artwork(nullptr, bitmap));
    EXPECT(WebView::same_artwork(nullptr, nullptr));
}

TEST_CASE(media_session_artwork_comparison_uses_alpha_type)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }));
    auto other = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Unpremultiplied, { 2, 2 }));
    fill_bitmap(*bitmap, Gfx::Color::Red);
    fill_bitmap(*other, Gfx::Color::Red);
    EXPECT(!WebView::same_artwork(bitmap, other));
}

TEST_CASE(media_session_ipc_preserves_metadata_artwork_and_position_time)
{
    auto state = playing_state("track"_string);
    state.activation_counter = 0x123456789abcdef0;
    state.artist = "artist"_string;
    state.album = "album"_string;
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 2, 3 }));
    fill_bitmap(*bitmap, Gfx::Color::Red);
    state.artwork = bitmap;
    state.position = Web::MediaSession::MediaSessionPositionState {
        12.0, 1.5, 4.0, MonotonicTime::now() - AK::Duration::from_seconds(3)
    };
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(state));
    auto bytes = buffer.take_data();
    FixedMemoryStream stream { bytes.span() };
    Queue<IPC::Attachment> attachments;
    for (auto& attachment : buffer.take_attachments())
        attachments.enqueue(move(attachment));
    IPC::Decoder decoder { stream, attachments };
    auto decoded = MUST(decoder.decode<NowPlayingState>());
    EXPECT_EQ(decoded.activation_counter, state.activation_counter);
    EXPECT_EQ(decoded.title, state.title);
    EXPECT_EQ(decoded.artist, state.artist);
    EXPECT_EQ(decoded.album, state.album);
    EXPECT(decoded.playback_state == state.playback_state);
    EXPECT(decoded.supported_actions == state.supported_actions);
    EXPECT(decoded.artwork);
    EXPECT_EQ(decoded.artwork->width(), 2);
    EXPECT_EQ(decoded.artwork->height(), 3);
    EXPECT_EQ(decoded.artwork->get_pixel(1, 2), Gfx::Color::Red);
    EXPECT(decoded.position.has_value());
    EXPECT_EQ(decoded.position->duration, 12.0);
    EXPECT_EQ(decoded.position->playback_rate, 1.5);
    EXPECT_EQ(decoded.position->position, 4.0);
    EXPECT(decoded.position->last_position_updated_time == state.position->last_position_updated_time);
}

static ErrorOr<NowPlayingState> round_trip_state(NowPlayingState const& state)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(state));
    auto bytes = buffer.take_data();
    FixedMemoryStream stream { bytes.span() };
    Queue<IPC::Attachment> attachments;
    for (auto& attachment : buffer.take_attachments())
        attachments.enqueue(move(attachment));
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<NowPlayingState>();
}

TEST_CASE(media_session_ipc_preserves_state_without_artwork_or_position)
{
    auto state = playing_state("track"_string);
    state.playback_state = MediaSessionPlaybackState::Paused;
    state.supported_actions.clear();
    auto decoded = MUST(round_trip_state(state));
    EXPECT(decoded == state);
}

TEST_CASE(media_session_ipc_limits_artwork_dimensions)
{
    struct TestCase {
        Gfx::IntSize size;
        bool accepted;
    };
    TestCase cases[] = {
        { { 512, 512 }, true },
        { { 513, 1 }, false },
        { { 1, 513 }, false },
        { { 513, 513 }, false },
    };
    for (auto const& test : cases) {
        auto state = playing_state();
        state.artwork = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, test.size));
        auto decoded = round_trip_state(state);
        EXPECT_EQ(!decoded.is_error(), test.accepted);
        if (!decoded.is_error()) {
            EXPECT(decoded.value().artwork);
            EXPECT_EQ(decoded.value().artwork->size(), test.size);
        }
    }
}

TEST_CASE(media_session_source_can_change_without_changing_the_published_state)
{
    CoordinatorFixture fixture;
    auto state = playing_state("same track"_string);
    fixture.coordinator.update(source_a, state, {});
    fixture.coordinator.update(source_b, state, {});
    EXPECT_EQ(fixture.updates.size(), 1u);
    MediaSessionSource expected_source = source_b;
    size_t actions = 0;
    fixture.coordinator.on_action = [&](MediaSessionSource source, auto, auto, auto, auto) {
        ++actions;
        EXPECT(source == expected_source);
    };
    fixture.coordinator.handle_action(MediaSessionAction::Pause);
    EXPECT_EQ(actions, 1u);

    ++state.activation_counter;
    fixture.coordinator.update(source_a, state, {});
    EXPECT_EQ(fixture.updates.size(), 1u);
    EXPECT_EQ(fixture.coordinator.now_playing_state()->activation_counter, state.activation_counter);
    expected_source = source_a;
    fixture.coordinator.handle_action(MediaSessionAction::Pause);
    EXPECT_EQ(actions, 2u);
}

TEST_CASE(media_session_position_and_supported_action_changes_are_published)
{
    CoordinatorFixture fixture;
    auto state = playing_state();
    auto updated_at = MonotonicTime::now();
    state.position = Web::MediaSession::MediaSessionPositionState { 12.0, 1.0, 4.0, updated_at };
    fixture.coordinator.update(source_a, state, {});
    fixture.coordinator.update(source_a, state, {});
    EXPECT_EQ(fixture.updates.size(), 1u);
    state.position->position = 7.0;
    fixture.coordinator.update(source_a, state, {});
    EXPECT_EQ(fixture.updates.size(), 2u);
    EXPECT_EQ(fixture.updates.last()->position->position, 7.0);
    state.supported_actions = { MediaSessionAction::Play };
    fixture.coordinator.update(source_a, state, {});
    EXPECT_EQ(fixture.updates.size(), 3u);
    size_t actions = 0;
    fixture.coordinator.on_action = [&](auto, auto, auto, auto, auto) { ++actions; };
    fixture.coordinator.handle_action(MediaSessionAction::Pause);
    EXPECT_EQ(actions, 0u);
    fixture.coordinator.handle_action(MediaSessionAction::Play);
    EXPECT_EQ(actions, 1u);
}
