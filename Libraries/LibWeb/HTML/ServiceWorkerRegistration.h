/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

class ServiceWorkerRegistration : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(ServiceWorkerRegistration, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(ServiceWorkerRegistration);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ServiceWorkerRegistration> create(JS::Realm& realm);

    explicit ServiceWorkerRegistration(JS::Realm&);
    virtual ~ServiceWorkerRegistration() override = default;

    virtual void initialize(JS::Realm&) override;
};

}
