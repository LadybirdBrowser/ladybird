/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibJS/Forward.h>

namespace JS::Bytecode {

class Executable;

// Whether the bytecode being validated still has m_cache fields stored as
// indices (BeforeFixup) or has had Executable::fixup_cache_pointers() rewrite
// them into live pointers (AfterFixup). Cache fields are only range-checked
// in the BeforeFixup case.
enum class CacheState : u8 {
    BeforeFixup,
    AfterFixup,
};

ErrorOr<void> validate_bytecode(Executable const&, CacheState);

}
