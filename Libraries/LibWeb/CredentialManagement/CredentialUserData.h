/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#credentialuserdata
class CredentialUserData {
public:
    virtual ~CredentialUserData() = default;

    String const& name() const { return m_name; }
    String const& icon_url() const { return m_icon_url; }

protected:
    CredentialUserData(String name, String icon_url)
        : m_name(move(name))
        , m_icon_url(move(icon_url))
    {
    }

    String m_name;
    String m_icon_url;
};

}
