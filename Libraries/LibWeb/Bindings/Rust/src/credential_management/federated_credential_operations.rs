/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_rust::api::Vm;
use liburl_rust::origin::Origin;

use super::federated_credential::{FederatedCredential, FederatedCredentialInit};
use crate::bindings::interface::{Ref, allocate};

/// https://w3c.github.io/webappsec-credential-management/#abstract-opdef-create-a-federatedcredential-from-federatedcredentialinit
pub(crate) fn create_federated_credential(
    vm: &Vm,
    init: FederatedCredentialInit,
    origin: Origin,
) -> Ref<FederatedCredential> {
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
    allocate(vm, |_| FederatedCredential::new(init, origin))
}
