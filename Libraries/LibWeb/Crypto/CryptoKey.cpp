/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Crypto/CryptoKey.h>

namespace Web::Crypto {

extern "C" void web_rust_crypto_key_remove_algorithm_hash_for_testing(CryptoKey*);

void CryptoKey::remove_algorithm_hash_for_testing()
{
    web_rust_crypto_key_remove_algorithm_hash_for_testing(this);
}

}
