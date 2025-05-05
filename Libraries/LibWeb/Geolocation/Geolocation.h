/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geolocation/GeolocationPosition.h>
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
    virtual void visit_edges(Cell::Visitor&) override;

    void request_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback = nullptr, Optional<PositionOptions> options = {}, Optional<WebIDL::Long> watch_id = {});
    void acquire_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback = nullptr, Optional<PositionOptions> options = {}, Optional<WebIDL::Long> watch_id = {});
    void call_back_with_error(WebIDL::CallbackType* error_callback, WebIDL::UnsignedShort code);

    // https://w3c.github.io/geolocation/#dfn-cachedposition
    Optional<GC::Ptr<GeolocationPosition>> m_cached_position;

    // https://w3c.github.io/geolocation/#dfn-watchids
    HashTable<WebIDL::Long> m_watch_ids;
    WebIDL::Long m_next_watch_id { 0 };
    HashMap<WebIDL::Long, u64> m_watch_request_ids;
};

}
