/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

struct FocusEventInit : public UIEventInit {
    JS::GCPtr<DOM::EventTarget> related_target;
};

class FocusEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(FocusEvent, UIEvent);
    JS_DECLARE_ALLOCATOR(FocusEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<FocusEvent> create(JS::Realm&, FlyString const& event_name, FocusEventInit const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<FocusEvent>> construct_impl(JS::Realm&, FlyString const& event_name, FocusEventInit const& event_init);

    virtual ~FocusEvent() override;

private:
    FocusEvent(JS::Realm&, FlyString const& event_name, FocusEventInit const&);

    virtual void initialize(JS::Realm&) override;
};

}
