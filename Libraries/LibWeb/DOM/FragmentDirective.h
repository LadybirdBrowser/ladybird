/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>

namespace Web::DOM {

// https://wicg.github.io/scroll-to-text-fragment/#feature-detectability
class FragmentDirective final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(FragmentDirective, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(FragmentDirective);

public:
    static GC::Ref<FragmentDirective> create();
    virtual ~FragmentDirective() override = default;

private:
    FragmentDirective() = default;
};

}
