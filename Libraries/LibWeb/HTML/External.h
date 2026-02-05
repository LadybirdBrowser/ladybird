/*
 * Copyright (c) 2026, Reimar Pihl Browa <mail@reim.ar>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/obsolete.html#external
class WEB_API External final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(External, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(External);

public:
    [[nodiscard]] static GC::Ref<External> create(JS::Realm&);

    virtual ~External() override;

    void add_search_provider();
    void is_search_provider_installed();

private:
    External(JS::Realm&);

    void initialize(JS::Realm&) override;
};

}
