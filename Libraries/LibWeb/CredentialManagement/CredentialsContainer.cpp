/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>

namespace Web::CredentialManagement {

extern "C" {
CredentialsContainer* web_rust_credentials_container_create(JSVM*);
}

GC::Ref<CredentialsContainer> CredentialsContainer::create()
{
    return *web_rust_credentials_container_create(JS::HostABI::vm_to_abi(JS::VM::the()));
}

}
