/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Crypto/CryptoKey.h>

namespace Web::Crypto {

extern "C" JSCompletion web_rust_crypto_key_from_value(JSVM*, JSValue, CryptoKey**);

JS::ThrowCompletionOr<GC::Ref<CryptoKey>> crypto_key_from_value(JS::VM& vm, JS::Value value)
{
    CryptoKey* key = nullptr;
    TRY(JS::HostABI::completion_from_abi<void>(web_rust_crypto_key_from_value(
        JS::HostABI::vm_to_abi(vm), JS::HostABI::value_to_abi(value), &key)));
    return GC::Ref { *key };
}

}
