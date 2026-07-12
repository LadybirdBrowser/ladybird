/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListDamage.h>

using namespace Web::Painting;

template<DisplayListCommand Command>
static ByteBuffer command_bytes(Command const& command, Optional<Gfx::IntRect> bounding_rect = {}, VisualContextIndex context_index = VISUAL_VIEWPORT_NODE_INDEX)
{
    auto payload = display_list_object_bytes(command);
    auto record_size = sizeof(DisplayListCommandHeader) + payload.size();
    constexpr size_t command_alignment = 16;
    auto payload_size = align_up_to(record_size, command_alignment) - sizeof(DisplayListCommandHeader);
    DisplayListCommandHeader header {
        .type = Command::command_type,
        .payload_size = static_cast<u32>(payload_size),
        .context_index = context_index,
        .has_bounding_rect = bounding_rect.has_value(),
        .bounding_rect = bounding_rect.value_or({}),
    };
    ByteBuffer bytes;
    bytes.append(display_list_object_bytes(header));
    bytes.append(payload);
    bytes.resize(sizeof(header) + payload_size, ByteBuffer::ZeroFillNewElements::Yes);
    return bytes;
}

static ByteBuffer fill_command_bytes(Gfx::IntRect rect, Gfx::Color color)
{
    return command_bytes(FillRect { rect, color }, rect);
}

static ByteBuffer glyph_run_command_bytes(size_t inline_padding)
{
    DisplayListGlyph glyph { .position = { 1, 2 }, .glyph_id = 3 };
    DrawGlyphRun command {
        .font_id = FontResourceId { 1 },
        .glyphs = { static_cast<u32>(sizeof(DrawGlyphRun) + inline_padding), static_cast<u32>(sizeof(glyph)) },
        .rect = { 10, 10, 20, 20 },
        .glyph_bounding_rect = { 10, 10, 20, 20 },
        .translation = { 10, 10 },
        .scale = 1,
        .color = Gfx::Color::Red,
        .orientation = Gfx::Orientation::Horizontal,
    };
    auto payload_size = align_up_to(sizeof(DisplayListCommandHeader) + sizeof(command) + inline_padding + sizeof(glyph), 16) - sizeof(DisplayListCommandHeader);
    DisplayListCommandHeader header {
        .type = DrawGlyphRun::command_type,
        .payload_size = static_cast<u32>(payload_size),
        .context_index = VISUAL_VIEWPORT_NODE_INDEX,
        .has_bounding_rect = true,
        .bounding_rect = command.glyph_bounding_rect,
    };
    ByteBuffer bytes;
    bytes.append(display_list_object_bytes(header));
    bytes.append(display_list_object_bytes(command));
    bytes.resize(bytes.size() + inline_padding, ByteBuffer::ZeroFillNewElements::Yes);
    bytes.append(display_list_object_bytes(glyph));
    bytes.resize(sizeof(header) + payload_size, ByteBuffer::ZeroFillNewElements::Yes);
    return bytes;
}

TEST_CASE(identical_display_lists_have_no_damage)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto display_list = fill_command_bytes({ 10, 10, 20, 20 }, Gfx::Color::Red);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(display_list, visual_context_tree, scroll_state, display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT(damage->is_empty());
}

TEST_CASE(inactive_optional_storage_does_not_damage_scaled_images)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    DrawScaledDecodedImageFrame command {
        .dst_rect = { 0, 0, 100, 100 },
        .src_rect = {},
        .frame_id = ImageFrameResourceId { 1 },
        .scaling_mode = Gfx::ScalingMode::Bilinear,
        .compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::Normal,
        .isolated_backdrop_color = {},
    };
    auto old_display_list = command_bytes(command, command.dst_rect);
    auto new_display_list = MUST(ByteBuffer::copy(old_display_list));

    // Optional<T> leaves its inactive T storage unspecified. Alter that storage
    // without changing the empty src_rect value represented by the command.
    new_display_list[sizeof(DisplayListCommandHeader) + sizeof(Gfx::IntRect)] ^= 0xff;

    ScrollStateSnapshot scroll_state;
    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT(damage->is_empty());
}

TEST_CASE(inline_payload_alignment_does_not_damage_glyph_runs)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto old_display_list = glyph_run_command_bytes(0);
    auto new_display_list = glyph_run_command_bytes(4);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT(damage->is_empty());
}

TEST_CASE(damage_contains_old_and_new_command_bounds)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto old_display_list = fill_command_bytes({ 10, 10, 20, 20 }, Gfx::Color::Red);
    auto new_display_list = fill_command_bytes({ 40, 40, 20, 20 }, Gfx::Color::Blue);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT_EQ(*damage, (Gfx::IntRect { 9, 9, 52, 52 }));
}

TEST_CASE(changed_unbounded_commands_require_full_repaint)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto old_display_list = command_bytes(Translate { { 1, 1 } });
    auto new_display_list = command_bytes(Translate { { 2, 2 } });
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(!damage.has_value());
}

TEST_CASE(changed_compositor_metadata_has_no_raster_damage)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto old_display_list = command_bytes(CompositorMainThreadWheelEventRegion { { 0, 0, 10, 10 } });
    auto new_display_list = command_bytes(CompositorMainThreadWheelEventRegion { { 20, 20, 10, 10 } });
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT(damage->is_empty());
}

