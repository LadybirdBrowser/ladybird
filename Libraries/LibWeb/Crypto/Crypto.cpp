/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Crypto/Crypto.h>

namespace Web::Crypto {

extern "C" {
Crypto* web_rust_crypto_create(JSVM*);
}

GC::Ref<Crypto> Crypto::create()
{
    return *web_rust_crypto_create(JS::HostABI::vm_to_abi(JS::VM::the()));
}

String generate_random_uuid()
{
    return AK::generate_random_uuid();
}

}
