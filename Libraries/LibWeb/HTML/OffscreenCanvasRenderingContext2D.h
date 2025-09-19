/*
 * Copyright (c) 2025, Ladybird contributors
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
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
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
#include <LibWeb/HTML/Canvas/CanvasSettings.h>
#include <LibWeb/HTML/Canvas/CanvasShadowStyles.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/Canvas/CanvasText.h>
#include <LibWeb/HTML/Canvas/CanvasTextDrawingStyles.h>
#include <LibWeb/HTML/Canvas/CanvasTransform.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class OffscreenCanvasRenderingContext2D : public Bindings::PlatformObject
    , public CanvasState
    , public CanvasTransform<OffscreenCanvasRenderingContext2D>
    , public CanvasFillStrokeStyles<OffscreenCanvasRenderingContext2D>
    , public CanvasShadowStyles<OffscreenCanvasRenderingContext2D>
    , public CanvasFilters
    , public CanvasRect
    , public CanvasDrawPath
    , public CanvasText
    , public CanvasDrawImage
    , public CanvasImageData
    , public CanvasImageSmoothing
    , public CanvasCompositing
    , public CanvasSettings
    , public CanvasPathDrawingStyles<OffscreenCanvasRenderingContext2D>
    , public CanvasTextDrawingStyles<OffscreenCanvasRenderingContext2D, OffscreenCanvas>
    , public CanvasPath

{
    WEB_PLATFORM_OBJECT(OffscreenCanvasRenderingContext2D, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

public:
    [[nodiscard]] static JS::ThrowCompletionOr<GC::Ref<OffscreenCanvasRenderingContext2D>> create(JS::Realm&, OffscreenCanvas&, JS::Value);
    virtual ~OffscreenCanvasRenderingContext2D() override;

    GC::Ref<OffscreenCanvas> canvas();

    virtual void fill_rect(float x, float y, float width, float height) override;
    virtual void stroke_rect(float x, float y, float width, float height) override;
    virtual void clear_rect(float x, float y, float width, float height) override;

    virtual WebIDL::ExceptionOr<void> draw_image_internal(CanvasImageSource const&, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height) override;

    virtual void begin_path() override;
    virtual void stroke() override;
    virtual void stroke(Path2D const& path) override;

    virtual void fill_text(Utf16String const&, float x, float y, Optional<double> max_width) override;
    virtual void stroke_text(Utf16String const&, float x, float y, Optional<double> max_width) override;

    virtual void fill(StringView fill_rule) override;
    virtual void fill(Path2D& path, StringView fill_rule) override;

    virtual WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(int width, int height, Optional<ImageDataSettings> const& settings = {}) const override;
    virtual WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(ImageData const& image_data) const override;
    virtual WebIDL::ExceptionOr<GC::Ptr<ImageData>> get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings = {}) const override;
    virtual void put_image_data(ImageData&, float x, float y) override;

    virtual void reset_to_default_state() override;

    virtual CanvasRenderingContext2DSettings get_context_attributes() const override { return m_context_attributes; }

    virtual GC::Ref<TextMetrics> measure_text(Utf16String const&) override;

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

    OffscreenCanvas& canvas_element();
    OffscreenCanvas const& canvas_element() const;

    [[nodiscard]] Gfx::Painter* painter();

    void set_size(Gfx::IntSize const&);

private:
    explicit OffscreenCanvasRenderingContext2D(JS::Realm&, OffscreenCanvas&, CanvasRenderingContext2DSettings);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual Gfx::Painter* painter_for_canvas_state() override
    {
        dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::painter_for_canvas_state()");
        return nullptr;
    }
    virtual Gfx::Path& path_for_canvas_state() override { return path(); }

    GC::Ref<OffscreenCanvas> m_canvas;
    Gfx::IntSize m_size;
    CanvasRenderingContext2DSettings m_context_attributes;
};

}
