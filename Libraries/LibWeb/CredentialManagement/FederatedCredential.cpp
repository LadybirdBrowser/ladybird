/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/CredentialManagement/FederatedCredentialOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(FederatedCredential);

// https://www.w3.org/TR/credential-management-1/#dom-federatedcredential-federatedcredential
WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> FederatedCredential::create(FederatedCredentialInit const& data)
{
    // 1. Let r be the result of executing Create a FederatedCredential from FederatedCredentialInit on data. If that
    // threw an exception, rethrow that exception.
    // 2. Return r.
    if (data.id.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "'id' must not be empty."sv };
    if (data.provider.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "'provider' must not be empty."sv };

    // AD-HOC: Aligning with how Chromium retrieves the origin by parsing the URL from data.provider.
    auto url = URL::Parser::basic_parse(data.provider);
    if (!url.has_value())
        return WebIDL::SyntaxError::create("'provider' is not a valid url."_utf16);

    return create_federated_credential(data, url->origin());
}

FederatedCredential::~FederatedCredential()
{
}

FederatedCredential::FederatedCredential(FederatedCredentialInit init, URL::Origin origin)
    : Credential(move(init.id))
    , CredentialUserData(init.name.value_or(String {}), init.icon_url.value_or(String {}))
    , m_provider(move(init.provider))
    , m_protocol(move(init.protocol))
    , m_origin(move(origin))
{
}

}
