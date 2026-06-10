/*
 * Copyright (c) 2022, DerpyCrabs <derpycrabs@gmail.com>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Geometry/DOMRect.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry-1/#DOMRectList
class DOMRectList final : public Bindings::Wrappable {
    WEB_WRAPPABLE(DOMRectList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMRectList);

public:
    [[nodiscard]] static GC::Ref<DOMRectList> create(Vector<GC::Root<DOMRect>>);

    virtual ~DOMRectList() override;

    u32 length() const;
    DOMRect const* item(u32 index) const;

private:
    explicit DOMRectList(Vector<GC::Ref<DOMRect>>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<GC::Ref<DOMRect>> m_rects;
};

}
