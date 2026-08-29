/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>

using namespace Web::Painting;

static TransformData make_transform(float translation)
{
    auto matrix = Gfx::FloatMatrix4x4::identity();
    matrix[0, 3] = translation;
    matrix[1, 3] = translation;
    return { matrix, { translation, translation } };
}

TEST_CASE(compatible_trees_can_reuse_versions)
{
    auto tree = AccumulatedVisualContextTree::create(make_transform(1));
    tree.append_spatial(make_transform(2), VISUAL_VIEWPORT_NODE_INDEX);

    auto updated_tree = AccumulatedVisualContextTree::create(make_transform(3));
    updated_tree.append_spatial(make_transform(4), VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT_NE(tree.version(), updated_tree.version());

    updated_tree.reuse_version_from(tree);
    EXPECT_EQ(updated_tree.version(), tree.version());
}

TEST_CASE(mask_data_contributes_to_empty_effective_clip)
{
    auto tree = AccumulatedVisualContextTree::create();
    auto non_empty_mask = tree.append_frame(MaskData { .rect = Web::DevicePixelRect { 0, 0, 1, 1 } }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(!tree.has_empty_effective_clip(non_empty_mask));

    auto empty_mask = tree.append_frame(MaskData { .rect = Web::DevicePixelRect {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(tree.has_empty_effective_clip(empty_mask));

    auto empty_parent = tree.append_frame(ClipData { Gfx::FloatRect {}, {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    auto inherited_empty_clip = tree.append_frame(MaskData { .rect = Web::DevicePixelRect { 0, 0, 1, 1 } }, empty_parent, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(tree.has_empty_effective_clip(inherited_empty_clip));
    EXPECT(!tree.has_empty_effective_clip(NO_FRAME_NODE));
}

TEST_CASE(a_difference_clip_with_an_empty_rect_is_not_an_empty_effective_clip)
{
    auto tree = AccumulatedVisualContextTree::create();
    auto frame = tree.append_frame(ClipData { Gfx::FloatRect {}, {}, ClipMode::Difference }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(!tree.has_empty_effective_clip(frame));
}

TEST_CASE(a_difference_clip_passes_only_points_outside_its_rect)
{
    auto tree = AccumulatedVisualContextTree::create();
    auto frame = tree.append_frame(ClipData { Gfx::FloatRect { 10, 10, 20, 20 }, {}, ClipMode::Difference }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    ContextRef context { VISUAL_VIEWPORT_NODE_INDEX, frame };
    ScrollStateSnapshot scroll_state;
    EXPECT(!tree.transform_point_for_hit_test(context, { 15, 15 }, scroll_state).has_value());
    EXPECT(tree.transform_point_for_hit_test(context, { 5, 5 }, scroll_state).has_value());
}

TEST_CASE(a_fractional_clip_rect_contains_points_by_float_containment)
{
    auto tree = AccumulatedVisualContextTree::create();
    auto frame = tree.append_frame(ClipData { Gfx::FloatRect { 10.5f, 10.5f, 20, 20 }, {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    ContextRef context { VISUAL_VIEWPORT_NODE_INDEX, frame };
    ScrollStateSnapshot scroll_state;
    EXPECT(!tree.transform_point_for_hit_test(context, { 10.25f, 15 }, scroll_state).has_value());
    EXPECT(tree.transform_point_for_hit_test(context, { 10.75f, 15 }, scroll_state).has_value());
}
