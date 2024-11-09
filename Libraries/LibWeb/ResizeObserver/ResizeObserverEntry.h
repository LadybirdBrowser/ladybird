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
    JS_DECLARE_ALLOCATOR(ResizeObserverEntry);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<ResizeObserverEntry>> create_and_populate(JS::Realm&, DOM::Element& target);

    JS::NonnullGCPtr<Geometry::DOMRectReadOnly> content_rect() const { return *m_content_rect; }
    JS::NonnullGCPtr<DOM::Element> target() const { return m_target; }

    Vector<JS::NonnullGCPtr<ResizeObserverSize>> const& border_box_size() const { return m_border_box_size; }
    Vector<JS::NonnullGCPtr<ResizeObserverSize>> const& content_box_size() const { return m_content_box_size; }
    Vector<JS::NonnullGCPtr<ResizeObserverSize>> const& device_pixel_content_box_size() const { return m_device_pixel_content_box_size; }

    JS::NonnullGCPtr<JS::Object> border_box_size_js_array() const;
    JS::NonnullGCPtr<JS::Object> content_box_size_js_array() const;
    JS::NonnullGCPtr<JS::Object> device_pixel_content_box_size_js_array() const;

private:
    explicit ResizeObserverEntry(JS::Realm& realm, DOM::Element& target)
        : PlatformObject(realm)
        , m_target(target)
    {
    }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    JS::NonnullGCPtr<DOM::Element> m_target;

    Vector<JS::NonnullGCPtr<ResizeObserverSize>> m_content_box_size;
    Vector<JS::NonnullGCPtr<ResizeObserverSize>> m_border_box_size;
    Vector<JS::NonnullGCPtr<ResizeObserverSize>> m_device_pixel_content_box_size;

    JS::GCPtr<Geometry::DOMRectReadOnly> m_content_rect;
};

}
