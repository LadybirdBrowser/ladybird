/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/RustFFI.h>
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

size_t AccumulatedVisualContextTree::node_count() const
{
    return Compositing::RustFFI::visual_context_tree_node_count(m_rust_tree);
}

size_t AccumulatedVisualContextTree::live_node_count() const
{
    return Compositing::RustFFI::visual_context_tree_live_node_count(m_rust_tree);
}

bool AccumulatedVisualContextTree::has_visual_animations() const
{
    return Compositing::RustFFI::visual_context_tree_has_visual_animations(m_rust_tree);
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

AccumulatedVisualContextTree AccumulatedVisualContextTree::with_visual_animation_samples(i64 monotonic_time_ns) const
{
    return adopt_rust_handle(Compositing::RustFFI::visual_context_tree_with_visual_animation_samples(m_rust_tree, monotonic_time_ns));
}

Optional<Gfx::Color> AccumulatedVisualContextTree::sampled_background_color(EffectNodeIndex effect) const
{
    Gfx::Color color;
    if (!Compositing::RustFFI::visual_context_tree_sampled_background_color(m_rust_tree, effect, &color))
        return {};
    return color;
}

bool AccumulatedVisualContextTree::translations_snap_to_device_pixels(SpatialNodeIndex spatial_node_index) const
{
    return Compositing::RustFFI::visual_context_tree_translations_snap_to_device_pixels(m_rust_tree, spatial_node_index);
}

void AccumulatedVisualContextTree::for_each_effects_filter_image_frame_id(Function<void(u64)> const& visit) const
{
    struct ImageFrameIdVisitor {
        Function<void(u64)> const& visit;
    } visitor { visit };
    Compositing::RustFFI::visual_context_tree_for_each_effects_filter_image_frame_id(m_rust_tree, &visitor, [](void* context, u64 image_frame_id) {
        static_cast<ImageFrameIdVisitor*>(context)->visit(image_frame_id);
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
