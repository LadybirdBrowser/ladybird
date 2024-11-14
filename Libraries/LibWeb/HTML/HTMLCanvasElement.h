/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibGfx/Forward.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

namespace Web::HTML {

class HTMLCanvasElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLCanvasElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLCanvasElement);

public:
    using RenderingContext = Variant<GC::Root<CanvasRenderingContext2D>, GC::Root<WebGL::WebGLRenderingContext>, Empty>;

    virtual ~HTMLCanvasElement() override;

    bool allocate_painting_surface(size_t minimum_width = 0, size_t minimum_height = 0);
    RefPtr<Gfx::PaintingSurface> surface() { return m_surface; }
    RefPtr<Gfx::PaintingSurface const> surface() const { return m_surface; }

    JS::ThrowCompletionOr<RenderingContext> get_context(String const& type, JS::Value options);

    unsigned width() const;
    unsigned height() const;

    WebIDL::ExceptionOr<void> set_width(unsigned);
    WebIDL::ExceptionOr<void> set_height(unsigned);

    String to_data_url(StringView type, JS::Value quality);
    WebIDL::ExceptionOr<void> to_blob(GC::Ref<WebIDL::CallbackType> callback, StringView type, JS::Value quality);

    void present();

private:
    HTMLCanvasElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    virtual GC::Ptr<Layout::Node> create_layout_node(CSS::StyleProperties) override;
    virtual void adjust_computed_style(CSS::StyleProperties&) override;

    enum class HasOrCreatedContext {
        No,
        Yes,
    };

    HasOrCreatedContext create_2d_context();
    JS::ThrowCompletionOr<HasOrCreatedContext> create_webgl_context(JS::Value options);
    void reset_context_to_default_state();

    RefPtr<Gfx::PaintingSurface> m_surface;

    Variant<GC::Ref<HTML::CanvasRenderingContext2D>, GC::Ref<WebGL::WebGLRenderingContext>, Empty> m_context;
};

}
