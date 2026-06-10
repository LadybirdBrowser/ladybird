/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PageSwapEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class NavigationActivation;
class Window;

}

namespace Web::ViewTransition {

class ViewTransition;

}

namespace Web::HTML {

using PageSwapEventInit = Bindings::PageSwapEventInit;

class PageSwapEvent final : public DOM::Event {
    WEB_WRAPPABLE(PageSwapEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(PageSwapEvent);

public:
    [[nodiscard]] static GC::Ref<PageSwapEvent> create(FlyString const& event_name, PageSwapEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~PageSwapEvent() override;

    GC::Ptr<NavigationActivation> activation() const { return m_activation; }
    GC::Ptr<ViewTransition::ViewTransition> view_transition() const { return m_view_transition; }

private:
    PageSwapEvent(FlyString const& event_name, PageSwapEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<NavigationActivation> m_activation;
    GC::Ptr<ViewTransition::ViewTransition> m_view_transition;
};

}
