/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/PerformanceMark.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/NavigationTiming/EntryNames.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/UserTiming/PerformanceMark.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::UserTiming {

GC_DEFINE_ALLOCATOR(PerformanceMark);

PerformanceMark::PerformanceMark(Utf16String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail)
    : PerformanceTimeline::PerformanceEntry(name, start_time, duration)
    , m_detail(detail)
{
}

PerformanceMark::~PerformanceMark() = default;

// https://w3c.github.io/user-timing/#dfn-performancemark-constructor
WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> PerformanceMark::create_for_constructor(JS::Object& relevant_global_object, Utf16String const& mark_name, Bindings::PerformanceMarkOptions const& mark_options)
{
    auto& current_global_object = HTML::current_global_object();
    auto& vm = relevant_global_object.vm();

    // 1. If the current global object is a Window object and markName uses the same name as a read only attribute in the PerformanceTiming interface, throw a SyntaxError.
    if (is<HTML::Window>(current_global_object)) {
        bool matched = false;

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _)                    \
    if (mark_name.utf16_view() == NavigationTiming::EntryNames::name.view()) \
        matched = true;
        ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

        if (matched)
            return WebIDL::SyntaxError::create(Utf16String::formatted("'{}' markName cannot be used in a Window context because it is part of the PerformanceTiming interface", mark_name));
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
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "startTime cannot be negative"_utf16 };

        // 2. Otherwise, set entry's startTime to the value of markOptions's startTime.
        start_time = mark_options.start_time.value();
    }
    // 2. Otherwise, set it to the value that would be returned by the Performance object's now() method.
    else {
        start_time = HighResolutionTime::current_high_resolution_time(current_global_object);
    }

    // 6. Set entry's duration attribute to 0.
    constexpr HighResolutionTime::DOMHighResTimeStamp duration = 0.0;

    // 2. Create a new PerformanceMark object (entry).
    JS::Value detail;
    if (!mark_options.detail.has_value() || mark_options.detail->is_null()) {
        detail = JS::js_null();
    } else {
        auto record = TRY(HTML::structured_serialize(vm, *mark_options.detail));
        detail = TRY(HTML::structured_deserialize(vm, record, HTML::relevant_realm(relevant_global_object)));
    }

    return GC::Heap::the().allocate<PerformanceMark>(name, start_time, duration, detail);
}

Utf16FlyString const& PerformanceMark::entry_type() const
{
    return PerformanceTimeline::EntryTypes::mark;
}

WebIDL::ExceptionOr<JS::Value> PerformanceMark::detail(JS::Object const& relevant_global_object) const
{
    auto& relevant_realm = HTML::relevant_realm(relevant_global_object);
    auto serialized = TRY(HTML::structured_serialize(relevant_realm.vm(), m_detail));
    return HTML::structured_deserialize(relevant_realm.vm(), serialized, relevant_realm);
}

void PerformanceMark::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_detail);
}

}
