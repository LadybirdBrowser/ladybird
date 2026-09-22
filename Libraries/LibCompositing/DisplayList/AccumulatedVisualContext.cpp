/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/RustFFI.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

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
    auto const* retained_tree = Compositing::RustFFI::visual_context_tree_deserialize(bytes.data(), bytes.size());
    if (!retained_tree)
        return Error::from_string_literal("Malformed visual context tree bytes");
    return adopt_rust_handle(retained_tree);
}

AccumulatedVisualContextTree::AccumulatedVisualContextTree(AccumulatedVisualContextTree const& other)
    : m_rust_tree(other.m_rust_tree ? Compositing::RustFFI::visual_context_tree_retain(other.m_rust_tree) : nullptr)
{
}

AccumulatedVisualContextTree& AccumulatedVisualContextTree::operator=(AccumulatedVisualContextTree const& other)
{
    if (this == &other)
        return *this;
    auto const* retained_tree = other.m_rust_tree ? Compositing::RustFFI::visual_context_tree_retain(other.m_rust_tree) : nullptr;
    release_rust_handle();
    m_rust_tree = retained_tree;
    return *this;
}

AccumulatedVisualContextTree::AccumulatedVisualContextTree(AccumulatedVisualContextTree&& other)
    : m_rust_tree(exchange(other.m_rust_tree, nullptr))
{
}

AccumulatedVisualContextTree& AccumulatedVisualContextTree::operator=(AccumulatedVisualContextTree&& other)
{
    if (this == &other)
        return *this;
    release_rust_handle();
    m_rust_tree = exchange(other.m_rust_tree, nullptr);
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
    Compositing::RustFFI::visual_context_tree_release(m_rust_tree);
    m_rust_tree = nullptr;
}

u64 AccumulatedVisualContextTree::structural_epoch() const
{
    return Compositing::RustFFI::visual_context_tree_structural_epoch(m_rust_tree);
}

ByteBuffer AccumulatedVisualContextTree::serialize_to_bytes() const
{
    ByteBuffer bytes;
    Compositing::RustFFI::visual_context_tree_serialize(m_rust_tree, &bytes, [](void* sink, u8 const* data, size_t size) {
        MUST(static_cast<ByteBuffer*>(sink)->try_append(data, size));
    });
    return bytes;
}

size_t AccumulatedVisualContextTree::spatial_node_count() const
{
    return Compositing::RustFFI::visual_context_tree_spatial_node_count(m_rust_tree);
}

bool AccumulatedVisualContextTree::context_is_valid(ContextRef context) const
{
    return Compositing::RustFFI::visual_context_tree_context_is_valid(m_rust_tree, context);
}

size_t AccumulatedVisualContextTree::node_count() const
{
    return Compositing::RustFFI::visual_context_tree_node_count(m_rust_tree);
}

size_t AccumulatedVisualContextTree::live_node_count() const
{
    return Compositing::RustFFI::visual_context_tree_live_node_count(m_rust_tree);
}

TransformWithOrigin AccumulatedVisualContextTree::visual_viewport_transform() const
{
    return Compositing::RustFFI::visual_context_tree_visual_viewport_transform(m_rust_tree);
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::with_visual_viewport_transform(TransformWithOrigin const& transform) const
{
    return adopt_rust_handle(Compositing::RustFFI::visual_context_tree_with_visual_viewport_transform(m_rust_tree, transform));
}

bool AccumulatedVisualContextTree::has_visual_animations() const
{
    return Compositing::RustFFI::visual_context_tree_has_visual_animations(m_rust_tree);
}

bool AccumulatedVisualContextTree::has_active_visual_animation_at(i64 monotonic_time_ns) const
{
    return Compositing::RustFFI::visual_context_tree_has_active_visual_animation_at(m_rust_tree, monotonic_time_ns);
}

VisualAnimationSummary AccumulatedVisualContextTree::visual_animation_summary() const
{
    auto summary = Compositing::RustFFI::visual_context_tree_visual_animation_summary(m_rust_tree);
    return {
        .count = summary.count,
        .local_time_at_anchor_ms_of_first = summary.local_time_at_anchor_ms_of_first,
        .share_timing_anchor = summary.share_timing_anchor,
        .targets_are_valid = summary.targets_are_valid,
    };
}

Vector<bool> AccumulatedVisualContextTree::spatial_nodes_in_subtrees_of_transform_animations() const
{
    Vector<bool> in_subtree;
    in_subtree.resize(spatial_node_count());
    Compositing::RustFFI::visual_context_tree_mark_spatial_subtrees_of_transform_animations(m_rust_tree, in_subtree.data(), in_subtree.size());
    return in_subtree;
}

AccumulatedVisualContextTree AccumulatedVisualContextTree::with_visual_animation_samples(i64 monotonic_time_ns) const
{
    return adopt_rust_handle(Compositing::RustFFI::visual_context_tree_with_visual_animation_samples(m_rust_tree, monotonic_time_ns));
}

Optional<float> AccumulatedVisualContextTree::effects_opacity(EffectNodeIndex effect) const
{
    float opacity = 1;
    if (!Compositing::RustFFI::visual_context_tree_effects_opacity(m_rust_tree, effect, &opacity))
        return {};
    return opacity;
}

Optional<Gfx::Color> AccumulatedVisualContextTree::sampled_background_color(EffectNodeIndex effect) const
{
    Gfx::Color color;
    if (!Compositing::RustFFI::visual_context_tree_sampled_background_color(m_rust_tree, effect, &color))
        return {};
    return color;
}

Vector<bool> AccumulatedVisualContextTree::spatial_nodes_in_subtrees_of(ReadonlySpan<SpatialNodeIndex> roots) const
{
    Vector<bool> in_subtree;
    in_subtree.resize(spatial_node_count());
    Compositing::RustFFI::visual_context_tree_mark_spatial_subtrees(m_rust_tree, roots.data(), roots.size(), in_subtree.data(), in_subtree.size());
    return in_subtree;
}

Optional<Gfx::FloatPoint> AccumulatedVisualContextTree::transform_point_for_hit_test(ContextRef context, Gfx::FloatPoint screen_point, ScrollStateSnapshot const& scroll_state, ClipBehavior clip_behavior) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    Gfx::FloatPoint local_point;
    if (!Compositing::RustFFI::visual_context_tree_transform_point_for_hit_test(m_rust_tree, context, screen_point, scroll_offsets.data(), scroll_offsets.size(), clip_behavior == ClipBehavior::Respect, &local_point))
        return {};
    return local_point;
}

