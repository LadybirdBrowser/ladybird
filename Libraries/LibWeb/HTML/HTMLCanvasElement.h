/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibGC/Function.h>
#include <LibGfx/Forward.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/HTML/Canvas/CanvasSettings.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebGL/WebGLContextAttributes.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

class HTMLCanvasElement final
    : public HTMLElement
    , public CanvasHost {
    WEB_WRAPPABLE(HTMLCanvasElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLCanvasElement);

public:
    using RenderingContext = Variant<GC::Ref<CanvasRenderingContext2D>, GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>, Empty>;

    virtual ~HTMLCanvasElement() override;

    // ^CanvasHost
    virtual Gfx::IntSize bitmap_size_for_canvas() const override;
    virtual Canvas2DContextBase* canvas_2d_context() const override;
    virtual WebGL::WebGLRenderingContextBase* canvas_webgl_context() const override;
    virtual Page& canvas_page() override;
    virtual DOM::EventTarget& canvas_event_target() override { return *this; }
    virtual JS::Object& canvas_relevant_global_object() const override;
    virtual GC::Ptr<Bindings::Wrappable> canvas_relevant_global_impl() const override;
    virtual void did_change_canvas_content() override;
    virtual void did_create_canvas_backing_storage() override;
    virtual CSS::FontComputer& canvas_font_computer() override;
    virtual CSS::ComputationContext canvas_font_computation_context() override;
    virtual CSS::ColorResolutionStyle canvas_color_resolution_style() override;
    virtual CSSPixelRect canvas_viewport_rect() const override;
    virtual RefPtr<Gfx::Bitmap> get_bitmap_from_surface() override;
    virtual bool is_origin_clean() const override;

    JS::ThrowCompletionOr<RenderingContext> get_context(Utf16View type, JS::Value options);
    enum class HasOrCreatedContext {
        No,
        Yes,
    };
    HasOrCreatedContext create_2d_context(CanvasRenderingContext2DSettings);
    HasOrCreatedContext create_webgl_context(WebGL::WebGLContextAttributes);
    HasOrCreatedContext create_webgl2_context(WebGL::WebGLContextAttributes);
    RenderingContext const& context() const { return m_context; }

    WebIDL::UnsignedLong width() const;
    WebIDL::UnsignedLong height() const;

    WebIDL::ExceptionOr<void> set_width(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<void> set_height(WebIDL::UnsignedLong);

    bool is_placeholder() const { return m_is_placeholder; }

    WebIDL::ExceptionOr<GC::Ref<OffscreenCanvas>> transfer_control_to_offscreen();
    WEB_API static void placeholder_frame_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean);
    virtual void attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    WebIDL::ExceptionOr<Utf16String> to_data_url(Utf16View type, Optional<JS::Value> quality);
    WebIDL::ExceptionOr<void> to_blob(GC::Ref<WebIDL::CallbackType> callback, Utf16View type, Optional<JS::Value> quality);

    void prepare_for_compositing();
    void notify_compositor_backing_storage_lost();
    void set_canvas_content_dirty();
    GC::Ptr<HTML::CanvasRenderingContext2D> canvas_rendering_context_2d() const
    {
        if (auto const* context = m_context.get_pointer<GC::Ref<HTML::CanvasRenderingContext2D>>())
            return *context;
        return nullptr;
    }

    Optional<Compositing::CanvasId> canvas_id() const;

    u64 content_generation() const { return m_content_generation; }

    Optional<Gfx::IntSize> canvas_surface_content_size() const;

    void ensure_backing_storage();

    void notify_compositor_connection_lost();

private:
    HTMLCanvasElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize_element() override;
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_html_canvas_element() const override { return true; }

    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;

    virtual CSS::ElementBoxKind box_kind() const override;

    template<typename ContextType>
    JS::ThrowCompletionOr<HasOrCreatedContext> create_webgl_context(JS::Value options);
    void reset_context_to_default_state();
    void notify_context_about_canvas_size_change();
    void did_commit_placeholder_frame(Gfx::IntSize, bool origin_clean);

    Variant<GC::Ref<HTML::CanvasRenderingContext2D>, GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>, Empty> m_context;
    bool m_canvas_content_dirty { false };
    u64 m_content_generation { 0 };

    bool m_is_placeholder { false };
    Optional<Compositing::CanvasId> m_placeholder_canvas_id;
    bool m_placeholder_frame_is_origin_clean { true };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLCanvasElement>() const { return is_html_canvas_element(); }

}
