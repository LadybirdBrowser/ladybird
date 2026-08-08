/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Queue.h>
#include <AK/Stream.h>
#include <Compositor/CompositorState.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/UIEvents/MouseButton.h>

struct TestWebContentClient final : public Compositor::CompositorStateWebContentClient {
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const&) override { }
    virtual void request_rendering_update() override { }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool) override { }
};

template<Web::Painting::DisplayListCommand Command>
static void append_display_list_command(ByteBuffer& command_bytes, Command const& command, Optional<Gfx::IntRect> bounding_rect = {})
{
    auto payload = Web::Painting::display_list_object_bytes(command);
    auto record_size = sizeof(Web::Painting::DisplayListCommandHeader) + payload.size();
    auto payload_size = align_up_to(record_size, Web::Painting::DisplayList::command_alignment) - sizeof(Web::Painting::DisplayListCommandHeader);
    Web::Painting::DisplayListCommandHeader header {
        .type = Command::command_type,
        .payload_size = static_cast<u32>(payload_size),
        .context_index = Web::Painting::VISUAL_VIEWPORT_NODE_INDEX,
        .has_bounding_rect = bounding_rect.has_value(),
        .bounding_rect = bounding_rect.value_or({}),
    };

    auto size_before_command = command_bytes.size();
    command_bytes.append(Web::Painting::display_list_object_bytes(header));
    command_bytes.append(payload);
    command_bytes.resize(size_before_command + sizeof(header) + payload_size, ByteBuffer::ZeroFillNewElements::Yes);
}

static NonnullRefPtr<Web::Painting::DisplayList> make_display_list_from_commands(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, ByteBuffer command_bytes, Optional<Gfx::Color> surface_clear_color, Optional<Web::Painting::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(static_cast<u64>(1)));
    MUST(encoder.encode(command_bytes));
    MUST(encoder.encode(visual_context_tree.version()));
    MUST(encoder.encode(surface_clear_color));
    MUST(encoder.encode(async_scrolling_metadata));
    MUST(encoder.encode(HashMap<Web::Painting::VisualContextIndex, Web::Painting::DisplayListResourceId> {}));

    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<NonnullRefPtr<Web::Painting::DisplayList>>());
}

static NonnullRefPtr<Web::Painting::DisplayList> make_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Optional<Gfx::Color> color, Optional<Gfx::Color> surface_clear_color = {})
{
    ByteBuffer command_bytes;
    if (color.has_value()) {
        Web::Painting::FillRect command { { 0, 0, 4, 4 }, *color };
        append_display_list_command(command_bytes, command, command.rect);
    }

    return make_display_list_from_commands(visual_context_tree, move(command_bytes), surface_clear_color, {});
}

static constexpr Web::UniqueNodeID TEST_DOCUMENT_ID { 1 };
static constexpr Web::Painting::VisualContextIndex TEST_SCROLL_NODE_INDEX { 1 };
static Gfx::IntRect const TEST_VIEWPORT_RECT { 0, 0, 100, 200 };
static Gfx::IntRect const TEST_SCROLLBAR_GUTTER_RECT { 90, 0, 10, 200 };
static Gfx::IntRect const TEST_SCROLLBAR_THUMB_RECT { 90, 0, 10, 50 };
static constexpr float TEST_MAX_SCROLL_OFFSET = 600;

// The length of thumb travel per scrolled pixel.
static double const TEST_SCROLLBAR_SCROLL_SIZE = (TEST_SCROLLBAR_GUTTER_RECT.height() - TEST_SCROLLBAR_THUMB_RECT.height()) / TEST_MAX_SCROLL_OFFSET;

static NonnullRefPtr<Web::Painting::DisplayList> make_scrollable_viewport_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, bool with_viewport_scrollbar)
{
    ByteBuffer command_bytes;
    append_display_list_command(command_bytes, Web::Painting::CompositorScrollNode {
                                                   .document_id = TEST_DOCUMENT_ID,
                                                   .scrollable_node_id = TEST_DOCUMENT_ID,
                                                   .scroll_node_index = TEST_SCROLL_NODE_INDEX,
                                                   .parent_scroll_node_index = Web::Painting::VISUAL_VIEWPORT_NODE_INDEX,
                                                   .scrollport_rect = TEST_VIEWPORT_RECT,
                                                   .max_scroll_offset = { 0, TEST_MAX_SCROLL_OFFSET },
                                                   .scroll_node_kind = Web::Painting::CompositorScrollNodeKind::Viewport,
                                                   .is_viewport = true,
                                                   .can_be_wheel_scrolled_vertically = true,
                                               });

    if (with_viewport_scrollbar) {
        append_display_list_command(command_bytes, Web::Painting::CompositorViewportScrollbar {
                                                       .document_id = TEST_DOCUMENT_ID,
                                                       .scroll_node_index = TEST_SCROLL_NODE_INDEX,
                                                       .gutter_rect = TEST_SCROLLBAR_GUTTER_RECT,
                                                       .thumb_rect = TEST_SCROLLBAR_THUMB_RECT,
                                                       .expanded_gutter_rect = TEST_SCROLLBAR_GUTTER_RECT,
                                                       .expanded_thumb_rect = TEST_SCROLLBAR_THUMB_RECT,
                                                       .scroll_size = TEST_SCROLLBAR_SCROLL_SIZE,
                                                       .expanded_scroll_size = TEST_SCROLLBAR_SCROLL_SIZE,
                                                       .max_scroll_offset = TEST_MAX_SCROLL_OFFSET,
                                                       .thumb_color = Gfx::Color::Black,
                                                       .track_color = Gfx::Color::White,
                                                       .vertical = true,
                                                   });
    }

    return make_display_list_from_commands(visual_context_tree, move(command_bytes), {}, Web::Painting::DisplayList::AsyncScrollingMetadata { .viewport_rect = TEST_VIEWPORT_RECT });
}

