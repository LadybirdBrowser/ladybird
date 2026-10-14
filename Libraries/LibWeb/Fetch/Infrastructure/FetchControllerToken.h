/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::Fetch::Infrastructure {

// Names one fetch controller, and only within the registry that issued it.
class FetchControllerToken {
public:
    bool operator==(FetchControllerToken const&) const = default;

private:
    friend class FetchControllerRegistry;

    FetchControllerToken(u64 registry_id, u64 serial)
        : m_registry_id(registry_id)
        , m_serial(serial)
    {
    }

    u64 m_registry_id { 0 };
    u64 m_serial { 0 };
};

}
