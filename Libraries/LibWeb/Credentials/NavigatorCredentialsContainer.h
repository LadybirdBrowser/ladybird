/*
 * Copyright (c) 2024, Miguel Sacristán <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Credentials/CredentialsContainer.h>

namespace Web::Credentials {
class NavigatorCredentialsContainerMixin {
public:
    virtual ~NavigatorCredentialsContainerMixin() = default;

    [[nodiscard]] GC::Ref<CredentialsContainer> credentials();

protected:
    virtual Bindings::PlatformObject const& this_navigator_storage_object() const = 0;

private:
    GC::Ptr<CredentialsContainer> m_credentials_container;
};
}
