/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ScreenOrientation.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

using OrientationLockType = Bindings::OrientationLockType;
using OrientationType = Bindings::OrientationType;

class ScreenOrientation final : public DOM::EventTarget {
    WEB_WRAPPABLE(ScreenOrientation, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(ScreenOrientation);

public:
    [[nodiscard]] static GC::Ref<ScreenOrientation> create();

    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> lock(JS::Realm&, OrientationLockType);
    WebIDL::ExceptionOr<void> lock();
    void unlock();
    OrientationType type() const;
    WebIDL::UnsignedShort angle() const;

    void set_onchange(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onchange();

private:
    ScreenOrientation();
};

}
