/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Bindings/Highlight.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::CSS {

using HighlightType = Bindings::HighlightType;

// https://drafts.csswg.org/css-highlight-api-1/#highlight
class WEB_API Highlight final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Highlight, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Highlight);

public:
    static GC::Ref<Highlight> construct_impl(GC::RootVector<GC::Ref<DOM::AbstractRange>> const& initial_ranges);
    virtual ~Highlight() override = default;

    WebIDL::Long priority() const { return m_priority; }
    void set_priority(WebIDL::Long priority) { m_priority = priority; }

    HighlightType type() const { return m_type; }
    void set_type(HighlightType type) { m_type = type; }

    // The set entries of this setlike object, in insertion order.
    OrderedHashTable<GC::Ref<DOM::AbstractRange>> const& ranges() const { return m_ranges; }
    bool has_range(DOM::AbstractRange&) const;
    void add_range(GC::Ref<DOM::AbstractRange>);
    bool remove_range(DOM::AbstractRange&);
    void clear_ranges();

private:
    Highlight();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    OrderedHashTable<GC::Ref<DOM::AbstractRange>> m_ranges;
    WebIDL::Long m_priority { 0 };
    HighlightType m_type { HighlightType::Highlight };
};

}
