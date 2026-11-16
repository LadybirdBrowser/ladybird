/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/Forward.h>

namespace Web::SVG {

// https://svgwg.org/specs/animations/#SVGElementTimelinebeginAttribute
class SMILTimeContainer final : public GC::Cell {
    GC_CELL(SMILTimeContainer, GC::Cell);
    GC_DECLARE_ALLOCATOR(SMILTimeContainer);

public:
    [[nodiscard]] static GC::Ref<SMILTimeContainer> create(DOM::Document&);

    void begin();
    bool has_begun() const { return m_has_begun; }

    void pause();
    void unpause();
    bool is_paused() const { return m_is_paused; }

    double current_time_in_milliseconds() const;
    void seek_to_time_in_milliseconds(double);
    void record_time_to_seek_to_once_begun_in_milliseconds(double);

    void add_animation(Animations::AnimationRef const&);
    void remove_animation(Animations::AnimationRef const&);

private:
    explicit SMILTimeContainer(DOM::Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    double document_timeline_time() const;
    void synchronize_animations();
    void synchronize_animation(Animations::AnimationRef const&);

    GC::Ref<DOM::Document> m_document;
    // NB: The animations of the animation elements of the timeline, which this traces as their animation elements hold
    //     them.
    Vector<Animations::AnimationRef> m_animations;

    bool m_has_begun { false };
    bool m_is_paused { false };
    Optional<double> m_time_to_seek_to_once_begun;
    double m_paused_time { 0 };
    double m_zero_time { 0 };
};

}
