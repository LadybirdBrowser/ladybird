/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CredentialManagement/FederatedCredential.h>

namespace Web::CredentialManagement {

WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> create_federated_credential(JS::Realm& realm, FederatedCredentialInit const&);

}