static Web::MouseEvent make_mouse_event(Web::MouseEvent::Type type, Web::DevicePixels y)
{
    Web::MouseEvent event;
    event.type = type;
    event.position = { TEST_SCROLLBAR_THUMB_RECT.x() + TEST_SCROLLBAR_THUMB_RECT.width() / 2, y };
    event.button = Web::UIEvents::MouseButton::Primary;
    return event;
}

TEST_CASE(dragging_a_viewport_scrollbar_reports_a_user_scroll_gesture_until_it_is_released)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();

    context.viewport_size_updated(TEST_VIEWPORT_RECT.size(), Web::Compositor::WindowResizingInProgress::No);
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree, true), visual_context_tree, {});

    EXPECT(context.handle_mouse_event(make_mouse_event(Web::MouseEvent::Type::MouseDown, TEST_SCROLLBAR_THUMB_RECT.center().y())).accepted);
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT(updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);

    EXPECT(context.handle_mouse_event(make_mouse_event(Web::MouseEvent::Type::MouseMove, TEST_SCROLLBAR_THUMB_RECT.center().y() + 100)).accepted);
    updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.scroll_offsets.is_empty());
    EXPECT(updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);

    // The release always asks for a rendering update, so that the main thread learns of it even when nothing scrolled.
    auto release_result = context.handle_mouse_event(make_mouse_event(Web::MouseEvent::Type::MouseUp, TEST_SCROLLBAR_THUMB_RECT.center().y() + 100));
    EXPECT(release_result.accepted);
    EXPECT(release_result.should_request_rendering_update);
    updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(updates.user_scroll_gesture_ended);

    // The release is reported once.
    updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(!updates.user_scroll_gesture_ended);
}

TEST_CASE(losing_the_scrollbar_a_drag_holds_ends_its_user_scroll_gesture)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();

    context.viewport_size_updated(TEST_VIEWPORT_RECT.size(), Web::Compositor::WindowResizingInProgress::No);
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree, true), visual_context_tree, {});

    EXPECT(context.handle_mouse_event(make_mouse_event(Web::MouseEvent::Type::MouseDown, TEST_SCROLLBAR_THUMB_RECT.center().y())).accepted);
    EXPECT(context.take_pending_async_scroll_updates().user_scroll_gesture_in_progress);

    // The drag cannot outlive the scrollbar it holds.
    context.install_display_list_update(make_scrollable_viewport_display_list(visual_context_tree, false), visual_context_tree, {});
    auto updates = context.take_pending_async_scroll_updates();
    EXPECT(!updates.user_scroll_gesture_in_progress);
    EXPECT(updates.user_scroll_gesture_ended);
}

TEST_CASE(rasterization_clears_damaged_pixels_to_the_canvas_color_in_presentation_backing_stores)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, false };
    Web::Painting::DisplayListPlayerSkia display_list_player { RefPtr<Gfx::SkiaBackendContext> {} };
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();
    auto viewport_rect = Gfx::IntRect { 0, 0, 4, 4 };

    context.viewport_size_updated(viewport_rect.size(), Web::Compositor::WindowResizingInProgress::No);
    auto publication = context.resize_backing_stores_if_needed({}, Compositor::BackingStoreManager::GpuSharing::Disallowed);
    VERIFY(publication.has_value());

    auto paint_frame = [&](NonnullRefPtr<Web::Painting::DisplayList> display_list) {
        context.install_display_list_update(move(display_list), visual_context_tree, {});
        context.queue_present_frame({ viewport_rect, viewport_rect });
        EXPECT(context.present_synchronously(display_list_player, nullptr));
    };

    // Paint both backing stores red before reusing the first one for a frame with no commands.
    paint_frame(make_display_list(visual_context_tree, Gfx::Color::Red));
    EXPECT(context.acknowledge_presented_bitmap(publication->bitmap_ids[0]));
    paint_frame(make_display_list(visual_context_tree, Gfx::Color::Red));
    paint_frame(make_display_list(visual_context_tree, {}, Gfx::Color::Green));

    auto bitmap = context.latest_rendered_surface()->snapshot_bitmap();
    EXPECT_EQ(bitmap->get_pixel(0, 0), Gfx::Color::Green);

    paint_frame(make_display_list(visual_context_tree, {}));
    bitmap = context.latest_rendered_surface()->snapshot_bitmap();
    EXPECT_EQ(bitmap->get_pixel(0, 0), Gfx::Color::Transparent);
}
