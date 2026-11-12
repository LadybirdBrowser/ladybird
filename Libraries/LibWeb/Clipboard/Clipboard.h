/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Clipboard.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::Clipboard {

class Clipboard : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Clipboard, clipboard)

public:
    static GC::Ref<Clipboard> create(JS::Realm&);
};

}
