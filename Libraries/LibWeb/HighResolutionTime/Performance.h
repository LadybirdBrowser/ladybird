/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibCore/ElapsedTimer.h>
#include <LibWeb/Bindings/PerformanceMeasure.h>
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

    WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMark>> mark(Utf16String const& mark_name, Bindings::PerformanceMarkOptions const& mark_options = {});
    void clear_marks(Optional<Utf16String> const& mark_name);
    WebIDL::ExceptionOr<GC::Ref<UserTiming::PerformanceMeasure>> measure(Utf16String const& measure_name, Variant<Utf16String, Bindings::PerformanceMeasureOptions> const& start_or_measure_options, Optional<Utf16String> end_mark);
    void clear_measures(Optional<Utf16String> const& measure_name);

    void clear_resource_timings();
    void set_resource_timing_buffer_size(u32 max_size);
    void set_onresourcetimingbufferfull(WebIDL::CallbackType*);
    WebIDL::CallbackType* onresourcetimingbufferfull();

    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries() const;
    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries_by_type(Utf16FlyString const& type) const;
    WebIDL::ExceptionOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> get_entries_by_name(Utf16String const& name, Optional<Utf16FlyString> type) const;

    GC::Ptr<NavigationTiming::PerformanceTiming> timing();
    GC::Ptr<NavigationTiming::PerformanceNavigation> navigation();

private:
    explicit Performance(JS::Realm&);

    HTML::WindowOrWorkerGlobalScopeMixin& window_or_worker();
    HTML::WindowOrWorkerGlobalScopeMixin const& window_or_worker() const;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> convert_name_to_timestamp(JS::Realm& realm, Utf16String const& name);
    WebIDL::ExceptionOr<HighResolutionTime::DOMHighResTimeStamp> convert_mark_to_timestamp(JS::Realm& realm, Variant<Utf16String, HighResolutionTime::DOMHighResTimeStamp> const& mark);

    GC::Ptr<NavigationTiming::PerformanceNavigation> m_navigation;
    GC::Ptr<NavigationTiming::PerformanceTiming> m_timing;
};

}
