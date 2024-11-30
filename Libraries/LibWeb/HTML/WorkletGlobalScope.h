/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/worklets.html#workletglobalscope
class WorkletGlobalScope : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WorkletGlobalScope, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WorkletGlobalScope);

public:
    virtual ~WorkletGlobalScope() override;

protected:
    explicit WorkletGlobalScope(JS::Realm&);
};

}
