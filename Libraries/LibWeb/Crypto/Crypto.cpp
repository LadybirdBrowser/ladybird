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
void web_crypto_fill_with_random(Bytes);
void web_crypto_generate_random_uuid(void*, void (*)(void*, ReadonlyBytes));
}

GC::Ref<Crypto> Crypto::create()
{
    return *web_rust_crypto_create(JS::HostABI::vm_to_abi(JS::VM::the()));
}

void web_crypto_fill_with_random(Bytes bytes)
{
    fill_with_random(bytes);
}

void web_crypto_generate_random_uuid(void* state, void (*receive)(void*, ReadonlyBytes))
{
    auto uuid = generate_random_uuid();
    receive(state, uuid.bytes());
}

String generate_random_uuid()
{
    return AK::generate_random_uuid();
}

}
