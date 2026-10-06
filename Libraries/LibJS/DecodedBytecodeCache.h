/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCore/ImmutableBytes.h>
#include <LibJS/Export.h>

namespace JS::RustIntegration {

enum class ProgramType : u8 {
    Script = 0,
    Module = 1,
};

// A bytecode cache blob, decoded and checked against the program type and the source hash it was written for. A
// pointer to it is the Rust runtime's decoded cache itself, which is reference counted like the C++ runtime's
// RefCounted<DecodedBytecodeCache>: the thread that decoded it may hand it to the VM's thread while nothing else
// references it, and from then on only the VM's thread may reference or release it.
class JS_API DecodedBytecodeCache {
    AK_MAKE_NONCOPYABLE(DecodedBytecodeCache);
    AK_MAKE_NONMOVABLE(DecodedBytecodeCache);

public:
    // Null for a blob of another format version, runtime, program type or source hash, and for a malformed one.
    static RefPtr<DecodedBytecodeCache> create(Core::ImmutableBytes, ProgramType, ReadonlyBytes source_hash);

    void ref() const;
    void unref() const;

    DecodedBytecodeCache() = delete;
    ~DecodedBytecodeCache() = delete;
};

}

template<>
inline constexpr bool AllocatedWithCustomAllocator<JS::RustIntegration::DecodedBytecodeCache> = true;
