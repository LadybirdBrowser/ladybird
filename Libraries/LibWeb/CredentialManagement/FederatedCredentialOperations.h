/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Origin.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>

namespace Web::CredentialManagement {

GC::Ref<FederatedCredential> create_federated_credential(FederatedCredentialInit, URL::Origin);

}
