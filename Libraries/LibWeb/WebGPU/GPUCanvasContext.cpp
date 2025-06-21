/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PainterSkia.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebGPU/GPUCanvasContext.h>
#include <LibWeb/WebGPU/GPUDevice.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUCanvasContext);

GPUCanvasContext::GPUCanvasContext(JS::Realm& realm, HTML::HTMLCanvasElement& element)
    : PlatformObject(realm)
    , m_size(element.bitmap_size_for_canvas())
    , m_canvas(element)
{
}

GPUCanvasContext::~GPUCanvasContext() = default;

JS::ThrowCompletionOr<GC::Ref<GPUCanvasContext>> GPUCanvasContext::create(JS::Realm& realm, HTML::HTMLCanvasElement& element, JS::Value)
{
    return realm.create<GPUCanvasContext>(realm, element);
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpucanvascontext-configure
void GPUCanvasContext::configure(GPUCanvasConfiguration const& config)
{
    allocate_painting_surface_if_needed();
    VERIFY(!config.device.is_null());

    config.device->on_queue_submitted([this]() {
        // FIXME: Follow spec guidelines for how to update the canvas drawing buffer
        //  https://www.w3.org/TR/webgpu/#abstract-opdef-get-a-copy-of-the-image-contents-of-a-context

        m_surface->notify_content_will_change();
        auto const mapped_texture_buffer = MUST(m_current_texture->map_buffer());
        for (auto const& [pixel, x, y] : mapped_texture_buffer->pixels()) {
            m_bitmap->set_pixel(x, y, pixel);
        }
        update_display();
    });

    m_current_texture = config.device->texture(m_size);

    m_surface->notify_content_will_change();
    constexpr auto transparent_black = Color(0, 0, 0, 0);
    for (int y = 0; y < m_bitmap->height(); ++y) {
        for (int x = 0; x < m_bitmap->width(); ++x) {
            m_bitmap->set_pixel(x, y, transparent_black);
        }
    }
    update_display();
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpucanvascontext-getcurrenttexture
GC::Root<GPUTexture> GPUCanvasContext::get_current_texture() const
{
    // FIXME: Use double or triple buffering
    return m_current_texture;
}

void GPUCanvasContext::allocate_painting_surface_if_needed()
{
    if (m_surface || m_size.is_empty())
        return;

    // FIXME: Handle all supported configuration formats, not just RGBA
    m_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, m_size));
    m_surface = Gfx::PaintingSurface::wrap_bitmap(*m_bitmap);
}

void GPUCanvasContext::set_size(Gfx::IntSize const& size)
{
    if (m_size == size)
        return;
    m_size = size;
    m_surface = nullptr;
    m_bitmap = nullptr;
}

void GPUCanvasContext::update_display() const
{
    if (auto* paintable = m_canvas->paintable()) {
        paintable->set_needs_display();
    }
}

void GPUCanvasContext::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_canvas);
    visitor.visit(m_current_texture);
}

void GPUCanvasContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUCanvasContext);
    Base::initialize(realm);
}

}
