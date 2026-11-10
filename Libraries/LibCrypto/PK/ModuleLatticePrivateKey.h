/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>

namespace Crypto::PK {

struct ModuleLatticePrivateKey {
    ReadonlyBytes seed;
    ReadonlyBytes expanded_key;
};

ErrorOr<ModuleLatticePrivateKey> parse_module_lattice_private_key(ReadonlyBytes der, size_t seed_size, size_t expanded_key_size);

}
