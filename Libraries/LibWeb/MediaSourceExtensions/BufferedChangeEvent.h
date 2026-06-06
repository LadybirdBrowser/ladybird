/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/BufferedChangeEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/TimeRanges.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#bufferedchangeevent-interface
class BufferedChangeEvent : public DOM::Event {
    WEB_WRAPPABLE(BufferedChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(BufferedChangeEvent);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<BufferedChangeEvent>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, FlyString const& type, Bindings::BufferedChangeEventInit const& = {});

private:
    BufferedChangeEvent(FlyString const& type, Bindings::BufferedChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~BufferedChangeEvent() override;
};

}
