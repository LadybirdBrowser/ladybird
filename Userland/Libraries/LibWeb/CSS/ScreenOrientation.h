/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/ScreenOrientationPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

class ScreenOrientation final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(ScreenOrientation, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(ScreenOrientation);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ScreenOrientation> create(JS::Realm&);

    WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> lock(Bindings::OrientationLockType);
    void unlock();
    Bindings::OrientationType type() const;
    WebIDL::UnsignedShort angle() const;

    void set_onchange(JS::GCPtr<WebIDL::CallbackType>);
    JS::GCPtr<WebIDL::CallbackType> onchange();

private:
    explicit ScreenOrientation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
