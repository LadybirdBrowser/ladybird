/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>

namespace Crypto::Curves {

ErrorOr<bool> ed25519_verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message);

}
