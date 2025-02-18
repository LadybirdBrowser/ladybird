/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Path.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/Canvas/CanvasCompositing.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/Canvas/CanvasDrawPath.h>
#include <LibWeb/HTML/Canvas/CanvasFillStrokeStyles.h>
#include <LibWeb/HTML/Canvas/CanvasFilters.h>
#include <LibWeb/HTML/Canvas/CanvasImageData.h>
#include <LibWeb/HTML/Canvas/CanvasImageSmoothing.h>
#include <LibWeb/HTML/Canvas/CanvasPath.h>
#include <LibWeb/HTML/Canvas/CanvasPathDrawingStyles.h>
#include <LibWeb/HTML/Canvas/CanvasRect.h>
#include <LibWeb/HTML/Canvas/CanvasShadowStyles.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/Canvas/CanvasText.h>
#include <LibWeb/HTML/Canvas/CanvasTextDrawingStyles.h>
#include <LibWeb/HTML/Canvas/CanvasTransform.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class CanvasRenderingContext2D
    : public Bindings::PlatformObject
    , public CanvasPath
    , public CanvasState
    , public CanvasTransform<CanvasRenderingContext2D>
    , public CanvasFillStrokeStyles<CanvasRenderingContext2D>
    , public CanvasShadowStyles<CanvasRenderingContext2D>
    , public CanvasFilters
    , public CanvasRect
    , public CanvasDrawPath
    , public CanvasText
    , public CanvasDrawImage
    , public CanvasImageData
    , public CanvasImageSmoothing
    , public CanvasCompositing
    , public CanvasPathDrawingStyles<CanvasRenderingContext2D>
    , public CanvasTextDrawingStyles<CanvasRenderingContext2D> {

    WEB_PLATFORM_OBJECT(CanvasRenderingContext2D, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CanvasRenderingContext2D);

public:
    [[nodiscard]] static GC::Ref<CanvasRenderingContext2D> create(JS::Realm&, HTMLCanvasElement&);
    virtual ~CanvasRenderingContext2D() override;

    virtual void fill_rect(float x, float y, float width, float height) override;
    virtual void stroke_rect(float x, float y, float width, float height) override;
    virtual void clear_rect(float x, float y, float width, float height) override;

    virtual WebIDL::ExceptionOr<void> draw_image_internal(CanvasImageSource const&, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height) override;

    virtual void begin_path() override;
    virtual void stroke() override;
    virtual void stroke(Path2D const& path) override;

    virtual void fill_text(StringView, float x, float y, Optional<double> max_width) override;
    virtual void stroke_text(StringView, float x, float y, Optional<double> max_width) override;

    virtual void fill(StringView fill_rule) override;
    virtual void fill(Path2D& path, StringView fill_rule) override;

    virtual WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(int width, int height, Optional<ImageDataSettings> const& settings = {}) const override;
    virtual WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(ImageData const& image_data) const override;
    virtual WebIDL::ExceptionOr<GC::Ptr<ImageData>> get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings = {}) const override;
    virtual void put_image_data(ImageData const&, float x, float y) override;

    virtual void reset_to_default_state() override;

    GC::Ref<HTMLCanvasElement> canvas_for_binding() const;

    virtual GC::Ref<TextMetrics> measure_text(StringView text) override;

    virtual void clip(StringView fill_rule) override;
    virtual void clip(Path2D& path, StringView fill_rule) override;

    virtual bool is_point_in_path(double x, double y, StringView fill_rule) override;
    virtual bool is_point_in_path(Path2D const& path, double x, double y, StringView fill_rule) override;

    virtual bool image_smoothing_enabled() const override;
    virtual void set_image_smoothing_enabled(bool) override;
    virtual Bindings::ImageSmoothingQuality image_smoothing_quality() const override;
    virtual void set_image_smoothing_quality(Bindings::ImageSmoothingQuality) override;

    virtual float global_alpha() const override;
    virtual void set_global_alpha(float) override;

    virtual String global_composite_operation() const override;
    virtual void set_global_composite_operation(String) override;

    virtual String filter() const override;
    virtual void set_filter(String) override;

    virtual float shadow_offset_x() const override;
    virtual void set_shadow_offset_x(float) override;
    virtual float shadow_offset_y() const override;
    virtual void set_shadow_offset_y(float) override;
    virtual float shadow_blur() const override;
    virtual void set_shadow_blur(float) override;
    virtual String shadow_color() const override;
    virtual void set_shadow_color(String) override;

    HTMLCanvasElement& canvas_element();
    HTMLCanvasElement const& canvas_element() const;

    [[nodiscard]] Gfx::Painter* painter();

    void set_size(Gfx::IntSize const&);

    RefPtr<Gfx::PaintingSurface> surface() { return m_surface; }
    void allocate_painting_surface_if_needed();

private:
    explicit CanvasRenderingContext2D(JS::Realm&, HTMLCanvasElement&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual Gfx::Painter* painter_for_canvas_state() override { return painter(); }
    virtual Gfx::Path& path_for_canvas_state() override { return path(); }

    struct PreparedText {
        RefPtr<Gfx::GlyphRun> glyph_run;
        Gfx::TextAlignment physical_alignment;
        Gfx::IntRect bounding_box;
    };

    void did_draw(Gfx::FloatRect const&);

    RefPtr<Gfx::Font const> current_font();

    PreparedText prepare_text(ByteString const& text, float max_width = INFINITY);

    [[nodiscard]] Gfx::Path rect_path(float x, float y, float width, float height);
    [[nodiscard]] Gfx::Path text_path(StringView text, float x, float y, Optional<double> max_width);

    void stroke_internal(Gfx::Path const&);
    void fill_internal(Gfx::Path const&, Gfx::WindingRule);
    void clip_internal(Gfx::Path&, Gfx::WindingRule);
    void paint_shadow_for_fill_internal(Gfx::Path const&, Gfx::WindingRule);
    void paint_shadow_for_stroke_internal(Gfx::Path const&);

    GC::Ref<HTMLCanvasElement> m_element;
    OwnPtr<Gfx::Painter> m_painter;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    bool m_origin_clean { true };

    Gfx::IntSize m_size;
    RefPtr<Gfx::PaintingSurface> m_surface;
};

enum class CanvasImageSourceUsability {
    Bad,
    Good,
};

WebIDL::ExceptionOr<CanvasImageSourceUsability> check_usability_of_image(CanvasImageSource const&);
bool image_is_not_origin_clean(CanvasImageSource const&);

}
