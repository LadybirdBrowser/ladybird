/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/CrossOriginPropertyDescriptorMap.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

class WEB_API RemoteWindow final : public JS::Cell {
    GC_CELL(RemoteWindow, JS::Cell);
    GC_DECLARE_ALLOCATOR(RemoteWindow);

public:
    static GC::Ref<RemoteWindow> create(RemoteNavigable&);
    virtual ~RemoteWindow() override;

    GC::Ptr<RemoteNavigable> navigable() const;
    URL::Origin const& origin() const;

    GC::Ref<WindowProxy> window() const;
    GC::Ref<WindowProxy> self() const;
    GC::Ref<Location> location();
    bool closed() const;
    GC::Ref<WindowProxy> frames() const;
    u32 length();
    GC::Ptr<WindowProxy const> top() const;
    GC::Ptr<WindowProxy const> opener() const;
    GC::Ptr<WindowProxy const> parent() const;
    void close();
    void focus();
    void blur();
    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, Utf16String const& target_origin, GC::RootVector<GC::Ref<JS::Object>> const& transfer);
    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, Window::PostMessageOptions const&);

    Vector<GC::Root<Navigable>> document_tree_child_navigables();
    OrderedHashMap<Utf16FlyString, GC::Ref<Navigable>> document_tree_child_navigable_target_name_property_set();

    CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map() { return m_cross_origin_property_descriptor_map; }

private:
    explicit RemoteWindow(RemoteNavigable&);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<RemoteNavigable> m_navigable;

    CrossOriginPropertyDescriptorMap m_cross_origin_property_descriptor_map;
};

}
