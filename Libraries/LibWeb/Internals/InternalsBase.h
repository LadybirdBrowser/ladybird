/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::Internals {

class InternalsBase : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(InternalsBase, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(InternalsBase);

public:
    virtual ~InternalsBase() override;

protected:
    explicit InternalsBase(JS::Realm&);

    HTML::Window& window() const;
    Page& page() const;
};

}
