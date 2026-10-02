/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Size.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::PictureInPicture {

// https://w3c.github.io/picture-in-picture/#interface-picture-in-picture-window
class PictureInPictureWindow final : public DOM::EventTarget {
    WEB_WRAPPABLE(PictureInPictureWindow, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(PictureInPictureWindow);

public:
    [[nodiscard]] static GC::Ref<PictureInPictureWindow> create(HTML::HTMLVideoElement&, Gfx::IntSize, HTML::LocalTraversableNavigable&);

    virtual ~PictureInPictureWindow() override;

    HTML::HTMLVideoElement& video() const { return m_video; }
    HTML::LocalTraversableNavigable& traversable() const { return m_traversable; }

    WebIDL::Long width() const;
    WebIDL::Long height() const;

    bool is_open() const { return m_state == State::Opened; }
    void set_size(Gfx::IntSize size) { m_size = size; }
    void close();

    void set_onresize(WebIDL::CallbackType*);
    WebIDL::CallbackType* onresize();

private:
    PictureInPictureWindow(HTML::HTMLVideoElement&, Gfx::IntSize, HTML::LocalTraversableNavigable&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    enum class State : u8 {
        Opened,
        Closed,
    };

    GC::Ref<HTML::HTMLVideoElement> m_video;
    GC::Ref<HTML::LocalTraversableNavigable> m_traversable;
    Gfx::IntSize m_size;
    State m_state { State::Opened };
};

}
