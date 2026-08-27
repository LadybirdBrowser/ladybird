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
    EXPECT(updated_tree.is_compatible_with(tree));

    updated_tree.reuse_version_from(tree);
    EXPECT_EQ(updated_tree.version(), tree.version());
}

TEST_CASE(compatibility_requires_same_shape)
{
    auto tree = AccumulatedVisualContextTree::create();
    tree.append_spatial(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);

    auto shorter_tree = AccumulatedVisualContextTree::create();
    EXPECT(!shorter_tree.is_compatible_with(tree));

    auto different_type_tree = AccumulatedVisualContextTree::create();
    different_type_tree.append_spatial(PerspectiveData {}, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(!different_type_tree.is_compatible_with(tree));

    auto frame_tree = AccumulatedVisualContextTree::create();
    frame_tree.append_frame(EffectsData {}, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(!frame_tree.is_compatible_with(tree));

    auto mask_tree = AccumulatedVisualContextTree::create();
    mask_tree.append_frame(MaskData { .rect = Web::DevicePixelRect { 0, 0, 1, 1 } }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(!mask_tree.is_compatible_with(frame_tree));

    auto different_parent_tree = AccumulatedVisualContextTree::create();
    auto parent = different_parent_tree.append_spatial(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);
    different_parent_tree.append_spatial(make_transform(2), parent);

    auto same_node_count_tree = AccumulatedVisualContextTree::create();
    same_node_count_tree.append_spatial(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);
    same_node_count_tree.append_spatial(make_transform(2), VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT(!different_parent_tree.is_compatible_with(same_node_count_tree));

    auto frame_under_root_tree = AccumulatedVisualContextTree::create();
    auto frame_under_root_spatial = frame_under_root_tree.append_spatial(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);
    frame_under_root_tree.append_frame(EffectsData {}, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);

    auto frame_under_transform_tree = AccumulatedVisualContextTree::create();
    auto frame_under_transform_spatial = frame_under_transform_tree.append_spatial(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);
    frame_under_transform_tree.append_frame(EffectsData {}, NO_FRAME_NODE, frame_under_transform_spatial);

    EXPECT_EQ(frame_under_root_spatial, frame_under_transform_spatial);
    EXPECT(!frame_under_root_tree.is_compatible_with(frame_under_transform_tree));
}

TEST_CASE(compatibility_requires_same_empty_effective_clip)
{
    auto empty_clip_tree = AccumulatedVisualContextTree::create();
    empty_clip_tree.append_frame(ClipData { Gfx::FloatRect {}, {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);

    auto non_empty_clip_tree = AccumulatedVisualContextTree::create();
    non_empty_clip_tree.append_frame(ClipData { Gfx::FloatRect { 0, 0, 1, 1 }, {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT(!empty_clip_tree.is_compatible_with(non_empty_clip_tree));
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

TEST_CASE(compatibility_requires_same_root_isolation_frame)
{
    auto isolated = AccumulatedVisualContextTree::create();
    isolated.set_root_isolation_frame(isolated.append_frame(EffectsData {}, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX));

    auto not_isolated = AccumulatedVisualContextTree::create();
    not_isolated.append_frame(EffectsData {}, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(!isolated.is_compatible_with(not_isolated));

    auto also_isolated = AccumulatedVisualContextTree::create();
    also_isolated.set_root_isolation_frame(also_isolated.append_frame(EffectsData {}, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX));
    EXPECT(isolated.is_compatible_with(also_isolated));
}

TEST_CASE(compatibility_requires_same_visual_context_types)
{
    auto clip_tree = AccumulatedVisualContextTree::create();
    clip_tree.append_frame(ClipData { Gfx::FloatRect { 0, 0, 1, 1 }, {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);

    Gfx::Path path;
    path.move_to({ 0, 0 });
    path.line_to({ 1, 0 });
    path.line_to({ 1, 1 });
    path.close();

    auto clip_path_tree = AccumulatedVisualContextTree::create();
    clip_path_tree.append_frame(ClipPathData { path, Web::DevicePixelRect { 0, 0, 1, 1 }, Gfx::WindingRule::Nonzero }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT(!clip_tree.is_compatible_with(clip_path_tree));
}
