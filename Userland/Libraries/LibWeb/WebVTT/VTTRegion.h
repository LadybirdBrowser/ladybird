/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/VTTRegionPrototype.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebVTT {

// https://w3c.github.io/webvtt/#vttregion
class VTTRegion final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(VTTRegion, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(VTTRegion);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<VTTRegion>> construct_impl(JS::Realm&);
    virtual ~VTTRegion() override = default;

    String const& id() const { return m_identifier; }
    void set_id(String const& id) { m_identifier = id; }

    double width() const { return m_width; }
    WebIDL::ExceptionOr<void> set_width(double width);

    WebIDL::UnsignedLong lines() const { return m_lines; }
    void set_lines(WebIDL::UnsignedLong lines) { m_lines = lines; }

    double region_anchor_x() const { return m_anchor_x; }
    WebIDL::ExceptionOr<void> set_region_anchor_x(double region_anchor_x);

    double region_anchor_y() const { return m_anchor_y; }
    WebIDL::ExceptionOr<void> set_region_anchor_y(double region_anchor_y);

    double viewport_anchor_x() const { return m_viewport_anchor_x; }
    WebIDL::ExceptionOr<void> set_viewport_anchor_x(double viewport_anchor_x);

    double viewport_anchor_y() const { return m_viewport_anchor_y; }
    WebIDL::ExceptionOr<void> set_viewport_anchor_y(double viewport_anchor_y);

    Bindings::ScrollSetting scroll() const { return m_scroll_setting; }
    void set_scroll(Bindings::ScrollSetting scroll) { m_scroll_setting = scroll; }

private:
    VTTRegion(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    // https://w3c.github.io/webvtt/#webvtt-region-identifier
    String m_identifier {};

    // https://w3c.github.io/webvtt/#webvtt-region-width
    double m_width { 100 };

    // https://w3c.github.io/webvtt/#webvtt-region-lines
    WebIDL::UnsignedLong m_lines { 3 };

    // https://w3c.github.io/webvtt/#webvtt-region-anchor
    double m_anchor_x { 0 };
    double m_anchor_y { 100 };

    // https://w3c.github.io/webvtt/#webvtt-region-viewport-anchor
    double m_viewport_anchor_x { 0 };
    double m_viewport_anchor_y { 100 };

    // https://w3c.github.io/webvtt/#webvtt-region-scroll
    Bindings::ScrollSetting m_scroll_setting { Bindings::ScrollSetting::Empty };
};

}
