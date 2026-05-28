/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCore/ImmutableBytes.h>
#include <LibJS/Export.h>

namespace JS::FFI {

struct DecodedBytecodeCacheBlob;

}

namespace JS::RustIntegration {

enum class ProgramType : u8 {
    Script = 0,
    Module = 1,
};

class JS_API DecodedBytecodeCache final : public RefCounted<DecodedBytecodeCache> {
public:
    static RefPtr<DecodedBytecodeCache> create(Core::ImmutableBytes, ProgramType, ReadonlyBytes source_hash);
    static NonnullRefPtr<DecodedBytecodeCache> create(FFI::DecodedBytecodeCacheBlob*);
    ~DecodedBytecodeCache();

    FFI::DecodedBytecodeCacheBlob* create_materialization_handle() const;

private:
    explicit DecodedBytecodeCache(FFI::DecodedBytecodeCacheBlob*);

    FFI::DecodedBytecodeCacheBlob* m_blob { nullptr };
};

}