TEST_CASE(changed_visual_context_damages_affected_commands)
{
    auto old_visual_context_tree = AccumulatedVisualContextTree::create();
    auto transform = Gfx::FloatMatrix4x4::identity();
    transform[0, 3] = 10;
    auto new_visual_context_tree = AccumulatedVisualContextTree::create({ transform, {} });
    auto display_list = fill_command_bytes({ 10, 10, 20, 20 }, Gfx::Color::Red);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(display_list, old_visual_context_tree, scroll_state, display_list, new_visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT_EQ(*damage, (Gfx::IntRect { 9, 9, 32, 22 }));
}

TEST_CASE(unrelated_inserted_visual_context_does_not_damage_commands)
{
    auto old_visual_context_tree = AccumulatedVisualContextTree::create();
    auto old_command_context = old_visual_context_tree.append(TransformData { Gfx::FloatMatrix4x4::identity(), {} }, VISUAL_VIEWPORT_NODE_INDEX);

    auto new_visual_context_tree = AccumulatedVisualContextTree::create();
    new_visual_context_tree.append(EffectsData { .opacity = 0.5f, .blend_mode = Gfx::CompositingAndBlendingOperator::Normal, .gfx_filter = {} }, VISUAL_VIEWPORT_NODE_INDEX);
    auto new_command_context = new_visual_context_tree.append(TransformData { Gfx::FloatMatrix4x4::identity(), {} }, VISUAL_VIEWPORT_NODE_INDEX);

    auto old_display_list = command_bytes(FillRect { { 10, 10, 20, 20 }, Gfx::Color::Red }, Gfx::IntRect { 10, 10, 20, 20 }, old_command_context);
    auto new_display_list = command_bytes(FillRect { { 10, 10, 20, 20 }, Gfx::Color::Red }, Gfx::IntRect { 10, 10, 20, 20 }, new_command_context);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, old_visual_context_tree, scroll_state, new_display_list, new_visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT(damage->is_empty());
}

static ByteBuffer canvas_command_bytes(Gfx::IntRect rect, u64 content_generation)
{
    DrawCanvas command {
        .dst_rect = rect,
        .canvas_id = CanvasId { 1 },
        .content_generation = content_generation,
        .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
    };
    return command_bytes(command, rect);
}

TEST_CASE(changed_canvas_content_generation_damages_canvas_rect)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto old_display_list = canvas_command_bytes({ 10, 10, 20, 20 }, 1);
    auto new_display_list = canvas_command_bytes({ 10, 10, 20, 20 }, 2);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT_EQ(*damage, (Gfx::IntRect { 9, 9, 22, 22 }));
}

TEST_CASE(changed_video_frame_content_generation_damages_video_rect)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    DrawVideoFrame command {
        .dst_rect = { 10, 10, 20, 20 },
        .video_frame_id = VideoFrameResourceId { 1 },
        .content_generation = 1,
        .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
    };
    auto old_display_list = command_bytes(command, command.dst_rect);
    command.content_generation = 2;
    auto new_display_list = command_bytes(command, command.dst_rect);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT_EQ(*damage, (Gfx::IntRect { 9, 9, 22, 22 }));
}

TEST_CASE(unchanged_canvas_content_generation_does_not_damage_canvas)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto canvas = canvas_command_bytes({ 10, 10, 20, 20 }, 1);
    auto old_fill = fill_command_bytes({ 50, 50, 10, 10 }, Gfx::Color::Red);
    auto new_fill = fill_command_bytes({ 50, 50, 10, 10 }, Gfx::Color::Blue);
    ByteBuffer old_display_list;
    old_display_list.append(canvas);
    old_display_list.append(old_fill);
    ByteBuffer new_display_list;
    new_display_list.append(canvas);
    new_display_list.append(new_fill);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT_EQ(*damage, (Gfx::IntRect { 49, 49, 12, 12 }));
}

TEST_CASE(inserted_and_removed_commands_do_not_damage_shifted_commands)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto first = fill_command_bytes({ 10, 10, 10, 10 }, Gfx::Color::Red);
    auto second = fill_command_bytes({ 30, 10, 10, 10 }, Gfx::Color::Green);
    auto third = fill_command_bytes({ 50, 10, 10, 10 }, Gfx::Color::Blue);
    auto removed = fill_command_bytes({ 70, 10, 10, 10 }, Gfx::Color::Yellow);
    auto inserted = fill_command_bytes({ 20, 40, 10, 10 }, Gfx::Color::Cyan);
    ByteBuffer old_display_list;
    old_display_list.append(first);
    old_display_list.append(second);
    old_display_list.append(third);
    old_display_list.append(removed);
    ByteBuffer new_display_list;
    new_display_list.append(first);
    new_display_list.append(inserted);
    new_display_list.append(second);
    new_display_list.append(third);
    ScrollStateSnapshot scroll_state;

    auto damage = compute_display_list_damage(old_display_list, visual_context_tree, scroll_state, new_display_list, visual_context_tree, scroll_state, { 0, 0, 100, 100 });
    EXPECT(damage.has_value());
    EXPECT_EQ(*damage, (Gfx::IntRect { 19, 9, 62, 42 }));
}
