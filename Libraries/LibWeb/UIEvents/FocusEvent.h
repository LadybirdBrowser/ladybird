/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/FocusEvent.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

class FocusEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(FocusEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(FocusEvent);

public:
    [[nodiscard]] static GC::Ref<FocusEvent> create(JS::Realm&, FlyString const& event_name, Bindings::FocusEventInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<FocusEvent>> construct_impl(JS::Realm&, FlyString const& event_name, Bindings::FocusEventInit const& event_init);

    virtual ~FocusEvent() override;

private:
    FocusEvent(JS::Realm&, FlyString const& event_name, Bindings::FocusEventInit const&);

    virtual void initialize(JS::Realm&) override;
};

}
