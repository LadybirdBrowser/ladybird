/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/SanitizerPrototype.h>

namespace Web::SanitizerAPI {

// https://wicg.github.io/sanitizer-api/#sanitizer
class Sanitizer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Sanitizer, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Sanitizer);

public:
    virtual ~Sanitizer() override = default;

private:
    explicit Sanitizer(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
