/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#dictdef-documenttimelineoptions
struct DocumentTimelineOptions {
    HighResolutionTime::DOMHighResTimeStamp origin_time { 0.0 };
};

// https://www.w3.org/TR/web-animations-1/#the-documenttimeline-interface
class DocumentTimeline : public AnimationTimeline {
    WEB_PLATFORM_OBJECT(DocumentTimeline, AnimationTimeline);
    GC_DECLARE_ALLOCATOR(DocumentTimeline);

public:
    static GC::Ref<DocumentTimeline> create(JS::Realm&, DOM::Document&, HighResolutionTime::DOMHighResTimeStamp origin_time);
    static WebIDL::ExceptionOr<GC::Ref<DocumentTimeline>> construct_impl(JS::Realm&, DocumentTimelineOptions options = {});

    virtual void set_current_time(Optional<double> current_time) override;
    virtual bool is_inactive() const override;

    virtual Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<double>) override;
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const override { return true; }

private:
    DocumentTimeline(JS::Realm&, DOM::Document&, HighResolutionTime::DOMHighResTimeStamp origin_time);
    virtual ~DocumentTimeline() override = default;

    virtual void initialize(JS::Realm&) override;

    HighResolutionTime::DOMHighResTimeStamp m_origin_time;
};

}
