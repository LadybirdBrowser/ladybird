/*
 * Copyright (c) 2022, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/UIEvent.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Bindings {

struct WheelEventInit;

}

namespace Web::UIEvents {

enum WheelDeltaMode : WebIDL::UnsignedLong {
    DOM_DELTA_PIXEL = 0,
    DOM_DELTA_LINE = 1,
    DOM_DELTA_PAGE = 2,
};

enum class WheelEventIsCancelable : u8 {
    No,
    Yes,
};

struct WheelEventOptions : public MouseEventOptions {
    double delta_x { 0 };
    double delta_y { 0 };
    double delta_z { 0 };
    WebIDL::UnsignedLong delta_mode { WheelDeltaMode::DOM_DELTA_PIXEL };
};

class WheelEvent final : public MouseEvent {
    WEB_WRAPPABLE(WheelEvent, MouseEvent);
    GC_DECLARE_ALLOCATOR(WheelEvent);

public:
    [[nodiscard]] static GC::Ref<WheelEvent> create(FlyString const& event_name, WheelEventOptions const& = {}, double page_x = 0, double page_y = 0, double offset_x = 0, double offset_y = 0, HighResolutionTime::DOMHighResTimeStamp = 0);
    [[nodiscard]] static GC::Ref<WheelEvent> create_for_constructor(FlyString const&, Bindings::WheelEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    static WebIDL::ExceptionOr<GC::Ref<WheelEvent>> create_from_platform_event(JS::Object const& relevant_global_object, GC::Ptr<HTML::WindowProxy>, FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, double delta_x, double delta_y, unsigned button, unsigned buttons, unsigned modifiers, WheelEventIsCancelable = WheelEventIsCancelable::Yes);

    virtual ~WheelEvent() override;

    double delta_x() const { return m_delta_x; }
    double delta_y() const { return m_delta_y; }
    double delta_z() const { return m_delta_z; }
    WebIDL::UnsignedLong delta_mode() const { return m_delta_mode; }

private:
    WheelEvent(FlyString const& event_name, WheelEventOptions const& options, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp);

    double m_delta_x { 0 };
    double m_delta_y { 0 };
    double m_delta_z { 0 };
    WebIDL::UnsignedLong m_delta_mode { WheelDeltaMode::DOM_DELTA_PIXEL };
};

}
