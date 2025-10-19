/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/SanitizerPrototype.h>
#include <LibWeb/SanitizerAPI/SanitizerConfig.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SanitizerAPI {

// https://wicg.github.io/sanitizer-api/#sanitizer
class Sanitizer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Sanitizer, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Sanitizer);

public:
    static WebIDL::ExceptionOr<GC::Ref<Sanitizer>> construct_impl(JS::Realm&, Optional<Variant<SanitizerConfig, Bindings::SanitizerPresets>> configuration = Bindings::SanitizerPresets::Default);
    virtual ~Sanitizer() override = default;

private:
    explicit Sanitizer(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    enum class AllowCommentsAndDataAttributes {
        Yes,
        No
    };

    // https://wicg.github.io/sanitizer-api/#sanitizer-set-a-configuration
    bool set_a_configuration(SanitizerConfig const&, AllowCommentsAndDataAttributes);
};

}
