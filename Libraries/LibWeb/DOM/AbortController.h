/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AbortController.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class WEB_API AbortController final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AbortController, abort_controller)

public:
    static GC::Ref<AbortController> create();
    GC::Ref<AbortSignal> signal() const;
    void abort(JS::Realm&, Optional<JS::Value> reason);
};

}
