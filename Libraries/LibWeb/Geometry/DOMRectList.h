/*
 * Copyright (c) 2022, DerpyCrabs <derpycrabs@gmail.com>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geometry/DOMRect.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry-1/#DOMRectList
class DOMRectList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMRectList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DOMRectList);

public:
    [[nodiscard]] static JS::NonnullGCPtr<DOMRectList> create(JS::Realm&, Vector<JS::Handle<DOMRect>>);

    virtual ~DOMRectList() override;

    u32 length() const;
    DOMRect const* item(u32 index) const;

    virtual Optional<JS::Value> item_value(size_t index) const override;

private:
    DOMRectList(JS::Realm&, Vector<JS::NonnullGCPtr<DOMRect>>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<JS::NonnullGCPtr<DOMRect>> m_rects;
};

}
