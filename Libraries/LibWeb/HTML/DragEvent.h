/*
 * Copyright (c) 2024, Maciej <sppmacd@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::Bindings {

struct DragEventInit;

}

namespace Web::HTML {

struct DragEventInit : UIEvents::MouseEventOptions {
    GC::Ptr<DataTransfer> data_transfer;
};

// https://html.spec.whatwg.org/multipage/dnd.html#the-dragevent-interface
class DragEvent : public UIEvents::MouseEvent {
    WEB_WRAPPABLE(DragEvent, UIEvents::MouseEvent);
    GC_DECLARE_ALLOCATOR(DragEvent);

public:
    [[nodiscard]] static GC::Ref<DragEvent> create(Utf16FlyString const& event_name, DragEventInit const& event_init = {}, double page_x = 0, double page_y = 0, double offset_x = 0, double offset_y = 0, HighResolutionTime::DOMHighResTimeStamp = 0);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<DragEvent>> create_for_constructor(Utf16FlyString const&, Bindings::DragEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~DragEvent() override;

    GC::Ptr<DataTransfer> data_transfer() { return m_data_transfer; }
    static constexpr size_t data_transfer_offset() { return offsetof(DragEvent, m_data_transfer); }

private:
    DragEvent(Utf16FlyString const& event_name, DragEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<DataTransfer> m_data_transfer;
};

}
