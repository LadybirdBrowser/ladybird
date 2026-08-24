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

struct TestWebContentClient final : public Compositor::CompositorStateWebContentClient {
    virtual void dispatch_mouse_event_to_web_content(u64, Web::MouseEvent const&) override { }
    virtual void request_rendering_update() override { }
    virtual void create_video_edge(Media::VideoSinkHandle) override { }
    virtual void release_video_edge(Media::VideoSinkHandle) override { }
};

template<Web::Painting::DisplayListCommand Command>
static void append_display_list_command(ByteBuffer& command_bytes, Command const& command, bool context_geometry_only, Optional<Gfx::IntRect> bounding_rect = {})
{
    auto payload = Web::Painting::display_list_object_bytes(command);
    auto record_size = sizeof(Web::Painting::DisplayListCommandHeader) + payload.size();
    auto payload_size = align_up_to(record_size, Web::Painting::DisplayList::command_alignment) - sizeof(Web::Painting::DisplayListCommandHeader);
    Web::Painting::DisplayListCommandHeader header {
        .command_type = Command::command_type,
        .payload_size = static_cast<u32>(payload_size),
        .context_index = Web::Painting::VISUAL_VIEWPORT_NODE_INDEX,
        .context_geometry_only = context_geometry_only,
        .has_bounding_rect = bounding_rect.has_value(),
        .is_clip = false,
        .bounding_rect = bounding_rect.value_or({}),
    };
    command_bytes.append(Web::Painting::display_list_object_bytes(header));
    command_bytes.append(payload);
    // Pad the record so the next command begins at an aligned offset.
    command_bytes.resize(align_up_to(command_bytes.size(), Web::Painting::DisplayList::command_alignment), ByteBuffer::ZeroFillNewElements::Yes);
}

static NonnullRefPtr<Web::Painting::DisplayList> decode_display_list(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, ByteBuffer command_bytes, Optional<Gfx::Color> surface_clear_color = {}, Optional<Web::Painting::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata = {})
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
        auto command = Web::Painting::FillRect { { 0, 0, 4, 4 }, *color };
        append_display_list_command(command_bytes, command, false, command.rect);
    }
    return decode_display_list(visual_context_tree, move(command_bytes), surface_clear_color);
}

static NonnullRefPtr<Web::Painting::DisplayList> make_display_list_with_viewport_scrollbar(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree)
{
    ByteBuffer command_bytes;
    Web::UniqueNodeID document_id { 1 };
    Web::Painting::VisualContextIndex scroll_node_index { 1 };

    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorScrollNode {
            .document_id = document_id,
            .scrollable_node_id = Web::UniqueNodeID { 2 },
            .scroll_node_index = scroll_node_index,
            .parent_scroll_node_index = Web::Painting::VISUAL_VIEWPORT_NODE_INDEX,
            .scrollport_rect = { 0, 0, 100, 100 },
            .min_scroll_offset = { 0, 0 },
            .max_scroll_offset = { 0, 100 },
            .scroll_node_kind = Web::Painting::CompositorScrollNodeKind::Viewport,
            .pseudo_element_type = 0,
            .is_viewport = true,
            .can_be_wheel_scrolled_horizontally = false,
            .can_be_wheel_scrolled_vertically = true,
        },
        true);

    append_display_list_command(
        command_bytes,
        Web::Painting::CompositorViewportScrollbar {
            .document_id = document_id,
            .scroll_node_index = scroll_node_index,
            .gutter_rect = { 96, 0, 4, 100 },
            .thumb_rect = { 98, 0, 2, 20 },
            .expanded_gutter_rect = { 92, 0, 8, 100 },
            .expanded_thumb_rect = { 94, 0, 6, 20 },
            .scroll_size = 0.8,
            .expanded_scroll_size = 0.8,
            .min_scroll_offset = 0,
            .max_scroll_offset = 100,
            .thumb_color = Gfx::Color::Black,
            .track_color = Gfx::Color::Transparent,
            .vertical = true,
        },
        true);

    return decode_display_list(visual_context_tree, move(command_bytes), {},
        Web::Painting::DisplayList::AsyncScrollingMetadata {
            .viewport_rect = { 0, 0, 100, 100 },
        });
}

static Web::MouseEvent mouse_event(Web::MouseEvent::Type type, int x, int y, Web::UIEvents::MouseButton button = Web::UIEvents::MouseButton::None)
{
    return {
        .type = type,
        .position = { Web::DevicePixels { x }, Web::DevicePixels { y } },
        .screen_position = {},
        .button = button,
        .buttons = Web::UIEvents::MouseButton::None,
        .modifiers = Web::UIEvents::KeyModifier::Mod_None,
        .wheel_delta_x = 0,
        .wheel_delta_y = 0,
        .click_count = 0,
        .browser_data = nullptr,
        .async_scroll_performed_default_action = false,
    };
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

TEST_CASE(oversized_backing_stores_are_rejected)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed({ 40'000, 40'000 }, Web::Compositor::WindowResizingInProgress::No);
    VERIFY(allocation.has_value());

    auto publication = manager.allocate_backing_stores(*allocation, {}, true, Compositor::BackingStoreManager::GpuSharing::Disallowed);

    EXPECT(!publication.has_value());
    EXPECT(!manager.is_valid());
}

TEST_CASE(viewport_scrollbar_collapses_when_drag_is_released_away_from_scrollbar)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();
    context.install_display_list_update(make_display_list_with_viewport_scrollbar(visual_context_tree), visual_context_tree, {});

    auto hover_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 10));
    EXPECT(hover_result.accepted);
    EXPECT(hover_result.frame_to_present.has_value());

    auto press_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary));
    EXPECT(press_result.accepted);

    auto drag_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 50, 50));
    EXPECT(drag_result.accepted);

    auto release_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 50, 50, Web::UIEvents::MouseButton::Primary));
    EXPECT(release_result.accepted);

    // Releasing the drag should have already cleared the hover state, so the next move must not trigger a delayed repaint.
    auto next_move_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 50, 50));
    EXPECT(!next_move_result.accepted);
    EXPECT(!next_move_result.frame_to_present.has_value());
}

TEST_CASE(viewport_scrollbar_drag_ignores_non_primary_mouse_up)
{
    TestWebContentClient client;
    Web::Painting::CanvasSurfaceRegistry canvas_surface_registry;
    Compositor::ContextState context { 0, client, canvas_surface_registry, true };
    auto visual_context_tree = Web::Painting::AccumulatedVisualContextTree::create();
    context.install_display_list_update(make_display_list_with_viewport_scrollbar(visual_context_tree), visual_context_tree, {});

    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 98, 10)).accepted);
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseDown, 98, 10, Web::UIEvents::MouseButton::Primary)).accepted);

    auto secondary_release_result = context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 50, 50, Web::UIEvents::MouseButton::Secondary));
    EXPECT(!secondary_release_result.accepted);

    // The primary-button drag remains captured after another button is released.
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseMove, 50, 60)).accepted);
    EXPECT(context.handle_mouse_event(mouse_event(Web::MouseEvent::Type::MouseUp, 50, 60, Web::UIEvents::MouseButton::Primary)).accepted);
}
