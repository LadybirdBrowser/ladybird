/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/CredentialManagement/FederatedCredentialOperations.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-create-a-federatedcredential-from-federatedcredentialinit
WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> create_federated_credential(JS::Realm& realm, FederatedCredentialInit const& init)
{
    // 1. Let c be a new FederatedCredential object.
    // 2. If any of the following are the empty string, throw a TypeError exception:
    //    - init.id's value
    //    - init.provider's value
    if (init.id.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "'id' must not be empty."sv };
    if (init.provider.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "'provider' must not be empty."sv };

    // AD-HOC: Aligning with how Chromium retrieves the origin by parsing the URL from init.provider.
    auto url = URL::Parser::basic_parse(init.provider);
    if (!url.has_value())
        WebIDL::SyntaxError::create(realm, "'provider' is not a valid url."_utf16);
    auto origin = url.value().origin();

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
    return realm.create<FederatedCredential>(realm, init, move(origin));
}

}
