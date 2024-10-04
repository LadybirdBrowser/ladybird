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
    JS_DECLARE_ALLOCATOR(Performance);

public:
    virtual ~Performance() override;

    double now() const { return static_cast<double>(m_timer.elapsed_time().to_nanoseconds()) / 1e6; }
    double time_origin() const;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<UserTiming::PerformanceMark>> mark(String const& mark_name, UserTiming::PerformanceMarkOptions const& mark_options = {});
    void clear_marks(Optional<String> mark_name);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<UserTiming::PerformanceMeasure>> measure(String const& measure_name, Variant<String, UserTiming::PerformanceMeasureOptions> const& start_or_measure_options, Optional<String> end_mark);
    void clear_measures(Optional<String> measure_name);

    WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> get_entries() const;
    WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> get_entries_by_type(String const& type) const;
    WebIDL::ExceptionOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> get_entries_by_name(String const& name, Optional<String> type) const;

    JS::GCPtr<NavigationTiming::PerformanceTiming> timing();
    JS::GCPtr<NavigationTiming::PerformanceNavigation> navigation();

private:
    explicit Performance(JS::Realm&);

    HTML::WindowOrWorkerGlobalScopeMixin& window_or_worker();
    HTML::WindowOrWorkerGlobalScopeMixin const& window_or_worker() const;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> convert_name_to_timestamp(JS::Realm& realm, String const& name);
    WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> convert_mark_to_timestamp(JS::Realm& realm, Variant<String, HighResolutionTime::DOMHighResTimeStamp> mark);

    JS::GCPtr<NavigationTiming::PerformanceNavigation> m_navigation;
    JS::GCPtr<NavigationTiming::PerformanceTiming> m_timing;

    Core::ElapsedTimer m_timer;
};

}
