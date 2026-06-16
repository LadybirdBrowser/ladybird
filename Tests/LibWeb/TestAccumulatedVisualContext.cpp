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
    tree.append(make_transform(2), VISUAL_VIEWPORT_NODE_INDEX);

    auto updated_tree = AccumulatedVisualContextTree::create(make_transform(3));
    updated_tree.append(make_transform(4), VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT_NE(tree.version(), updated_tree.version());
    EXPECT(updated_tree.is_compatible_with(tree));

    updated_tree.reuse_version_from(tree);
    EXPECT_EQ(updated_tree.version(), tree.version());
}

TEST_CASE(compatibility_requires_same_shape)
{
    auto tree = AccumulatedVisualContextTree::create();
    tree.append(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);

    auto shorter_tree = AccumulatedVisualContextTree::create();
    EXPECT(!shorter_tree.is_compatible_with(tree));

    auto different_type_tree = AccumulatedVisualContextTree::create();
    different_type_tree.append(EffectsData {}, VISUAL_VIEWPORT_NODE_INDEX);
    EXPECT(!different_type_tree.is_compatible_with(tree));

    auto different_parent_tree = AccumulatedVisualContextTree::create();
    auto parent = different_parent_tree.append(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);
    different_parent_tree.append(make_transform(2), parent);

    auto same_node_count_tree = AccumulatedVisualContextTree::create();
    same_node_count_tree.append(make_transform(1), VISUAL_VIEWPORT_NODE_INDEX);
    same_node_count_tree.append(make_transform(2), VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT(!different_parent_tree.is_compatible_with(same_node_count_tree));
}

TEST_CASE(compatibility_requires_same_empty_effective_clip)
{
    auto empty_clip_tree = AccumulatedVisualContextTree::create();
    empty_clip_tree.append(ClipData { Web::DevicePixelRect {}, {} }, VISUAL_VIEWPORT_NODE_INDEX);

    auto non_empty_clip_tree = AccumulatedVisualContextTree::create();
    non_empty_clip_tree.append(ClipData { Web::DevicePixelRect { 0, 0, 1, 1 }, {} }, VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT(!empty_clip_tree.is_compatible_with(non_empty_clip_tree));
}

TEST_CASE(compatibility_requires_same_visual_context_types)
{
    auto clip_tree = AccumulatedVisualContextTree::create();
    clip_tree.append(ClipData { Web::DevicePixelRect { 0, 0, 1, 1 }, {} }, VISUAL_VIEWPORT_NODE_INDEX);

    Gfx::Path path;
    path.move_to({ 0, 0 });
    path.line_to({ 1, 0 });
    path.line_to({ 1, 1 });
    path.close();

    auto clip_path_tree = AccumulatedVisualContextTree::create();
    clip_path_tree.append(ClipPathData { path, Web::DevicePixelRect { 0, 0, 1, 1 }, Gfx::WindingRule::Nonzero }, VISUAL_VIEWPORT_NODE_INDEX);

    EXPECT(!clip_tree.is_compatible_with(clip_path_tree));
}
