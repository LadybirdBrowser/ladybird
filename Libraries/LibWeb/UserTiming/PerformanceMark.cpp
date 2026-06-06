/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/EntryNames.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/UserTiming/PerformanceMark.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UserTiming {

GC_DEFINE_ALLOCATOR(PerformanceMark);

PerformanceMark::PerformanceMark(String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail)
    : PerformanceTimeline::PerformanceEntry(name, start_time, duration)
    , m_detail(detail)
{
}

PerformanceMark::~PerformanceMark() = default;

GC::Ref<PerformanceMark> PerformanceMark::create(String const& mark_name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail)
{
    return GC::Heap::the().allocate<PerformanceMark>(mark_name, start_time, duration, detail);
}

// https://w3c.github.io/user-timing/#dfn-performancemark-constructor
WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> PerformanceMark::construct_impl(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, String const& mark_name, Bindings::PerformanceMarkOptions const& mark_options)
{
    auto& realm = HTML::relevant_realm(global_scope);
    auto& vm = realm.vm();

    // 1. If the current global object is a Window object and markName uses the same name as a read only attribute in the PerformanceTiming interface, throw a SyntaxError.
    if (is<HTML::Window>(global_scope)) {
        bool matched = false;

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _) \
    if (mark_name == NavigationTiming::EntryNames::name)  \
        matched = true;
        ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

        if (matched)
            return WebIDL::SyntaxError::create(realm, Utf16String::formatted("'{}' markName cannot be used in a Window context because it is part of the PerformanceTiming interface", mark_name));
    }

    // NOTE: Step 2 (creating the entry) is done after determining values, as we set the values once during creation and never change them after.

    // 3. Set entry's name attribute to markName.
    auto const& name = mark_name;

    // 4. Set entry's entryType attribute to DOMString "mark".
    // NOTE: Already done via the `entry_type` virtual function.

    // 5. Set entry's startTime attribute as follows:
    HighResolutionTime::DOMHighResTimeStamp start_time { 0.0 };

    // 1. If markOptions's startTime member is present, then:
    if (mark_options.start_time.has_value()) {
        // 1. If markOptions's startTime is negative, throw a TypeError.
        if (mark_options.start_time.value() < 0.0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "startTime cannot be negative"sv };

        // 2. Otherwise, set entry's startTime to the value of markOptions's startTime.
        start_time = mark_options.start_time.value();
    }
    // 2. Otherwise, set it to the value that would be returned by the Performance object's now() method.
    else {
        start_time = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope));
    }

    // 6. Set entry's duration attribute to 0.
    constexpr HighResolutionTime::DOMHighResTimeStamp duration = 0.0;

    // 7. If markOptions's detail is null, set entry's detail to null.
    JS::Value detail;
    if (!mark_options.detail.has_value() || mark_options.detail->is_null()) {
        detail = JS::js_null();
    }
    // 8. Otherwise:
    else {
        // 1. Let record be the result of calling the StructuredSerialize algorithm on markOptions's detail.
        auto record = TRY(HTML::structured_serialize(realm, *mark_options.detail));

        // 2. Set entry's detail to the result of calling the StructuredDeserialize algorithm on record and the current realm.
        detail = TRY(HTML::structured_deserialize(vm, record, realm));
    }

    // 2. Create a new PerformanceMark object (entry).
    return PerformanceMark::create(name, start_time, duration, detail);
}

FlyString const& PerformanceMark::entry_type() const
{
    return PerformanceTimeline::EntryTypes::mark;
}

void PerformanceMark::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_detail);
}

}
