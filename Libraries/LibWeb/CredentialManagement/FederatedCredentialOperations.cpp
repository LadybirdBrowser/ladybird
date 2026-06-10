/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CredentialManagement/FederatedCredentialOperations.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-create-a-federatedcredential-from-federatedcredentialinit
GC::Ref<FederatedCredential> create_federated_credential(FederatedCredentialInit init, URL::Origin origin)
{
    // 1. Let c be a new FederatedCredential object.
    // 3. Set c’s properties as follows:
    //    - id
    //      - init.id's value
    //    - provider
    //      - init.provider's value
    //    - iconURL
    //      - init.iconURL's value
    //    - name
    //      - init.name's value
    //    - [[origin]]
    //      - init.origin's value.
    //      NOTE: origin is retrieved by parsing the URL from init.provider.
    // 4. Return c.
    return GC::Heap::the().allocate<FederatedCredential>(move(init), move(origin));
}

}
