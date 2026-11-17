/*
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>

namespace Crypto::PK {

ErrorOr<bool> ecdsa_verify(ReadonlyBytes x, ReadonlyBytes y, ReadonlyBytes digest, ReadonlyBytes r, ReadonlyBytes s);

}
