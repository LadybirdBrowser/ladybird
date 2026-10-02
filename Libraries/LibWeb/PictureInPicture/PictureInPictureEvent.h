/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PictureInPictureEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/PictureInPicture/PictureInPictureWindow.h>

namespace Web::PictureInPicture {

using PictureInPictureEventInit = Bindings::PictureInPictureEventInit;

// https://w3c.github.io/picture-in-picture/#pictureinpictureevent
class PictureInPictureEvent final : public DOM::Event {
    WEB_WRAPPABLE(PictureInPictureEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(PictureInPictureEvent);

public:
    static constexpr size_t picture_in_picture_window_offset() { return offsetof(PictureInPictureEvent, m_picture_in_picture_window); }
    [[nodiscard]] static GC::Ref<PictureInPictureEvent> create(Utf16FlyString const& event_name, PictureInPictureEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~PictureInPictureEvent() override;

    GC::Ref<PictureInPictureWindow> picture_in_picture_window() const { return m_picture_in_picture_window; }

private:
    PictureInPictureEvent(Utf16FlyString const& event_name, PictureInPictureEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<PictureInPictureWindow> m_picture_in_picture_window;
};

}
