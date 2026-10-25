/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16String.h>
#include <LibGC/ConservativeVector.h>
#include <LibWeb/Bindings/HighlightRegistry.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-highlight-api-1/#highlightregistry
class WEB_API HighlightRegistry final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(HighlightRegistry, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(HighlightRegistry);

public:
    [[nodiscard]] static GC::Ref<HighlightRegistry> create(DOM::Document&);
    virtual ~HighlightRegistry() override = default;

    // The map entries of this maplike object, in insertion order.
    OrderedHashMap<Utf16String, GC::Ref<Highlight>> const& highlights() const { return m_highlights; }
    GC::Ptr<Highlight> get_highlight(Utf16View name) const;
    bool has_highlight(Utf16View name) const;
    void set_highlight(Utf16String name, GC::Ref<Highlight>);
    bool remove_highlight(Utf16View name);
    void clear_highlights();

    GC::ConservativeVector<Bindings::HighlightHitResult> highlights_from_point(float x, float y, Bindings::HighlightsFromPointOptions const&);

private:
    explicit HighlightRegistry(DOM::Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<DOM::Document> m_document;
    OrderedHashMap<Utf16String, GC::Ref<Highlight>> m_highlights;
};

}
