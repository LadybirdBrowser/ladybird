/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>

namespace Web::CSS {

namespace {

struct FFIPixelPoint {
    i32 x;
    i32 y;
};

struct FFIPixelRect {
    i32 x;
    i32 y;
    i32 width;
    i32 height;
};

}

static FFIPixelPoint to_ffi(CSSPixelPoint point)
{
    return { point.x().raw_value(), point.y().raw_value() };
}

static CSSPixelPoint from_ffi(FFIPixelPoint point)
{
    return { CSSPixels::from_raw(point.x), CSSPixels::from_raw(point.y) };
}

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_visual_viewport_create(JSVM*, void* realm, void* document);
void* web_visual_viewport_event_target(void* viewport);
FFIPixelPoint web_visual_viewport_offset(void* viewport);
double web_visual_viewport_offset_left(void* viewport);
double web_visual_viewport_offset_top(void* viewport);
double web_visual_viewport_page_left(void* viewport);
double web_visual_viewport_page_top(void* viewport);
double web_visual_viewport_width(void* viewport);
double web_visual_viewport_height(void* viewport);
double web_visual_viewport_scale(void* viewport);
void web_visual_viewport_scroll_by(void* viewport, FFIPixelPoint delta);
void web_visual_viewport_zoom(void* viewport, FFIPixelPoint position, double scale_delta);
void web_visual_viewport_set_scale_zoomed_by_compositor(void* viewport, double scale);
bool web_visual_viewport_take_scale_for_compositor(void* viewport, double* scale);
FFIPixelPoint web_visual_viewport_map_to_layout_viewport(void* viewport, FFIPixelPoint position);
FFIPixelPoint web_visual_viewport_map_point_from_layout_viewport(void* viewport, FFIPixelPoint position);
FFIPixelRect web_visual_viewport_map_rect_from_layout_viewport(void* viewport, FFIPixelRect rect);
void web_visual_viewport_reset(void* viewport);

// Declared in Bindings/Rust/src/css/visual_viewport.rs.
bool web_css_visual_viewport_document_is_fully_active(void* document);
FFIPixelRect web_css_visual_viewport_document_viewport_rect(void* document);
void web_css_visual_viewport_append_pending_scroll_event(void* document, void* event_target, ReadonlyBytes type);
void web_css_visual_viewport_did_change_offset_or_scale(void* document);
}

static void* to_abi(VisualViewport const* viewport)
{
    return const_cast<VisualViewport*>(viewport);
}

static DOM::Document& document_from_abi(void* document)
{
    return *static_cast<DOM::Document*>(static_cast<GC::Cell*>(document));
}

bool web_css_visual_viewport_document_is_fully_active(void* document)
{
    return document_from_abi(document).is_fully_active();
}

FFIPixelRect web_css_visual_viewport_document_viewport_rect(void* document)
{
    auto rect = document_from_abi(document).viewport_rect();
    return { rect.x().raw_value(), rect.y().raw_value(), rect.width().raw_value(), rect.height().raw_value() };
}

void web_css_visual_viewport_append_pending_scroll_event(void* document, void* event_target, ReadonlyBytes type)
{
    auto& target = *static_cast<DOM::EventTarget*>(static_cast<GC::Cell*>(event_target));
    document_from_abi(document).append_pending_scroll_event({ target, Utf16FlyString::from_utf8_without_validation(type) });
}

void web_css_visual_viewport_did_change_offset_or_scale(void* document)
{
    VisualViewport::did_change_offset_or_scale(document_from_abi(document));
}

static void update_accumulated_visual_context(DOM::Document& document)
{
    Layout::ForcedReadScope read { document };
    if (document.has_committed_viewport_box() && document.paint_state().has_visual_context_tree(read)) {
        document.paint_state().update_visual_viewport_accumulated_visual_context(read, document);
        return;
    }

    document.schedule_full_accumulated_visual_context_rebuild(Layout::RustFFI::VisualContextUpdateScope::FreshTree);
}

void VisualViewport::did_change_offset_or_scale(DOM::Document& document)
{
    update_accumulated_visual_context(document);
    document.set_needs_repaint(Badge<CSS::VisualViewport> {}, InvalidateDisplayList::No);
    if (auto navigable = document.navigable())
        navigable->did_change_scroll_offset({ .node_id = document.unique_id(), .kind = Web::AsyncScrollNodeKind::VisualViewport });
}

GC::Ref<VisualViewport> VisualViewport::create(DOM::Document& document)
{
    auto* viewport = web_visual_viewport_create(JS::HostABI::vm_to_abi(JS::VM::the()), &HTML::relevant_realm(document), static_cast<GC::Cell*>(&document));
    return GC::Ref { *static_cast<VisualViewport*>(viewport) };
}

GC::Ref<DOM::EventTarget> VisualViewport::event_target() const
{
    return *static_cast<DOM::EventTarget*>(static_cast<GC::Cell*>(web_visual_viewport_event_target(to_abi(this))));
}

CSSPixelPoint VisualViewport::offset() const { return from_ffi(web_visual_viewport_offset(to_abi(this))); }
double VisualViewport::offset_left() const { return web_visual_viewport_offset_left(to_abi(this)); }
double VisualViewport::offset_top() const { return web_visual_viewport_offset_top(to_abi(this)); }
double VisualViewport::page_left() const { return web_visual_viewport_page_left(to_abi(this)); }
double VisualViewport::page_top() const { return web_visual_viewport_page_top(to_abi(this)); }
double VisualViewport::width() const { return web_visual_viewport_width(to_abi(this)); }
double VisualViewport::height() const { return web_visual_viewport_height(to_abi(this)); }
double VisualViewport::scale() const { return web_visual_viewport_scale(to_abi(this)); }
void VisualViewport::scroll_by(CSSPixelPoint delta) { web_visual_viewport_scroll_by(this, to_ffi(delta)); }
void VisualViewport::zoom(CSSPixelPoint position, double scale_delta) { web_visual_viewport_zoom(this, to_ffi(position), scale_delta); }
void VisualViewport::set_scale_zoomed_by_compositor(double scale) { web_visual_viewport_set_scale_zoomed_by_compositor(this, scale); }
void VisualViewport::reset() { web_visual_viewport_reset(this); }

Optional<double> VisualViewport::take_scale_for_compositor()
{
    double scale = 0;
    if (!web_visual_viewport_take_scale_for_compositor(this, &scale))
        return {};
    return scale;
}

CSSPixelPoint VisualViewport::map_to_layout_viewport(CSSPixelPoint position) const
{
    return from_ffi(web_visual_viewport_map_to_layout_viewport(to_abi(this), to_ffi(position)));
}

CSSPixelPoint VisualViewport::map_from_layout_viewport(CSSPixelPoint position) const
{
    return from_ffi(web_visual_viewport_map_point_from_layout_viewport(to_abi(this), to_ffi(position)));
}

CSSPixelRect VisualViewport::map_from_layout_viewport(CSSPixelRect const& rect) const
{
    auto mapped = web_visual_viewport_map_rect_from_layout_viewport(to_abi(this), { rect.x().raw_value(), rect.y().raw_value(), rect.width().raw_value(), rect.height().raw_value() });
    return { CSSPixels::from_raw(mapped.x), CSSPixels::from_raw(mapped.y), CSSPixels::from_raw(mapped.width), CSSPixels::from_raw(mapped.height) };
}

}
