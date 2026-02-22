/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CredentialManagement/FederatedCredential.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-create-a-federatedcredential-from-federatedcredentialinit
WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> create_federated_credential(JS::Realm& realm, FederatedCredentialInit const&);

}
