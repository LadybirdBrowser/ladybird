/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Bindings/DocumentTimeline.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class Window;

}

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#the-documenttimeline-interface
class DocumentTimeline : public AnimationTimeline {
    WEB_WRAPPABLE(DocumentTimeline, AnimationTimeline);
    GC_DECLARE_ALLOCATOR(DocumentTimeline);

public:
    static GC::Ref<DocumentTimeline> create(DOM::Document&, HighResolutionTime::DOMHighResTimeStamp origin_time);
    static WebIDL::ExceptionOr<GC::Ref<DocumentTimeline>> construct_impl(HTML::Window&, Bindings::DocumentTimelineOptions options = {});

    virtual Optional<TimeValue> duration() const override { return {}; }

    virtual void update_current_time(double timestamp) override;
    virtual bool is_inactive() const override;

    virtual Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<TimeValue>) override;
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const override { return true; }

private:
    DocumentTimeline(DOM::Document&, HighResolutionTime::DOMHighResTimeStamp origin_time);
    virtual ~DocumentTimeline() override = default;

    HighResolutionTime::DOMHighResTimeStamp m_origin_time;
};

}
