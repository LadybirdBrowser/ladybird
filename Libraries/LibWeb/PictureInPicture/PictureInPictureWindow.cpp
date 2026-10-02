/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/PictureInPicture/PictureInPictureWindow.h>

namespace Web::PictureInPicture {

GC_DEFINE_ALLOCATOR(PictureInPictureWindow);

GC::Ref<PictureInPictureWindow> PictureInPictureWindow::create(HTML::HTMLVideoElement& video, Gfx::IntSize size, HTML::LocalTraversableNavigable& traversable)
{
    return GC::Heap::the().allocate<PictureInPictureWindow>(video, size, traversable);
}

// When instantiated, an instance of PictureInPictureWindow has its state set to opened.
PictureInPictureWindow::PictureInPictureWindow(HTML::HTMLVideoElement& video, Gfx::IntSize size, HTML::LocalTraversableNavigable& traversable)
    : m_video(video)
    , m_traversable(traversable)
    , m_size(size)
{
}

PictureInPictureWindow::~PictureInPictureWindow() = default;

void PictureInPictureWindow::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_video);
    visitor.visit(m_traversable);
}

GC::Ptr<Bindings::Wrappable> PictureInPictureWindow::relevant_global_impl() const
{
    return m_video->document().window();
}

// https://w3c.github.io/picture-in-picture/#dom-pictureinpicturewindow-width
WebIDL::Long PictureInPictureWindow::width() const
{
    // The width attribute MUST return the width in CSS pixels of the Picture-in-Picture window associated with
    // pictureInPictureElement if the state is opened. Otherwise, it MUST return 0.
    return is_open() ? m_size.width() : 0;
}

// https://w3c.github.io/picture-in-picture/#dom-pictureinpicturewindow-height
WebIDL::Long PictureInPictureWindow::height() const
{
    // The height attribute MUST return the height in CSS pixels of the Picture-in-Picture window associated with
    // pictureInPictureElement if the state is opened. Otherwise, it MUST return 0.
    return is_open() ? m_size.height() : 0;
}

// https://w3c.github.io/picture-in-picture/#close-window-algorithm
void PictureInPictureWindow::close()
{
    // When the close window algorithm with an instance of PictureInPictureWindow is invoked, its state is set to
    // closed.
    m_state = State::Closed;
}

void PictureInPictureWindow::set_onresize(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::resize, event_handler);
}

WebIDL::CallbackType* PictureInPictureWindow::onresize()
{
    return event_handler_attribute(HTML::EventNames::resize);
}

}
