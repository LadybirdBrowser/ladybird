/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/TimeRanges.h>

namespace Web::MediaSourceExtensions {

struct BufferedChangeEventInit : public DOM::EventInit {
    GC::Ptr<HTML::TimeRanges> added_ranges;
    GC::Ptr<HTML::TimeRanges> removed_ranges;
};

// https://w3c.github.io/media-source/#bufferedchangeevent-interface
class BufferedChangeEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(BufferedChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(BufferedChangeEvent);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<BufferedChangeEvent>> construct_impl(JS::Realm&, FlyString const& type, BufferedChangeEventInit const& = {});

private:
    BufferedChangeEvent(JS::Realm&, FlyString const& type, BufferedChangeEventInit const& event_init);

    virtual ~BufferedChangeEvent() override;

    virtual void initialize(JS::Realm&) override;
};

}
