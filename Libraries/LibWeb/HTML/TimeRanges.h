/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/media.html#time-ranges
class TimeRanges final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TimeRanges, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TimeRanges);

public:
    // https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-length
    size_t length() const;

    // https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-start
    WebIDL::ExceptionOr<double> start(u32 index) const;

    // https://html.spec.whatwg.org/multipage/media.html#dom-timeranges-end
    WebIDL::ExceptionOr<double> end(u32 index) const;

    void add_range(double start, double end);
    bool in_range(double);

private:
    explicit TimeRanges(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    struct Range {
        double start;
        double end;
    };

    Vector<Range> m_ranges;
};

}
