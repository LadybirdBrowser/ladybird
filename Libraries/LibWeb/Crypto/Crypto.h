/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Crypto.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::Crypto {

class Crypto : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Crypto, crypto)

public:
    [[nodiscard]] static GC::Ref<Crypto> create();
};

WEB_API String generate_random_uuid();

}
