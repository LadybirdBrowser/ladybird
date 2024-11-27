/*
 * Copyright (c) 2024, Miguel Sacristán <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NavigatorCredentialsContainer.h"

#include <LibWeb/HTML/Navigator.h>

namespace Web::Credentials {
GC::Ref<CredentialsContainer> NavigatorCredentialsContainer::credentials()
{
    auto const& navigator = verify_cast<HTML::Navigator>(*this);
    auto& realm = navigator.realm();
    if (!m_credentials_container)
        m_credentials_container = realm.create<CredentialsContainer>(realm);
    return *m_credentials_container;
}
}
