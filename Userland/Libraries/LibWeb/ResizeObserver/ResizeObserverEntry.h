/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/ResizeObserverPrototype.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/ResizeObserver/ResizeObserverSize.h>

namespace Web::ResizeObserver {

// https://drafts.csswg.org/resize-observer-1/#resize-observer-entry-interface
class ResizeObserverEntry : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ResizeObserverEntry, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ResizeObserverEntry);

public:
    static WebIDL::ExceptionOr<GC::Ref<ResizeObserverEntry>> create_and_populate(JS::Realm&, DOM::Element& target);

    GC::Ref<Geometry::DOMRectReadOnly> content_rect() const { return *m_content_rect; }
    GC::Ref<DOM::Element> target() const { return m_target; }

    Vector<GC::Ref<ResizeObserverSize>> const& border_box_size() const { return m_border_box_size; }
    Vector<GC::Ref<ResizeObserverSize>> const& content_box_size() const { return m_content_box_size; }
    Vector<GC::Ref<ResizeObserverSize>> const& device_pixel_content_box_size() const { return m_device_pixel_content_box_size; }

    GC::Ref<JS::Object> border_box_size_js_array() const;
    GC::Ref<JS::Object> content_box_size_js_array() const;
    GC::Ref<JS::Object> device_pixel_content_box_size_js_array() const;

private:
    explicit ResizeObserverEntry(JS::Realm& realm, DOM::Element& target)
        : PlatformObject(realm)
        , m_target(target)
    {
    }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<DOM::Element> m_target;

    Vector<GC::Ref<ResizeObserverSize>> m_content_box_size;
    Vector<GC::Ref<ResizeObserverSize>> m_border_box_size;
    Vector<GC::Ref<ResizeObserverSize>> m_device_pixel_content_box_size;

    GC::Ptr<Geometry::DOMRectReadOnly> m_content_rect;
};

}
