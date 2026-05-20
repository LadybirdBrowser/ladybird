/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <LibJS/Forward.h>

namespace JS::Bytecode {

class Executable;

ErrorOr<void> validate_bytecode(Executable const&, ReadonlySpan<u32> basic_block_offsets);

}
