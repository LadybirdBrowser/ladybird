/*
 * Copyright (c) 2024, Maciej <sppmacd@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::HTML {

struct DragEventInit : public UIEvents::MouseEventInit {
    GC::Ptr<DataTransfer> data_transfer;
};

// https://html.spec.whatwg.org/multipage/dnd.html#the-dragevent-interface
class DragEvent : public UIEvents::MouseEvent {
    WEB_PLATFORM_OBJECT(DragEvent, UIEvents::MouseEvent);
    GC_DECLARE_ALLOCATOR(DragEvent);

public:
    [[nodiscard]] static GC::Ref<DragEvent> create(JS::Realm&, FlyString const& event_name, DragEventInit const& event_init = {}, double page_x = 0, double page_y = 0, double offset_x = 0, double offset_y = 0);
    static WebIDL::ExceptionOr<GC::Ref<DragEvent>> construct_impl(JS::Realm&, FlyString const& event_name, DragEventInit const& event_init);

    virtual ~DragEvent() override;

    GC::Ptr<DataTransfer> data_transfer() { return m_data_transfer; }

private:
    DragEvent(JS::Realm&, FlyString const& event_name, DragEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ptr<DataTransfer> m_data_transfer;
};

}
