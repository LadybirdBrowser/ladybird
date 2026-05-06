/*
 * Copyright (c) 2023-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/SegmentedVector.h>
#include <AK/Span.h>
#include <AK/StringBuilder.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Forward.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibPaintServer/Compositor/DrawList.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ShouldAntiAlias.h>

#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Types.h>

namespace Web::Painting {

struct PaintCommandEncodingContext {
    Function<Optional<PaintServer::ResourceID>(Gfx::DecodedImageFrame const&, Optional<u64> stable_bitmap_id)> ensure_bitmap_resource;
    Function<PaintServer::ResourceID(Gfx::Font const&)> ensure_font_resource;
    double device_pixels_per_css_pixel { 1.0 };
};

struct VisualContextRef {
    AccumulatedVisualContextTree const* tree { nullptr };
    VisualContextIndex index;

    friend bool operator==(VisualContextRef const&, VisualContextRef const&) = default;

    explicit operator bool() const { return tree != nullptr && index.value() != 0; }
};

using CommandByteWriter = Function<ErrorOr<void>(ReadonlyBytes)>;

class DisplayList : public AtomicRefCounted<DisplayList> {
public:
    enum class ItemKind : u8 {
        DrawCommand,
        NestedDisplayList,
    };

    struct NestedDisplayListEntry {
        RefPtr<DisplayList> display_list;
        Gfx::IntRect rect;
    };

    struct CommandListItem {
        VisualContextIndex context_index {};
        ItemKind kind { ItemKind::DrawCommand };
        u32 command_offset { 0 };
        u32 command_size { 0 };
        u32 nested_display_list_index { 0 };
    };

    static NonnullRefPtr<DisplayList> create(NonnullRefPtr<AccumulatedVisualContextTree const> visual_context_tree)
    {
        return adopt_ref(*new DisplayList(move(visual_context_tree)));
    }

    bool append_draw_command(ReadonlyBytes, VisualContextIndex context_index);
    bool append_nested_display_list(RefPtr<DisplayList>, Gfx::IntRect, VisualContextIndex context_index);
    void append_external_content_source(NonnullRefPtr<ExternalContentSource>);

    AccumulatedVisualContextTree const& visual_context_tree() const { return *m_visual_context_tree; }

    auto const& commands() const { return m_commands; }
    ByteBuffer const& command_stream() const { return m_command_stream; }
    Vector<NestedDisplayListEntry> const& nested_display_lists() const { return m_nested_display_lists; }
    Vector<NonnullRefPtr<ExternalContentSource>> const& external_content_sources() const { return m_external_content_sources; }

    ReadonlyBytes bytes_for(CommandListItem const&) const;
    NestedDisplayListEntry const& nested_display_list_for(CommandListItem const&) const;

    Optional<Gfx::IntSize> viewport_size() const { return m_viewport_size; }
    void set_viewport_size(Gfx::IntSize viewport_size) { m_viewport_size = viewport_size; }
    Optional<FlatPtr> source_context_namespace_id() const { return m_source_context_namespace_id; }
    void set_source_context_namespace_id(Optional<FlatPtr> source_context_namespace_id) { m_source_context_namespace_id = source_context_namespace_id; }
    HashMap<FlatPtr, ScrollStateSnapshot> const& scroll_state_snapshots_by_source_context_namespace_id() const { return m_scroll_state_snapshots_by_source_context_namespace_id; }
    void set_scroll_state_snapshots_by_source_context_namespace_id(HashMap<FlatPtr, ScrollStateSnapshot> scroll_state_snapshots_by_source_context_namespace_id) { m_scroll_state_snapshots_by_source_context_namespace_id = move(scroll_state_snapshots_by_source_context_namespace_id); }

private:
    explicit DisplayList(NonnullRefPtr<AccumulatedVisualContextTree const> visual_context_tree)
        : m_visual_context_tree(move(visual_context_tree))
    {
    }

    NonnullRefPtr<AccumulatedVisualContextTree const> const m_visual_context_tree;
    ByteBuffer m_command_stream;
    AK::SegmentedVector<CommandListItem, 512> m_commands;
    Vector<NestedDisplayListEntry> m_nested_display_lists;
    Vector<NonnullRefPtr<ExternalContentSource>> m_external_content_sources;
    Optional<Gfx::IntSize> m_viewport_size;
    Optional<FlatPtr> m_source_context_namespace_id;
    HashMap<FlatPtr, ScrollStateSnapshot> m_scroll_state_snapshots_by_source_context_namespace_id;
};

struct CachedDisplayListCommands {
    enum class ItemKind : u8 {
        DrawCommand,
        NestedDisplayList,
    };

    struct NestedDisplayListEntry {
        RefPtr<DisplayList> display_list;
        Gfx::IntRect rect;
    };

    struct Item {
        ItemKind kind { ItemKind::DrawCommand };
        u32 command_offset { 0 };
        u32 command_size { 0 };
        u32 nested_display_list_index { 0 };
    };

    Vector<Item> items;
    PaintServer::DrawList draw_commands;
    Vector<NestedDisplayListEntry> nested_display_lists;
    Vector<NonnullRefPtr<ExternalContentSource>> external_content_sources;
};

class DisplayListRecorder {
    AK_MAKE_NONCOPYABLE(DisplayListRecorder);
    AK_MAKE_NONMOVABLE(DisplayListRecorder);

public:
    class CommandEncoder;

    ~DisplayListRecorder();

    void fill_rect(Gfx::IntRect const& rect, Color color);
    void fill_rect_transparent(Gfx::IntRect const& rect);

    struct FillPathParams {
        Gfx::Path path;
        float opacity = 1.0f;
        PaintStyleOrColor paint_style_or_color;
        Gfx::WindingRule winding_rule = Gfx::WindingRule::EvenOdd;
        ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };
    };
    void fill_path(FillPathParams params);

    struct StrokePathParams {
        Gfx::Path::CapStyle cap_style;
        Gfx::Path::JoinStyle join_style;
        float miter_limit;
        Vector<float> dash_array;
        float dash_offset;
        Gfx::Path path;
        float opacity = 1.0f;
        PaintStyleOrColor paint_style_or_color;
        float thickness;
        ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };
    };
    void stroke_path(StrokePathParams);

    void draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness);

    void fill_ellipse(Gfx::IntRect const& a_rect, Color color);

    void fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data);
    void fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position);
    void fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size);

    void draw_rect(Gfx::IntRect const& rect, Color color, bool rough = false);

    void draw_scaled_decoded_image_frame(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::DecodedImageFrame const& frame, Gfx::ScalingMode scaling_mode = Gfx::ScalingMode::NearestNeighbor);
    void draw_external_content(Gfx::IntRect const& dst_rect, NonnullRefPtr<ExternalContentSource>, Gfx::ScalingMode scaling_mode = Gfx::ScalingMode::NearestNeighbor);
    void draw_external_content(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, NonnullRefPtr<ExternalContentSource>, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y);

    void draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness = 1, Gfx::LineStyle style = Gfx::LineStyle::Solid, Color alternate_color = Color::Transparent);

    void draw_text(Gfx::IntRect const&, Utf16String const&, Gfx::Font const&, Gfx::TextAlignment, Color);

    // Streamlined text drawing routine that does no wrapping/elision/alignment.
    void draw_glyph_run(Gfx::FloatPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale, Gfx::Orientation);

    void add_clip_rect(Gfx::IntRect const& rect);

    void translate(Gfx::IntPoint delta);

    void set_accumulated_visual_context(VisualContextIndex index) { m_accumulated_visual_context_index = index; }
    VisualContextIndex accumulated_visual_context() const { return m_accumulated_visual_context_index; }
    AccumulatedVisualContextTree const& visual_context_tree() const;

    void replay_cached_commands(CachedDisplayListCommands const& commands);

    class CommandCapture {
        AK_MAKE_NONCOPYABLE(CommandCapture);

    public:
        CommandCapture(CommandCapture&& other)
            : m_recorder(exchange(other.m_recorder, nullptr))
        {
        }
        ~CommandCapture();
        CachedDisplayListCommands take();

    private:
        friend class DisplayListRecorder;
        explicit CommandCapture(DisplayListRecorder&);
        DisplayListRecorder* m_recorder { nullptr };
    };

    CommandCapture begin_command_capture();

    void save();
    void save_layer();
    void restore();

    void paint_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect);

    void add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip);

    struct MaskInfo {
        RefPtr<DisplayList> display_list;
        Gfx::IntRect rect;
        Gfx::MaskKind kind;
    };
    void begin_masks(ReadonlySpan<MaskInfo>);
    void end_masks(ReadonlySpan<MaskInfo>);

    void apply_backdrop_filter(Gfx::IntRect const& backdrop_region, CornerRadii const& corner_radii, Gfx::Filter const& backdrop_filter);

    void paint_outer_box_shadow(Gfx::Color color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const& content_corner_radii, Gfx::IntRect shadow_rect, CornerRadii const& shadow_corner_radii);
    void paint_inner_box_shadow(Gfx::Color color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const& content_corner_radii, Gfx::IntRect outer_shadow_rect, Gfx::IntRect inner_shadow_rect, CornerRadii const& inner_shadow_corner_radii);
    void paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const&, double glyph_run_scale, Color color, Gfx::FloatPoint draw_location);

    void fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, CornerRadii const&);
    void fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius);
    void fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius);

    void paint_scrollbar(ScrollFrameIndex scroll_frame_index, Gfx::IntRect scrollbar_rect, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, double scroll_size, Color thumb_color, Color track_color, bool vertical);

    void apply_effects(float opacity = 1.0f, Gfx::CompositingAndBlendingOperator = Gfx::CompositingAndBlendingOperator::Normal, Optional<Gfx::Filter> filter = {}, Optional<Gfx::MaskKind> mask_kind = {});
    explicit DisplayListRecorder(DisplayList& display_list, PaintCommandEncodingContext&);
    explicit DisplayListRecorder(DisplayList& display_list, DisplayListRecorder& parent);

    int m_save_nesting_level { 0 };

private:
    bool append_encoded_command(Function<ErrorOr<void>(CommandEncoder&)>&&);
    bool append_serialized_command(ReadonlyBytes command);
    void append_nested_display_list_metadata(RefPtr<DisplayList>, Gfx::IntRect);
    void append_external_content_source(NonnullRefPtr<ExternalContentSource>);
    Optional<FlatPtr> current_source_context_namespace_id() const;
    void end_capture();

    VisualContextIndex m_accumulated_visual_context_index;
    Vector<size_t> m_push_sc_index_stack;
    DisplayList* m_display_list { nullptr };
    PaintCommandEncodingContext* m_encoding_context { nullptr };
    ByteBuffer m_pending_command_bytes;
    Function<ErrorOr<void>(ReadonlyBytes)> m_command_writer;
    OwnPtr<CommandEncoder> m_command_encoder;
    bool m_is_capturing { false };
    CachedDisplayListCommands m_captured_commands;
};

class DisplayListRecorderStateSaver {
public:
    explicit DisplayListRecorderStateSaver(DisplayListRecorder& recorder)
        : m_recorder(recorder)
    {
        m_recorder.save();
    }

    ~DisplayListRecorderStateSaver()
    {
        m_recorder.restore();
    }

private:
    DisplayListRecorder& m_recorder;
};

AccumulatedVisualContextNode const* context_node(VisualContextRef const& context);
ErrorOr<Vector<VisualContextRef>> collect_context_nodes_root_to_leaf(VisualContextRef context);
size_t shared_context_prefix_length(ReadonlySpan<VisualContextRef const> left, ReadonlySpan<VisualContextRef const> right);
AccumulatedVisualContextTree const* first_context_tree(ReadonlySpan<VisualContextRef const> context_nodes);
void verify_nested_display_list_tree_ownership(DisplayList const& display_list, ReadonlySpan<VisualContextRef const> inherited_context_nodes, VisualContextRef parent_context, bool requires_shared_tree);
ErrorOr<Vector<VisualContextRef>> extend_effective_context_nodes(ReadonlySpan<VisualContextRef const> inherited_context_nodes, VisualContextRef local_context);
PaintServer::WireCornerRadii to_wire_corner_radii(CornerRadii const& corner_radii);

ErrorOr<void> append_context_setup_commands(CommandByteWriter&, AccumulatedVisualContextTree const&, VisualContextIndex);
ErrorOr<void> append_apply_effects_payload(CommandByteWriter&, PaintCommandEncodingContext const&, Optional<Gfx::Filter> const&, float opacity, Gfx::CompositingAndBlendingOperator, Optional<Gfx::MaskKind>);

int command_nesting_level_change(PaintServer::DrawCommandView const&);
StringView dump_command_name(PaintServer::DrawCommandView const&);
void dump_command(StringBuilder&, PaintServer::DrawCommandView const&);

}

namespace AK {

template<>
struct Traits<Web::Painting::VisualContextRef> : public DefaultTraits<Web::Painting::VisualContextRef> {
    static unsigned hash(Web::Painting::VisualContextRef const& context)
    {
        return pair_int_hash(Traits<FlatPtr>::hash(reinterpret_cast<FlatPtr>(context.tree)), Traits<Web::Painting::VisualContextIndex>::hash(context.index));
    }
};

}
