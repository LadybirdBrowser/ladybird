/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/ElapsedTimer.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/UserTiming/PerformanceMark.h>
#include <LibWeb/UserTiming/PerformanceMeasure.h>

namespace Web::HighResolutionTime {

class Performance final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Performance, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Performance);

public:
    virtual ~Performance() override;

    double now() const;
    double time_origin() const;

    WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMark>> mark(String const& mark_name, UserTiming::PerformanceMarkOptions const& mark_options = {});
    void clear_marks(Optional<String> mark_name);
    WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMeasure>> measure(String const& measure_name, Variant<String, UserTiming::PerformanceMeasureOptions> const& start_or_measure_options, Optional<String> end_mark);
    void clear_measures(Optional<String> measure_name);

    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries() const;
    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries_by_type(String const& type) const;
    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries_by_name(String const& name, Optional<String> type) const;

    GC::Ptr<NavigationTiming::PerformanceTiming> timing();
    GC::Ptr<NavigationTiming::PerformanceNavigation> navigation();

private:
    explicit Performance(JS::Realm&);

    HTML::WindowOrWorkerGlobalScopeMixin& window_or_worker();
    HTML::WindowOrWorkerGlobalScopeMixin const& window_or_worker() const;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> convert_name_to_timestamp(JS::Realm& realm, String const& name);
    WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> convert_mark_to_timestamp(JS::Realm& realm, Variant<String, HighResolutionTime::DOMHighResTimeStamp> mark);

    GC::Ptr<NavigationTiming::PerformanceNavigation> m_navigation;
    GC::Ptr<NavigationTiming::PerformanceTiming> m_timing;
};

}
