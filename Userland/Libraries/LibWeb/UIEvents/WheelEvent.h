/*
 * Copyright (c) 2022, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/UIEvent.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::UIEvents {

enum WheelDeltaMode : WebIDL::UnsignedLong {
    DOM_DELTA_PIXEL = 0,
    DOM_DELTA_LINE = 1,
    DOM_DELTA_PAGE = 2,
};

struct WheelEventInit : public MouseEventInit {
    double delta_x = 0;
    double delta_y = 0;
    double delta_z = 0;

    WebIDL::UnsignedLong delta_mode = WheelDeltaMode::DOM_DELTA_PIXEL;
};

class WheelEvent final : public MouseEvent {
    WEB_PLATFORM_OBJECT(WheelEvent, MouseEvent);
    JS_DECLARE_ALLOCATOR(WheelEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<WheelEvent> create(JS::Realm&, FlyString const& event_name, WheelEventInit const& = {}, double page_x = 0, double page_y = 0, double offset_x = 0, double offset_y = 0);
    [[nodiscard]] static JS::NonnullGCPtr<WheelEvent> construct_impl(JS::Realm&, FlyString const& event_name, WheelEventInit const& = {});

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<WheelEvent>> create_from_platform_event(JS::Realm&, FlyString const& event_name, CSSPixelPoint screen, CSSPixelPoint page, CSSPixelPoint client, CSSPixelPoint offset, double delta_x, double delta_y, unsigned button, unsigned buttons, unsigned modifiers);

    virtual ~WheelEvent() override;

    double delta_x() const { return m_delta_x; }
    double delta_y() const { return m_delta_y; }
    double delta_z() const { return m_delta_z; }
    WebIDL::UnsignedLong delta_mode() const { return m_delta_mode; }

private:
    WheelEvent(JS::Realm&, FlyString const& event_name, WheelEventInit const& event_init, double page_x, double page_y, double offset_x, double offset_y);

    virtual void initialize(JS::Realm&) override;

    double m_delta_x { 0 };
    double m_delta_y { 0 };
    double m_delta_z { 0 };
    WebIDL::UnsignedLong m_delta_mode { WheelDeltaMode::DOM_DELTA_PIXEL };
};

}
