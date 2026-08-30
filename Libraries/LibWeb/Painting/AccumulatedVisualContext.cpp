/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibGfx/Matrix4x4.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

// Whole-tree transform root: the visual viewport transform for document trees, the content
// placement for nested display list trees.
AccumulatedVisualContextTree::AccumulatedVisualContextTree(TransformData root_transform, bool root_is_visual_viewport)
    : m_root_is_visual_viewport(root_is_visual_viewport)
{
    m_spatial_nodes.append({ move(root_transform), VISUAL_VIEWPORT_NODE_INDEX });
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create(TransformData visual_viewport_transform)
{
    return AccumulatedVisualContextTree { move(visual_viewport_transform), true };
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::create_with_content_root(TransformData content_transform)
{
    return AccumulatedVisualContextTree { move(content_transform), false };
}

ErrorOr<AccumulatedVisualContextTree> AccumulatedVisualContextTree::from_serialized_bytes(ReadonlyBytes bytes)
{
    auto const* retained_tree = Layout::RustFFI::visual_context_tree_deserialize(bytes.data(), bytes.size());
    if (!retained_tree)
        return Error::from_string_literal("Malformed visual context tree bytes");
    return materialize_from_rust(retained_tree);
}

u64 AccumulatedVisualContextTree::version() const
{
    return Layout::RustFFI::visual_context_tree_version(m_rust_tree);
}

ByteBuffer AccumulatedVisualContextTree::serialize_to_bytes() const
{
    ByteBuffer bytes;
    Layout::RustFFI::visual_context_tree_serialize(m_rust_tree, &bytes, [](void* sink, u8 const* data, size_t size) {
        MUST(static_cast<ByteBuffer*>(sink)->try_append(data, size));
    });
    return bytes;
}

SpatialNodeIndex AccumulatedVisualContextTree::append_spatial(SpatialData data, SpatialNodeIndex parent)
{
    VERIFY(parent.value() < m_spatial_nodes.size());
    auto index = SpatialNodeIndex(m_spatial_nodes.size());
    m_spatial_nodes.append({ move(data), parent });
    return index;
}

static bool frame_data_clips_everything(FrameData const& data)
{
    return data.visit(
        [](ClipData const& clip) { return clip.mode == ClipMode::Intersect && clip.rect.is_empty(); },
        [](ClipPathData const& clip_path) { return clip_path.path.bounding_box().is_empty(); },
        [](EffectsData const&) { return false; },
        [](MaskData const& mask) { return mask.rect.is_empty(); });
}

FrameNodeIndex AccumulatedVisualContextTree::append_frame(FrameData data, FrameNodeIndex parent, SpatialNodeIndex spatial)
{
    VERIFY(spatial.value() < m_spatial_nodes.size());
    VERIFY(parent == NO_FRAME_NODE || parent.value() < m_frame_nodes.size());
    auto index = FrameNodeIndex(m_frame_nodes.size());
    bool clips_everything = frame_data_clips_everything(data);
    m_frame_nodes.append({ move(data), parent, spatial, clips_everything });
    return index;
}

void AccumulatedVisualContextTree::set_frame_data(FrameNodeIndex index, FrameData data)
{
    auto& node = m_frame_nodes[index.value()];
    node.clips_everything = frame_data_clips_everything(data);
    node.data = move(data);
}

Vector<bool> AccumulatedVisualContextTree::frames_with_empty_effective_clip() const
{
    Vector<bool> empty;
    empty.ensure_capacity(m_frame_nodes.size());
    for (auto const& node : m_frame_nodes)
        empty.unchecked_append(node.clips_everything || (node.parent != NO_FRAME_NODE && empty[node.parent.value()]));
    return empty;
}

AccumulatedVisualContextTree::AccumulatedVisualContextTree(AccumulatedVisualContextTree const& other)
    : m_rust_tree(other.m_rust_tree ? Layout::RustFFI::visual_context_tree_retain(other.m_rust_tree) : nullptr)
    , m_spatial_nodes(other.m_spatial_nodes)
    , m_frame_nodes(other.m_frame_nodes)
    , m_root_is_visual_viewport(other.m_root_is_visual_viewport)
    , m_root_isolation_frame(other.m_root_isolation_frame)
{
}

AccumulatedVisualContextTree& AccumulatedVisualContextTree::operator=(AccumulatedVisualContextTree const& other)
{
    if (this == &other)
        return *this;
    adopt_rust_tree(other.m_rust_tree ? Layout::RustFFI::visual_context_tree_retain(other.m_rust_tree) : nullptr);
    m_spatial_nodes = other.m_spatial_nodes;
    m_frame_nodes = other.m_frame_nodes;
    m_root_is_visual_viewport = other.m_root_is_visual_viewport;
    m_root_isolation_frame = other.m_root_isolation_frame;
    return *this;
}

AccumulatedVisualContextTree::AccumulatedVisualContextTree(AccumulatedVisualContextTree&& other)
    : m_rust_tree(exchange(other.m_rust_tree, nullptr))
    , m_spatial_nodes(move(other.m_spatial_nodes))
    , m_frame_nodes(move(other.m_frame_nodes))
    , m_root_is_visual_viewport(other.m_root_is_visual_viewport)
    , m_root_isolation_frame(other.m_root_isolation_frame)
{
}

AccumulatedVisualContextTree& AccumulatedVisualContextTree::operator=(AccumulatedVisualContextTree&& other)
{
    if (this == &other)
        return *this;
    adopt_rust_tree(exchange(other.m_rust_tree, nullptr));
    m_spatial_nodes = move(other.m_spatial_nodes);
    m_frame_nodes = move(other.m_frame_nodes);
    m_root_is_visual_viewport = other.m_root_is_visual_viewport;
    m_root_isolation_frame = other.m_root_isolation_frame;
    return *this;
}

AccumulatedVisualContextTree::~AccumulatedVisualContextTree()
{
    release_rust_tree();
}

void AccumulatedVisualContextTree::adopt_rust_tree(void const* retained_tree)
{
    release_rust_tree();
    m_rust_tree = retained_tree;
}

void AccumulatedVisualContextTree::release_rust_tree()
{
    if (!m_rust_tree)
        return;
    Layout::RustFFI::visual_context_tree_release(m_rust_tree);
    m_rust_tree = nullptr;
}

size_t AccumulatedVisualContextTree::spatial_node_count() const
{
    return Layout::RustFFI::visual_context_tree_spatial_node_count(m_rust_tree);
}

size_t AccumulatedVisualContextTree::frame_node_count() const
{
    return Layout::RustFFI::visual_context_tree_frame_node_count(m_rust_tree);
}

TransformWithOrigin AccumulatedVisualContextTree::visual_viewport_transform() const
{
    return Layout::RustFFI::visual_context_tree_visual_viewport_transform(m_rust_tree);
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::with_visual_viewport_transform(TransformWithOrigin const& transform) const
{
    auto tree = materialize_from_rust(Layout::RustFFI::visual_context_tree_with_visual_viewport_transform(m_rust_tree, transform));
    tree.m_visual_animations = m_visual_animations;
    return tree;
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::with_visual_animation_samples(i64 monotonic_time_ns) const
{
    Vector<Layout::RustFFI::FfiFrameOpacitySample> opacity_samples;
    Vector<Layout::RustFFI::FfiSpatialTransformSample> transform_samples;
    for (auto const& animation : m_visual_animations) {
        auto elapsed_nanoseconds = monotonic_time_ns > animation.monotonic_time_at_anchor_ns
            ? monotonic_time_ns - animation.monotonic_time_at_anchor_ns
            : 0;
        auto sample = animation.sample(AK::Duration::from_nanoseconds(elapsed_nanoseconds));
        if (!sample.has_value())
            continue;
        for (auto node_index : animation.visual_context_node_indices) {
            if (animation.target_kind == Compositor::VisualAnimation::TargetKind::Opacity)
                opacity_samples.append({ .frame = node_index, .opacity = sample->opacity });
            else
                transform_samples.append({ .spatial = node_index, .matrix = sample->transform });
        }
    }
    auto tree = materialize_from_rust(Layout::RustFFI::visual_context_tree_with_sampled_values(m_rust_tree, opacity_samples.data(), opacity_samples.size(), transform_samples.data(), transform_samples.size()));
    tree.m_visual_animations = m_visual_animations;
    return tree;
}

bool AccumulatedVisualContextTree::visual_animation_targets_are_valid(Compositor::VisualAnimation const& animation) const
{
    auto const& targets = animation.visual_context_node_indices;
    return Layout::RustFFI::visual_context_tree_visual_animation_targets_are_valid(m_rust_tree, animation.target_kind == Compositor::VisualAnimation::TargetKind::Opacity, targets.data(), targets.size());
}

Optional<float> AccumulatedVisualContextTree::effects_opacity(FrameNodeIndex frame) const
{
    float opacity = 1;
    if (!Layout::RustFFI::visual_context_tree_effects_opacity(m_rust_tree, frame, &opacity))
        return {};
    return opacity;
}

Vector<bool> AccumulatedVisualContextTree::spatial_nodes_in_subtrees_of(ReadonlySpan<SpatialNodeIndex> roots) const
{
    Vector<bool> in_subtree;
    in_subtree.resize(spatial_node_count());
    Layout::RustFFI::visual_context_tree_mark_spatial_subtrees(m_rust_tree, roots.data(), roots.size(), in_subtree.data(), in_subtree.size());
    return in_subtree;
}

Optional<Gfx::FloatPoint> AccumulatedVisualContextTree::transform_point_for_hit_test(ContextRef context, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state, ClipBehavior clip_behavior) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    Gfx::FloatPoint local_point;
    if (!Layout::RustFFI::visual_context_tree_transform_point_for_hit_test(m_rust_tree, context, screen_point, scroll_offsets.data(), scroll_offsets.size(), clip_behavior == ClipBehavior::Respect, &local_point))
        return {};
    return local_point;
}

Gfx::FloatPoint AccumulatedVisualContextTree::inverse_transform_point(SpatialNodeIndex index, Gfx::FloatPoint screen_point) const
{
    return Layout::RustFFI::visual_context_tree_inverse_transform_point(m_rust_tree, index, screen_point);
}

Gfx::FloatRect AccumulatedVisualContextTree::transform_rect_to_viewport(SpatialNodeIndex index, Gfx::FloatRect const& source_rect, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    return Layout::RustFFI::visual_context_tree_transform_rect_to_viewport(m_rust_tree, index, source_rect, scroll_offsets.data(), scroll_offsets.size(), include_visual_viewport_transform == IncludeVisualViewportTransform::Yes);
}

Gfx::FloatPoint AccumulatedVisualContextTree::cumulative_scroll_chain_offset(SpatialNodeIndex index, ScrollStateSnapshot const& scroll_state) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    return Layout::RustFFI::visual_context_tree_cumulative_scroll_chain_offset(m_rust_tree, index, scroll_offsets.data(), scroll_offsets.size());
}

Gfx::FloatMatrix4x4 AccumulatedVisualContextTree::accumulated_matrix(SpatialNodeIndex index, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    return Layout::RustFFI::visual_context_tree_accumulated_matrix(m_rust_tree, index, scroll_offsets.data(), scroll_offsets.size(), include_visual_viewport_transform == IncludeVisualViewportTransform::Yes);
}

void AccumulatedVisualContextTree::sample_visual_animations(i64 monotonic_time_ns, VisualAnimationOriginalValues& original_values)
{
    VERIFY(original_values.is_empty());
    for (auto const& animation : m_visual_animations) {
        auto elapsed_nanoseconds = monotonic_time_ns > animation.monotonic_time_at_anchor_ns
            ? monotonic_time_ns - animation.monotonic_time_at_anchor_ns
            : 0;
        auto sample = animation.sample(AK::Duration::from_nanoseconds(elapsed_nanoseconds));
        if (!sample.has_value())
            continue;
        for (auto node_index : animation.visual_context_node_indices) {
            if (animation.target_kind == Compositor::VisualAnimation::TargetKind::Opacity) {
                auto frame_node_index = FrameNodeIndex { node_index };
                auto& frame = frame_node_at(frame_node_index);
                if (!any_of(original_values.opacities, [&](auto const& original) { return original.node_index == frame_node_index; }))
                    original_values.opacities.append({ frame_node_index, frame.data.get<EffectsData>().opacity });
                frame.data.get<EffectsData>().opacity = sample->opacity;
            } else {
                auto spatial_node_index = SpatialNodeIndex { node_index };
                auto& spatial = spatial_node_at(spatial_node_index);
                if (!any_of(original_values.transforms, [&](auto const& original) { return original.node_index == spatial_node_index; }))
                    original_values.transforms.append({ spatial_node_index, spatial.data.get<TransformData>().matrix });
                spatial.data.get<TransformData>().matrix = sample->transform;
            }
        }
    }
}

void AccumulatedVisualContextTree::restore_visual_animation_original_values(VisualAnimationOriginalValues& original_values)
{
    for (auto const& original : original_values.opacities)
        frame_node_at(original.node_index).data.get<EffectsData>().opacity = original.value;
    for (auto const& original : original_values.transforms)
        spatial_node_at(original.node_index).data.get<TransformData>().matrix = original.value;
    original_values.clear();
}

bool AccumulatedVisualContextTree::frame_is_isolated_by_layer_frame(FrameNodeIndex frame) const
{
    return Layout::RustFFI::visual_context_tree_frame_is_isolated_by_layer_frame(m_rust_tree, frame);
}

bool AccumulatedVisualContextTree::has_unisolated_blending_frame() const
{
    return Layout::RustFFI::visual_context_tree_has_unisolated_blending_frame(m_rust_tree);
}

void AccumulatedVisualContextTree::for_each_effects_filter_bytes(Function<void(ReadonlyBytes)> const& visit) const
{
    struct FilterBytesVisitor {
        Function<void(ReadonlyBytes)> const& visit;
    } visitor { visit };
    Layout::RustFFI::visual_context_tree_for_each_effects_filter_bytes(m_rust_tree, &visitor, [](void* context, u8 const* bytes, size_t size) {
        static_cast<FilterBytesVisitor*>(context)->visit(ReadonlyBytes { bytes, size });
    });
}

void AccumulatedVisualContextTree::dump(StringBuilder& builder, ReadonlySpan<DisplayListCommandRun> command_runs, Function<Optional<String>(SpatialNodeIndex)> const& spatial_node_owner_label, Function<Optional<String>(FrameNodeIndex)> const& frame_node_owner_label) const
{
    struct OwnerLabelSource {
        Function<Optional<String>(SpatialNodeIndex)> const& spatial_node_owner_label;
        Function<Optional<String>(FrameNodeIndex)> const& frame_node_owner_label;
    } owner_label_source { spatial_node_owner_label, frame_node_owner_label };
    Layout::RustFFI::visual_context_tree_dump(
        m_rust_tree, command_runs.data(), command_runs.size(),
        &owner_label_source, [](void* context, bool is_frame, u32 index, void* label_sink) -> bool {
            auto& source = *static_cast<OwnerLabelSource*>(context);
            auto label = is_frame ? source.frame_node_owner_label(FrameNodeIndex { index }) : source.spatial_node_owner_label(SpatialNodeIndex { index });
            if (!label.has_value())
                return false;
            auto label_bytes = label->bytes();
            Layout::RustFFI::layout_arena_paint_push_bytes(label_sink, label_bytes.data(), label_bytes.size());
            return true; },
        &builder, [](void* sink, u8 const* bytes, size_t size) { static_cast<StringBuilder*>(sink)->append(StringView { bytes, size }); });
}

void resolve_sticky_offsets(AccumulatedVisualContextTree const& tree, ScrollStateSnapshot& scroll_state)
{
    auto scroll_offsets = scroll_state.device_offsets();
    Layout::RustFFI::visual_context_tree_resolve_sticky_offsets(
        tree.rust_tree(), scroll_offsets.data(), scroll_offsets.size(),
        &scroll_state, [](void* sink, SpatialNodeIndex index, Gfx::FloatPoint offset) {
            static_cast<ScrollStateSnapshot*>(sink)->set_device_offset_for_index(index, offset);
        });
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::AccumulatedVisualContextTree const& tree)
{
    TRY(encoder.encode(tree.serialize_to_bytes()));
    TRY(encoder.encode(tree.m_visual_animations));
    return {};
}

template<>
ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder& decoder)
{
    auto bytes = TRY(decoder.decode<ByteBuffer>());
    auto tree = TRY(Web::Painting::AccumulatedVisualContextTree::from_serialized_bytes(bytes));
    auto visual_animations = TRY(decoder.decode<Vector<Web::Compositor::VisualAnimation>>());
    auto spatial_nodes = tree.spatial_nodes();
    auto frame_nodes = tree.frame_nodes();
    for (auto const& animation : visual_animations) {
        if (!animation.is_valid())
            return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree has an invalid visual animation");
        for (auto node_index : animation.visual_context_node_indices) {
            if (animation.target_kind == Web::Compositor::VisualAnimation::TargetKind::Opacity) {
                if (node_index >= frame_nodes.size() || !frame_nodes[node_index].data.has<Web::Painting::EffectsData>())
                    return Error::from_string_literal("IPC decode: Opacity animation target is not an effects frame");
            } else {
                if (node_index >= spatial_nodes.size())
                    return Error::from_string_literal("IPC decode: Transform animation target is not a transform node");
                auto const* transform = spatial_nodes[node_index].data.get_pointer<Web::Painting::TransformData>();
                if (!transform || transform->role != Web::Painting::TransformDataRole::CssTransform || transform->synthetic_plane)
                    return Error::from_string_literal("IPC decode: Transform animation target is not a CSS transform node");
            }
        }
    }
    tree.set_visual_animations(move(visual_animations));
    return tree;
}

}