Gfx::FloatPoint AccumulatedVisualContextTree::inverse_transform_point(SpatialNodeIndex index, Gfx::FloatPoint screen_point) const
{
    return Compositing::RustFFI::visual_context_tree_inverse_transform_point(m_rust_tree, index, screen_point);
}

Gfx::FloatRect AccumulatedVisualContextTree::transform_rect_to_viewport(SpatialNodeIndex index, Gfx::FloatRect const& source_rect, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    return Compositing::RustFFI::visual_context_tree_transform_rect_to_viewport(m_rust_tree, index, source_rect, scroll_offsets.data(), scroll_offsets.size(), include_visual_viewport_transform == IncludeVisualViewportTransform::Yes);
}

Gfx::FloatPoint AccumulatedVisualContextTree::cumulative_scroll_chain_offset(SpatialNodeIndex index, ScrollStateSnapshot const& scroll_state) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    return Compositing::RustFFI::visual_context_tree_cumulative_scroll_chain_offset(m_rust_tree, index, scroll_offsets.data(), scroll_offsets.size());
}

Gfx::FloatMatrix4x4 AccumulatedVisualContextTree::accumulated_matrix(SpatialNodeIndex index, ScrollStateSnapshot const& scroll_state, IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto scroll_offsets = scroll_state.device_offsets();
    return Compositing::RustFFI::visual_context_tree_accumulated_matrix(m_rust_tree, index, scroll_offsets.data(), scroll_offsets.size(), include_visual_viewport_transform == IncludeVisualViewportTransform::Yes);
}

bool AccumulatedVisualContextTree::effect_is_isolated_by_layer(EffectNodeIndex effect) const
{
    return Compositing::RustFFI::visual_context_tree_effect_is_isolated_by_layer(m_rust_tree, effect);
}

bool AccumulatedVisualContextTree::has_unisolated_destination_reading_effect() const
{
    return Compositing::RustFFI::visual_context_tree_has_unisolated_destination_reading_effect(m_rust_tree);
}

void AccumulatedVisualContextTree::for_each_effects_filter_bytes(Function<void(ReadonlyBytes)> const& visit) const
{
    struct FilterBytesVisitor {
        Function<void(ReadonlyBytes)> const& visit;
    } visitor { visit };
    Compositing::RustFFI::visual_context_tree_for_each_effects_filter_bytes(m_rust_tree, &visitor, [](void* context, u8 const* bytes, size_t size) {
        static_cast<FilterBytesVisitor*>(context)->visit(ReadonlyBytes { bytes, size });
    });
}

void resolve_sticky_offsets(AccumulatedVisualContextTree const& tree, ScrollStateSnapshot& scroll_state)
{
    auto scroll_offsets = scroll_state.device_offsets();
    Compositing::RustFFI::visual_context_tree_resolve_sticky_offsets(
        tree.rust_handle(), scroll_offsets.data(), scroll_offsets.size(),
        &scroll_state, [](void* sink, SpatialNodeIndex index, Gfx::FloatPoint offset) {
            static_cast<ScrollStateSnapshot*>(sink)->set_device_offset_for_index(index, offset);
        });
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::AccumulatedVisualContextTree const& tree)
{
    TRY(encoder.encode(tree.serialize_to_bytes()));
    return {};
}

// The bytes carry the tree's animations, which the decoder validates against the nodes they name.
template<>
ErrorOr<Compositing::AccumulatedVisualContextTree> decode(Decoder& decoder)
{
    auto bytes = TRY(decoder.decode<ByteBuffer>());
    return Compositing::AccumulatedVisualContextTree::from_serialized_bytes(bytes);
}

}
