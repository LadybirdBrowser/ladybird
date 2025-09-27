/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CredentialManagement/PasswordCredential.h>

namespace Web::CredentialManagement {

WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_password_credential(JS::Realm& realm, GC::Ptr<HTML::HTMLFormElement> const, URL::Origin const& origin);
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_password_credential(JS::Realm& realm, PasswordCredentialData const&);

}
