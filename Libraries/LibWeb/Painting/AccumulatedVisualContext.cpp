/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

AccumulatedVisualContextTree::AccumulatedVisualContextTree(void const* retained_tree)
    : m_rust_tree(retained_tree)
{
    VERIFY(m_rust_tree);
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::adopt_rust_handle(void const* retained_tree)
{
    return AccumulatedVisualContextTree { retained_tree };
}

ErrorOr<AccumulatedVisualContextTree> AccumulatedVisualContextTree::from_serialized_bytes(ReadonlyBytes bytes)
{
    auto const* retained_tree = Layout::RustFFI::visual_context_tree_deserialize(bytes.data(), bytes.size());
    if (!retained_tree)
        return Error::from_string_literal("Malformed visual context tree bytes");
    return adopt_rust_handle(retained_tree);
}

AccumulatedVisualContextTree::AccumulatedVisualContextTree(AccumulatedVisualContextTree const& other)
    : m_rust_tree(other.m_rust_tree ? Layout::RustFFI::visual_context_tree_retain(other.m_rust_tree) : nullptr)
    , m_visual_animations(other.m_visual_animations)
{
}

AccumulatedVisualContextTree& AccumulatedVisualContextTree::operator=(AccumulatedVisualContextTree const& other)
{
    if (this == &other)
        return *this;
    auto const* retained_tree = other.m_rust_tree ? Layout::RustFFI::visual_context_tree_retain(other.m_rust_tree) : nullptr;
    release_rust_handle();
    m_rust_tree = retained_tree;
    m_visual_animations = other.m_visual_animations;
    return *this;
}

AccumulatedVisualContextTree::AccumulatedVisualContextTree(AccumulatedVisualContextTree&& other)
    : m_rust_tree(exchange(other.m_rust_tree, nullptr))
    , m_visual_animations(move(other.m_visual_animations))
{
}

AccumulatedVisualContextTree& AccumulatedVisualContextTree::operator=(AccumulatedVisualContextTree&& other)
{
    if (this == &other)
        return *this;
    release_rust_handle();
    m_rust_tree = exchange(other.m_rust_tree, nullptr);
    m_visual_animations = move(other.m_visual_animations);
    return *this;
}

AccumulatedVisualContextTree::~AccumulatedVisualContextTree()
{
    release_rust_handle();
}

void AccumulatedVisualContextTree::release_rust_handle()
{
    if (!m_rust_tree)
        return;
    Layout::RustFFI::visual_context_tree_release(m_rust_tree);
    m_rust_tree = nullptr;
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
    auto tree = adopt_rust_handle(Layout::RustFFI::visual_context_tree_with_visual_viewport_transform(m_rust_tree, transform));
    tree.m_visual_animations = m_visual_animations;
    return tree;
}

void AccumulatedVisualContextTree::set_visual_animations(Vector<Compositor::VisualAnimation> animations)
{
    m_visual_animations = animations.is_empty() ? nullptr : adopt_ref(*new VisualAnimationList(move(animations)));
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::with_visual_animation_samples(i64 monotonic_time_ns) const
{
    Vector<Layout::RustFFI::FfiFrameOpacitySample> opacity_samples;
    Vector<Layout::RustFFI::FfiSpatialTransformSample> transform_samples;
    for (auto const& animation : visual_animations()) {
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
    auto tree = adopt_rust_handle(Layout::RustFFI::visual_context_tree_with_sampled_values(m_rust_tree, opacity_samples.data(), opacity_samples.size(), transform_samples.data(), transform_samples.size()));
    tree.m_visual_animations = m_visual_animations;
    return tree;
}

bool AccumulatedVisualContextTree::visual_animation_targets_are_valid(Compositor::VisualAnimation const& animation) const
{
    auto const& targets = animation.visual_context_node_indices;
    return Layout::RustFFI::visual_context_tree_visual_animation_targets_are_valid(m_rust_tree, animation.target_kind == Compositor::VisualAnimation::TargetKind::Opacity, targets.data(), targets.size());
}

bool AccumulatedVisualContextTree::frame_is_effects(FrameNodeIndex frame) const
{
    u32 target = frame.value();
    return Layout::RustFFI::visual_context_tree_visual_animation_targets_are_valid(m_rust_tree, true, &target, 1);
}

bool AccumulatedVisualContextTree::spatial_node_is_css_transform(SpatialNodeIndex spatial) const
{
    u32 target = spatial.value();
    return Layout::RustFFI::visual_context_tree_visual_animation_targets_are_valid(m_rust_tree, false, &target, 1);
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

struct OwnerLabelSource {
    Function<Optional<String>(SpatialNodeIndex)> const& spatial_node_owner_label;
    Function<Optional<String>(FrameNodeIndex)> const& frame_node_owner_label;
};

static bool push_owner_label(void* context, bool is_frame, u32 index, void* label_sink)
{
    auto& source = *static_cast<OwnerLabelSource*>(context);
    auto label = is_frame ? source.frame_node_owner_label(FrameNodeIndex { index }) : source.spatial_node_owner_label(SpatialNodeIndex { index });
    if (!label.has_value())
        return false;
    auto label_bytes = label->bytes();
    Layout::RustFFI::layout_arena_paint_push_bytes(label_sink, label_bytes.data(), label_bytes.size());
    return true;
}

static void append_dump_text(void* sink, u8 const* bytes, size_t size)
{
    static_cast<StringBuilder*>(sink)->append(StringView { bytes, size });
}

void AccumulatedVisualContextTree::dump(StringBuilder& builder, ReadonlySpan<DisplayListCommandRun> command_runs, Function<Optional<String>(SpatialNodeIndex)> const& spatial_node_owner_label, Function<Optional<String>(FrameNodeIndex)> const& frame_node_owner_label) const
{
    OwnerLabelSource owner_label_source { spatial_node_owner_label, frame_node_owner_label };
    Layout::RustFFI::visual_context_tree_dump(m_rust_tree, command_runs.data(), command_runs.size(), &owner_label_source, push_owner_label, &builder, append_dump_text);
}

void resolve_sticky_offsets(AccumulatedVisualContextTree const& tree, ScrollStateSnapshot& scroll_state)
{
    auto scroll_offsets = scroll_state.device_offsets();
    Layout::RustFFI::visual_context_tree_resolve_sticky_offsets(
        tree.rust_handle(), scroll_offsets.data(), scroll_offsets.size(),
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
    auto visual_animations = tree.visual_animation_list();
    TRY(encoder.encode(visual_animations ? visual_animations->animations : Vector<Web::Compositor::VisualAnimation> {}));
    return {};
}

template<>
ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder& decoder)
{
    auto bytes = TRY(decoder.decode<ByteBuffer>());
    auto tree = TRY(Web::Painting::AccumulatedVisualContextTree::from_serialized_bytes(bytes));
    auto visual_animations = TRY(decoder.decode<Vector<Web::Compositor::VisualAnimation>>());
    for (auto const& animation : visual_animations) {
        if (!animation.is_valid())
            return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree has an invalid visual animation");
        if (!tree.visual_animation_targets_are_valid(animation))
            return Error::from_string_literal("IPC decode: AccumulatedVisualContextTree visual animation targets the wrong node kind");
    }
    tree.set_visual_animations(move(visual_animations));
    return tree;
}

}
