/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/ConservativeVector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/UIEvents/Touch.h>

namespace Web::UIEvents {

class TouchList final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(TouchList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(TouchList);

public:
    static GC::Ref<TouchList> create(GC::ConservativeVector<GC::Ref<Touch>>);
    u32 length() const { return m_touches.size(); }
    GC::Ptr<Touch> item(u32 index) const { return index < m_touches.size() ? m_touches[index].ptr() : nullptr; }

private:
    explicit TouchList(GC::ConservativeVector<GC::Ref<Touch>>);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<GC::Ref<Touch>> m_touches;
};

}
