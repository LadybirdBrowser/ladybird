/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CredentialManagement/PasswordCredential.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-create-a-passwordcredential-from-an-htmlformelement
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_password_credential(JS::Realm& realm, GC::Ptr<HTML::HTMLFormElement> const&, URL::Origin const&);

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-create-a-passwordcredential-from-passwordcredentialdata
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_password_credential(JS::Realm& realm, PasswordCredentialData const&, URL::Origin const&);

}
