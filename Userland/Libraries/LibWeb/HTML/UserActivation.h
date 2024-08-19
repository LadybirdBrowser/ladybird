/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class UserActivation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(UserActivation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(UserActivation);

public:
    static WebIDL::ExceptionOr<GC::Ref<UserActivation>> construct_impl(JS::Realm&);
    virtual ~UserActivation() override = default;

    bool has_been_active() const;
    bool is_active() const;

private:
    UserActivation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
