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

TEST_CASE(frames_under_an_empty_clip_clip_path_or_mask_have_an_empty_effective_clip)
{
    auto tree = AccumulatedVisualContextTree::create();
    auto empty_clip = tree.append_frame(ClipData { Gfx::FloatRect {}, {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    auto child_of_empty_clip = tree.append_frame(MaskData { .rect = Web::DevicePixelRect { 0, 0, 1, 1 } }, empty_clip, VISUAL_VIEWPORT_NODE_INDEX);
    auto empty_difference_clip = tree.append_frame(ClipData { Gfx::FloatRect {}, {}, ClipMode::Difference }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    auto empty_mask = tree.append_frame(MaskData { .rect = Web::DevicePixelRect {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    auto empty_clip_path = tree.append_frame(ClipPathData { Gfx::Path {}, {}, Gfx::WindingRule::Nonzero }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    auto effects = tree.append_frame(EffectsData {}, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);

    auto empty = tree.frames_with_empty_effective_clip();
    EXPECT(empty[empty_clip.value()]);
    EXPECT(empty[child_of_empty_clip.value()]);
    EXPECT(!empty[empty_difference_clip.value()]);
    EXPECT(empty[empty_mask.value()]);
    EXPECT(empty[empty_clip_path.value()]);
    EXPECT(!empty[effects.value()]);
}

TEST_CASE(patching_frame_data_updates_the_effective_clip_verdict)
{
    auto tree = AccumulatedVisualContextTree::create();
    auto clip = tree.append_frame(ClipData { Gfx::FloatRect {}, {} }, NO_FRAME_NODE, VISUAL_VIEWPORT_NODE_INDEX);
    auto child = tree.append_frame(EffectsData {}, clip, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(tree.frames_with_empty_effective_clip()[child.value()]);

    tree.set_frame_data(clip, ClipData { Gfx::FloatRect { 0, 0, 10, 10 }, {} });
    EXPECT(!tree.frames_with_empty_effective_clip()[child.value()]);

    tree.set_frame_data(clip, ClipData { Gfx::FloatRect {}, {} });
    EXPECT(tree.frames_with_empty_effective_clip()[child.value()]);
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

TEST_CASE(visual_animation_samples_restore_original_node_values)
{
    auto tree = AccumulatedVisualContextTree::create();
    auto spatial = tree.append_spatial(make_transform(12), VISUAL_VIEWPORT_NODE_INDEX);
    auto frame = tree.append_frame(EffectsData {
                                       .opacity = 0.75f,
                                       .blend_mode = Gfx::CompositingAndBlendingOperator::Normal,
                                       .gfx_filter = {},
                                   },
        NO_FRAME_NODE, spatial);
    tree.set_visual_animations({
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Transform,
            .visual_context_node_indices = { spatial.value() },
            .local_time_at_anchor_ms = 500,
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 0 } } } },
                { 1, {}, Web::Compositor::VisualAnimationTransformList { { Web::Compositor::VisualAnimationTransformOperationKind::TranslateX, { 8 } } } },
            },
        },
        {
            .target_kind = Web::Compositor::VisualAnimation::TargetKind::Opacity,
            .visual_context_node_indices = { frame.value() },
            .local_time_at_anchor_ms = 500,
            .iteration_duration_ms = 1000,
            .easing = {},
            .keyframes = {
                { 0, {}, 1.0f },
                { 1, {}, 0.0f },
            },
        },
    });

    AccumulatedVisualContextTree::VisualAnimationOriginalValues original_values;
    tree.sample_visual_animations(0, original_values);
    auto sampled_translation = tree.spatial_node_at(spatial).data.get<TransformData>().matrix[0, 3];
    EXPECT_EQ(sampled_translation, 4.0f);
    EXPECT_EQ(tree.frame_node_at(frame).data.get<EffectsData>().opacity, 0.5f);

    tree.restore_visual_animation_original_values(original_values);
    EXPECT(original_values.is_empty());
    auto restored_translation = tree.spatial_node_at(spatial).data.get<TransformData>().matrix[0, 3];
    EXPECT_EQ(restored_translation, 12.0f);
    EXPECT_EQ(tree.frame_node_at(frame).data.get<EffectsData>().opacity, 0.75f);
}
