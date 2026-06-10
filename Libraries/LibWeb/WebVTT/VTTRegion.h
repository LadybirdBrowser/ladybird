/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/VTTRegion.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebVTT {

using ScrollSetting = Bindings::ScrollSetting;

// https://w3c.github.io/webvtt/#vttregion
class VTTRegion final : public Bindings::Wrappable {
    WEB_WRAPPABLE(VTTRegion, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(VTTRegion);

public:
    static GC::Ref<VTTRegion> create();
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

    ScrollSetting scroll() const;
    void set_scroll(ScrollSetting);

private:
    VTTRegion();

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
    ScrollSetting m_scroll_setting { ScrollSetting::Empty };
};

}
