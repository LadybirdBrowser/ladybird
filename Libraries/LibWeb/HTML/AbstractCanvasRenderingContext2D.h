/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Path.h>
#include <LibGfx/TextLayout.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/Canvas/CanvasCompositing.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/Canvas/CanvasDrawPath.h>
#include <LibWeb/HTML/Canvas/CanvasFillStrokeStyles.h>
#include <LibWeb/HTML/Canvas/CanvasFilters.h>
#include <LibWeb/HTML/Canvas/CanvasImageData.h>
#include <LibWeb/HTML/Canvas/CanvasImageSmoothing.h>
#include <LibWeb/HTML/Canvas/CanvasImageSource.h>
#include <LibWeb/HTML/Canvas/CanvasPath.h>
#include <LibWeb/HTML/Canvas/CanvasPathDrawingStyles.h>
#include <LibWeb/HTML/Canvas/CanvasRect.h>
#include <LibWeb/HTML/Canvas/CanvasSettings.h>
#include <LibWeb/HTML/Canvas/CanvasShadowStyles.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/Canvas/CanvasText.h>
#include <LibWeb/HTML/Canvas/CanvasTextDrawingStyles.h>
#include <LibWeb/HTML/Canvas/CanvasTransform.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/Node.h>

#pragma once

namespace Web::HTML {

template<typename FinalContext, typename FinalElement>
class AbstractCanvasRenderingContext2D : public CanvasPath
    , public CanvasState
    , public CanvasTransform<FinalContext>
    , public CanvasFillStrokeStyles<FinalContext>
    , public CanvasShadowStyles<FinalContext>
    , public CanvasFilters
    , public CanvasRect
    , public CanvasDrawPath
    , public CanvasText
    , public CanvasDrawImage
    , public CanvasImageData
    , public CanvasImageSmoothing
    , public CanvasCompositing
    , public CanvasSettings
    , public CanvasPathDrawingStyles<FinalContext>
    , public CanvasTextDrawingStyles<FinalContext, FinalElement>

{

    GC_DECLARE_ALLOCATOR(AbstractCanvasRenderingContext2D);

public:
    virtual Gfx::Path& path() override { return CanvasPath::path(); }

    virtual CanvasRenderingContext2DSettings context_attributes() override { return m_context_attributes; }

    virtual CanvasRenderingContext2DSettings get_context_attributes() const override { return m_context_attributes; }

    void paint_shadow_for_fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule);
    void paint_shadow_for_stroke_internal(Gfx::Path const& path, Gfx::Path::CapStyle line_cap, Gfx::Path::JoinStyle line_join, Vector<float> const& dash_array);

    virtual void stroke_internal(Gfx::Path const& path) override;
    virtual void fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule) override;
    virtual void clip_internal(Gfx::Path& path, Gfx::WindingRule winding_rule) override;

    virtual void set_font(StringView font) override;

    virtual void set_shadow_color(String color) override = 0;

    FinalElement& canvas_element()
    {
        return *m_element;
    }

    FinalElement const& canvas_element() const
    {
        return *m_element;
    }

    GC::Ref<FinalElement> canvas_for_binding() const
    {
        return *m_element;
    }

    [[nodiscard]] virtual Gfx::Painter* painter() override = 0;

    void set_size(Gfx::IntSize const& size);

    // https://html.spec.whatwg.org/multipage/canvas.html#reset-the-rendering-context-to-its-default-state
    virtual void reset_to_default_state() override;

    virtual void allocate_painting_surface_if_needed() = 0;
    virtual RefPtr<Gfx::PaintingSurface> surface() const override { return m_surface; }

protected:
    explicit AbstractCanvasRenderingContext2D(Bindings::PlatformObject& platform_object, FinalElement& element, CanvasRenderingContext2DSettings context_attributes)
        : CanvasPath(platform_object, *this)
        , m_element(element)
        , m_size(element.bitmap_size_for_canvas())
        , m_context_attributes(move(context_attributes))
    {
    }

    virtual ~AbstractCanvasRenderingContext2D() = default;

    virtual Gfx::Painter* painter_for_canvas_state() override { return painter(); }
    virtual Gfx::Path& path_for_canvas_state() override { return path(); }

    struct PreparedText {
        Vector<NonnullRefPtr<Gfx::GlyphRun>> glyph_runs;
        Gfx::TextAlignment physical_alignment;
        Gfx::FloatRect bounding_box;
    };

    virtual void did_draw(Gfx::FloatRect const&) override = 0;

    GC::Ref<FinalElement> m_element;
    OwnPtr<Gfx::Painter> m_painter;

    // https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
    virtual Gfx::Color clear_color() const override
    {
        return m_context_attributes.alpha ? Gfx::Color::Transparent : Gfx::Color::Black;
    }

    virtual bool origin_clean() const override
    {
        return m_origin_clean;
    }

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    bool m_origin_clean { true };

    Gfx::IntSize m_size;
    RefPtr<Gfx::PaintingSurface> m_surface;
    CanvasRenderingContext2DSettings m_context_attributes;
};

}
