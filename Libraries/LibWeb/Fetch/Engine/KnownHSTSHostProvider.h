/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>

namespace Web::Fetch::Infrastructure {

class FetchParams;

}

namespace Web::Fetch::Engine {

class KnownHSTSHostProvider : public RefCounted<KnownHSTSHostProvider> {
public:
    virtual ~KnownHSTSHostProvider() = default;

    [[nodiscard]] virtual Optional<bool> known_answer(String const& host) const = 0;

    virtual void look_up(Infrastructure::FetchParams const&, String const& host, Function<void(bool is_known)> on_answer) = 0;
};

}
