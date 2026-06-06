/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibWeb/Bindings/BarProp.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#barprop
class BarProp : public Bindings::Wrappable {
    WEB_WRAPPABLE(BarProp, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(BarProp);

public:
    BarProp(JS::Realm&);
    static GC::Ref<BarProp> create(JS::Realm&);

    [[nodiscard]] bool visible() const;
};

}
