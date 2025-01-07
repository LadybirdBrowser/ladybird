/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PerformanceMarkPrototype.h>
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

PerformanceMark::PerformanceMark(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, JS::Value detail)
    : PerformanceTimeline::PerformanceEntry(realm, name, start_time, duration)
    , m_detail(detail)
{
}

PerformanceMark::~PerformanceMark() = default;

// https://w3c.github.io/user-timing/#dfn-performancemark-constructor
WebIDL::ExceptionOr<GC::Ref<PerformanceMark>> PerformanceMark::construct_impl(JS::Realm& realm, String const& mark_name, Web::UserTiming::PerformanceMarkOptions const& mark_options)
{
    auto& current_principal_global_object = HTML::current_principal_global_object();
    auto& vm = realm.vm();

    // 1. If the current global object is a Window object and markName uses the same name as a read only attribute in the PerformanceTiming interface, throw a SyntaxError.
    if (is<HTML::Window>(current_principal_global_object)) {
        bool matched = false;

#define __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME(name, _) \
    if (mark_name == NavigationTiming::EntryNames::name)  \
        matched = true;
        ENUMERATE_NAVIGATION_TIMING_ENTRY_NAMES
#undef __ENUMERATE_NAVIGATION_TIMING_ENTRY_NAME

        if (matched)
            return WebIDL::SyntaxError::create(realm, MUST(String::formatted("'{}' markName cannot be used in a Window context because it is part of the PerformanceTiming interface", mark_name)));
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
        start_time = HighResolutionTime::current_high_resolution_time(current_principal_global_object);
    }

    // 6. Set entry's duration attribute to 0.
    constexpr HighResolutionTime::DOMHighResTimeStamp duration = 0.0;

    // 7. If markOptions's detail is null, set entry's detail to null.
    JS::Value detail;
    if (mark_options.detail.is_null()) {
        detail = JS::js_null();
    }
    // 8. Otherwise:
    else {
        // 1. Let record be the result of calling the StructuredSerialize algorithm on markOptions's detail.
        auto record = TRY(HTML::structured_serialize(vm, mark_options.detail));

        // 2. Set entry's detail to the result of calling the StructuredDeserialize algorithm on record and the current realm.
        detail = TRY(HTML::structured_deserialize(vm, record, realm));
    }

    // 2. Create a new PerformanceMark object (entry) with the current global object's realm.
    return realm.create<PerformanceMark>(realm, name, start_time, duration, detail);
}

FlyString const& PerformanceMark::entry_type() const
{
    return PerformanceTimeline::EntryTypes::mark;
}

void PerformanceMark::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceMark);
}

void PerformanceMark::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_detail);
}

}
