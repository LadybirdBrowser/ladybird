/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/VisualViewportPrototype.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(VisualViewport);

JS::NonnullGCPtr<VisualViewport> VisualViewport::create(DOM::Document& document)
{
    return document.heap().allocate<VisualViewport>(document.realm(), document);
}

VisualViewport::VisualViewport(DOM::Document& document)
    : DOM::EventTarget(document.realm())
    , m_document(document)
{
}

void VisualViewport::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VisualViewport);
}

void VisualViewport::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-offsetleft
double VisualViewport::offset_left() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the offset of the left edge of the visual viewport from the left edge of the layout viewport.
    VERIFY(m_document->navigable());
    return m_document->viewport_rect().left().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-offsettop
double VisualViewport::offset_top() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the offset of the top edge of the visual viewport from the top edge of the layout viewport.
    VERIFY(m_document->navigable());
    return m_document->viewport_rect().top().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-pageleft
double VisualViewport::page_left() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // FIXME: 2. Otherwise, return the offset of the left edge of the visual viewport
    //           from the left edge of the initial containing block of the layout viewport’s document.
    return offset_left();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-pagetop
double VisualViewport::page_top() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // FIXME: 2. Otherwise, return the offset of the top edge of the visual viewport
    //           from the top edge of the initial containing block of the layout viewport’s document.
    return offset_top();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-width
double VisualViewport::width() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the width of the visual viewport
    //    FIXME: excluding the width of any rendered vertical classic scrollbar that is fixed to the visual viewport.
    VERIFY(m_document->navigable());
    return m_document->viewport_rect().width().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-height
double VisualViewport::height() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the height of the visual viewport
    //    FIXME: excluding the height of any rendered vertical classic scrollbar that is fixed to the visual viewport.
    VERIFY(m_document->navigable());
    return m_document->viewport_rect().height().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-scale
double VisualViewport::scale() const
{
    // FIXME: Implement.
    return 1;
}

void VisualViewport::set_onresize(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::resize, event_handler);
}

WebIDL::CallbackType* VisualViewport::onresize()
{
    return event_handler_attribute(HTML::EventNames::resize);
}

void VisualViewport::set_onscroll(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::scroll, event_handler);
}

WebIDL::CallbackType* VisualViewport::onscroll()
{
    return event_handler_attribute(HTML::EventNames::scroll);
}

void VisualViewport::set_onscrollend(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::scrollend, event_handler);
}

WebIDL::CallbackType* VisualViewport::onscrollend()
{
    return event_handler_attribute(HTML::EventNames::scrollend);
}

}
