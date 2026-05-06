/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Resource/Resource.h>
#include <LibPaintServer/Compositor/DrawList.h>

namespace Gfx {

class PaintingSurface;

}

namespace PaintServer {

struct DrawCommandImageResource {
    ResourceID image_resource_id { 0 };
    ImageID image_id { 0 };
};

class CanvasPainter final : public Gfx::Painter {
public:
    using ImageResourceResolver = Function<Optional<DrawCommandImageResource>(Gfx::DecodedImageFrame const&)>;

    explicit CanvasPainter(ImageResourceResolver = {}, Gfx::PaintingSurface* shadow_surface = nullptr);
    virtual ~CanvasPainter() override;

    DrawList const& draw_list() const { return m_draw_list; }
    DrawList take_draw_list();
    void clear_draw_list();
    bool has_error() const { return m_has_error; }

    virtual void clear_rect(Gfx::FloatRect const&, Gfx::Color) override;
    virtual void fill_rect(Gfx::FloatRect const&, Gfx::Color) override;
    virtual void draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::DecodedImageFrame const& source, Gfx::IntRect const& src_rect, Gfx::ScalingMode, Optional<Gfx::Filter> filters, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) override;
    virtual void stroke_path(Gfx::Path const&, Gfx::Color, float thickness) override;
    virtual void stroke_path(Gfx::Path const&, Gfx::Color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle, Gfx::Path::JoinStyle, float miter_limit, Vector<float> const& dash_array, float dash_offset) override;
    virtual void stroke_path(Gfx::Path const&, Gfx::PaintStyle const&, Optional<Gfx::Filter>, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) override;
    virtual void stroke_path(Gfx::Path const&, Gfx::PaintStyle const&, Optional<Gfx::Filter>, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle const&, Gfx::Path::JoinStyle const&, float miter_limit, Vector<float> const&, float dash_offset) override;
    virtual void fill_path(Gfx::Path const&, Gfx::Color, Gfx::WindingRule) override;
    virtual void fill_path(Gfx::Path const&, Gfx::Color, Gfx::WindingRule, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator) override;
    virtual void fill_path(Gfx::Path const&, Gfx::PaintStyle const&, Optional<Gfx::Filter>, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule) override;
    virtual void set_transform(Gfx::AffineTransform const&) override;
    virtual void save() override;
    virtual void restore() override;
    virtual void clip(Gfx::Path const&, Gfx::WindingRule) override;
    virtual void reset() override;

private:
    void append_command(ReadonlyBytes);
    void append_path_command(Gfx::Path const&, Gfx::Color, Optional<Gfx::Filter> const&, float opacity, Gfx::CompositingAndBlendingOperator, Gfx::WindingRule, float blur_radius = 0.0f);
    void append_path_command(Gfx::Path const&, Gfx::PaintStyle const&, Optional<Gfx::Filter> const&, float opacity, Gfx::CompositingAndBlendingOperator, Gfx::WindingRule);
    void append_stroke_path_command(Gfx::Path const&, Gfx::Color, Optional<Gfx::Filter> const&, float opacity, float thickness, Gfx::CompositingAndBlendingOperator, Gfx::Path::CapStyle, Gfx::Path::JoinStyle, float miter_limit, Vector<float> const&, float dash_offset, float blur_radius = 0.0f);
    void append_stroke_path_command(Gfx::Path const&, Gfx::PaintStyle const&, Optional<Gfx::Filter> const&, float opacity, float thickness, Gfx::CompositingAndBlendingOperator, Gfx::Path::CapStyle, Gfx::Path::JoinStyle, float miter_limit, Vector<float> const&, float dash_offset);
    bool encode_paint_style(Gfx::PaintStyle const&, u8& paint_style_type, Gfx::Color&, ByteBuffer& paint_style_payload) const;
    Optional<ByteBuffer> serialize_filter_bytes(Optional<Gfx::Filter> const&) const;

    DrawList m_draw_list;
    ImageResourceResolver m_resolve_image_resource;
    OwnPtr<Gfx::Painter> m_shadow_painter;
    bool m_has_error { false };
    size_t m_save_depth { 0 };
};

}
