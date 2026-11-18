/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::Crypto {

class WEB_API CryptoKey final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CryptoKey, crypto_key)

public:
    void remove_algorithm_hash_for_testing();
};

}
