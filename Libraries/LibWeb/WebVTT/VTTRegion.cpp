/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebVTT/VTTRegion.h>

namespace Web::WebVTT {

JS_DEFINE_ALLOCATOR(VTTRegion);

// https://w3c.github.io/webvtt/#dom-vttregion-vttregion
WebIDL::ExceptionOr<JS::NonnullGCPtr<VTTRegion>> VTTRegion::construct_impl(JS::Realm& realm)
{
    // 1. Create a new WebVTT region. Let region be that WebVTT region.
    auto region = realm.heap().allocate<VTTRegion>(realm, realm);

    // 2. Let region’s WebVTT region identifier be the empty string.
    region->m_identifier = ""_string;

    // 3. Let region’s WebVTT region width be 100.
    region->m_width = 100;

    // 4. Let region’s WebVTT region lines be 3.
    region->m_lines = 3;

    // 5. Let region’s text track region regionAnchorX be 0.
    region->m_anchor_x = 0;

    // 6. Let region’s text track region regionAnchorY be 100.
    region->m_anchor_y = 100;

    // 7. Let region’s text track region viewportAnchorX be 0.
    region->m_viewport_anchor_x = 0;

    // 8. Let region’s text track region viewportAnchorY be 100.
    region->m_viewport_anchor_y = 100;

    // 9. Let region’s WebVTT region scroll be the empty string.
    region->m_scroll_setting = Bindings::ScrollSetting::Empty;

    // 10. Return the VTTRegion object representing region.
    return region;
}

VTTRegion::VTTRegion(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void VTTRegion::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VTTRegion);
}

// https://w3c.github.io/webvtt/#dom-vttregion-width
WebIDL::ExceptionOr<void> VTTRegion::set_width(double width)
{
    // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
    if (width < 0 || width > 100)
        return WebIDL::IndexSizeError::create(realm(), "Value is negative or greater than 100"_string);

    // Otherwise, the WebVTT region width must be set to the new value.
    m_width = width;
    return {};
}

// https://w3c.github.io/webvtt/#dom-vttregion-regionanchorx
WebIDL::ExceptionOr<void> VTTRegion::set_region_anchor_x(double region_anchor_x)
{
    // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
    if (region_anchor_x < 0 || region_anchor_x > 100)
        return WebIDL::IndexSizeError::create(realm(), "Value is negative or greater than 100"_string);

    // Otherwise, the WebVTT region anchor X distance must be set to the new value.
    m_anchor_x = region_anchor_x;
    return {};
}

// https://w3c.github.io/webvtt/#dom-vttregion-regionanchory
WebIDL::ExceptionOr<void> VTTRegion::set_region_anchor_y(double region_anchor_y)
{
    // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
    if (region_anchor_y < 0 || region_anchor_y > 100)
        return WebIDL::IndexSizeError::create(realm(), "Value is negative or greater than 100"_string);

    // Otherwise, the WebVTT region anchor Y distance must be set to the new value.
    m_anchor_y = region_anchor_y;
    return {};
}

// https://w3c.github.io/webvtt/#dom-vttregion-viewportanchorx
WebIDL::ExceptionOr<void> VTTRegion::set_viewport_anchor_x(double viewport_anchor_x)
{
    // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
    if (viewport_anchor_x < 0 || viewport_anchor_x > 100)
        return WebIDL::IndexSizeError::create(realm(), "Value is negative or greater than 100"_string);

    // Otherwise, the WebVTT region viewport anchor X distance must be set to the new value.
    m_viewport_anchor_x = viewport_anchor_x;
    return {};
}

// https://w3c.github.io/webvtt/#dom-vttregion-viewportanchory
WebIDL::ExceptionOr<void> VTTRegion::set_viewport_anchor_y(double viewport_anchor_y)
{
    // On setting, if the new value is negative or greater than 100, then an IndexSizeError exception must be thrown.
    if (viewport_anchor_y < 0 || viewport_anchor_y > 100)
        return WebIDL::IndexSizeError::create(realm(), "Value is negative or greater than 100"_string);

    // Otherwise, the WebVTT region viewport anchor Y distance must be set to the new value.
    m_viewport_anchor_y = viewport_anchor_y;
    return {};
}

}
