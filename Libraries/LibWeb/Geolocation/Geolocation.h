/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#position_options_interface
struct PositionOptions {
    bool enable_high_accuracy;
    WebIDL::UnsignedLong timeout;
    WebIDL::UnsignedLong maximum_age;
};

// https://w3c.github.io/geolocation/#geolocation_interface
class Geolocation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Geolocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Geolocation);

public:
    [[nodiscard]] static GC::Ref<Geolocation> create(JS::Realm&);

    void get_current_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback = nullptr, Optional<PositionOptions> options = {});
    WebIDL::Long watch_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback = nullptr, Optional<PositionOptions> options = {});
    void clear_watch(WebIDL::Long watch_id);

private:
    Geolocation(JS::Realm&);
    virtual ~Geolocation() override;

    virtual void initialize(JS::Realm&) override;
};
}
