/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PageTransitionEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

using PageTransitionEventInit = Bindings::PageTransitionEventInit;

class PageTransitionEvent final : public DOM::Event {
    WEB_WRAPPABLE(PageTransitionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(PageTransitionEvent);

public:
    [[nodiscard]] static GC::Ref<PageTransitionEvent> create(FlyString const& event_name, PageTransitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~PageTransitionEvent() override;

    bool persisted() const { return m_persisted; }

private:
    PageTransitionEvent(FlyString const& event_name, PageTransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    bool m_persisted { false };
};

}
