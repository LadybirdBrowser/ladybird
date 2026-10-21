/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibGC/WeakHashSet.h>
#include <LibWeb/Animations/ViewTimeline.h>
#include <LibWeb/Forward.h>

namespace Web::Animations {

// The scroll progress timelines and view progress timelines that elements of a document declare by name with the
// 'scroll-timeline' and 'view-timeline' properties, and the animations that look them up by name.
// https://drafts.csswg.org/scroll-animations-1/#timeline-scoping
class NamedTimelines final : public GC::Cell {
    GC_CELL(NamedTimelines, GC::Cell);
    GC_DECLARE_ALLOCATOR(NamedTimelines);

public:
    [[nodiscard]] static GC::Ref<NamedTimelines> create(DOM::Document&);

    // https://drafts.csswg.org/scroll-animations-1/#target-timeline
    GC::Ptr<ScrollTimeline> target_timeline(DOM::AbstractElement const&, Utf16FlyString const& name);

    // Tracks an animation whose timeline is a named timeline, so that it follows the timeline its name finds as the
    // declarations of the document change.
    void track_animation(CSS::CSSAnimation&);

    // Looks the timelines of the tracked animations up again, where the declarations of the document have changed.
    // Returns whether any animation changed its timeline.
    bool update_tracked_animations();

private:
    explicit NamedTimelines(DOM::Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    struct Declaration {
        Utf16FlyString name;
        bool is_view { false };
        ScrollAxis axis { ScrollAxis::Block };
        ViewTimeline::Inset view_inset;
    };
    struct DeclaringElement {
        GC::Ref<DOM::Element> element;
        Vector<Declaration> declarations;
        bool scopes_all_names { false };
        Vector<Utf16FlyString> scoped_names;

        Declaration const* declaration_named(Utf16FlyString const&) const;
        bool scopes(Utf16FlyString const& name) const { return scopes_all_names || scoped_names.contains_slow(name); }
    };

    // Collects the declarations of the document again, when its tree or the timeline declarations of its elements
    // have changed since they were last collected.
    void refresh_declarations();

    DeclaringElement const* declaring_element(DOM::Element const&) const;
    GC::Ref<ScrollTimeline> timeline_for(DOM::Element&, Declaration const&);

    GC::Ref<DOM::Document> m_document;

    // The elements that declare timelines or scope timeline names, in tree order.
    Vector<DeclaringElement> m_declaring_elements;
    HashMap<GC::RawPtr<DOM::Element const>, size_t> m_declaring_element_indices;
    Optional<u64> m_collected_dom_tree_version;
    Optional<u64> m_collected_declarations_generation;

    struct TimelineKey {
        GC::Ref<DOM::Element> element;
        Utf16FlyString name;

        bool operator==(TimelineKey const&) const = default;
    };
    struct TimelineKeyTraits : public DefaultTraits<TimelineKey> {
        static unsigned hash(TimelineKey const& key) { return pair_int_hash(ptr_hash(key.element.ptr()), key.name.hash()); }
    };
    HashMap<TimelineKey, GC::Ref<ScrollTimeline>, TimelineKeyTraits> m_timelines;

    GC::WeakHashSet<CSS::CSSAnimation> m_tracked_animations;
    bool m_tracked_animations_need_update { false };
};

}
