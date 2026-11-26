/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerToken.h>

namespace Web::Fetch::Engine {

// Answers whether a host is a Known HSTS Host for the fetch the controller token names.
class KnownHSTSHostProvider : public RefCounted<KnownHSTSHostProvider> {
public:
    virtual ~KnownHSTSHostProvider() = default;

    [[nodiscard]] virtual bool is_known_hsts_host(Infrastructure::FetchControllerToken, String const& host) = 0;
};

}
