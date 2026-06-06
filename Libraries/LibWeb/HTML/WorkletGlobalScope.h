/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/worklets.html#workletglobalscope
class WorkletGlobalScope : public Bindings::Wrappable {
    WEB_WRAPPABLE(WorkletGlobalScope, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(WorkletGlobalScope);

public:
    virtual ~WorkletGlobalScope() override;

protected:
    explicit WorkletGlobalScope(JS::Realm&);
};

}
