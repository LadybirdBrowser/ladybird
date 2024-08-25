/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

class ServiceWorkerContainer : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(ServiceWorkerContainer, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(ServiceWorkerContainer);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ServiceWorkerContainer> create(JS::Realm& realm);
    virtual ~ServiceWorkerContainer() override = default;

    WebIDL::CallbackType* onmessage();
    void set_onmessage(WebIDL::CallbackType*);

private:
    explicit ServiceWorkerContainer(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
