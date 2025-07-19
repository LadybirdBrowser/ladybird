/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/CredentialUserData.h>

namespace Web::CredentialManagement {

String const& CredentialUserData::name()
{
    return m_name;
}

String const& CredentialUserData::icon_url()
{
    return m_icon_url;
}

}
