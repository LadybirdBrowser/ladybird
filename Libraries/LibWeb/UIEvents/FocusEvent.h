/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/FocusEvent.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

using FocusEventInit = Bindings::FocusEventInit;

class FocusEvent final : public UIEvent {
    WEB_WRAPPABLE(FocusEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(FocusEvent);

public:
    [[nodiscard]] static GC::Ref<FocusEvent> create(FlyString const& event_name, FocusEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~FocusEvent() override;

private:
    FocusEvent(FlyString const& event_name, FocusEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
