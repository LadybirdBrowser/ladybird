/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CredentialsContainer.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::CredentialManagement {

class CredentialsContainer : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CredentialsContainer, credentials_container)

public:
    [[nodiscard]] static GC::Ref<CredentialsContainer> create();
};

}
