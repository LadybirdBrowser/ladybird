/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/DecodedBytecodeCache.h>
#include <LibJS/ScriptAndModuleABIConversions.h>

namespace JS::RustIntegration {

using namespace EmbeddingABI;

static_assert(to_underlying(ProgramType::Script) == JS_PROGRAM_TYPE_SCRIPT);
static_assert(to_underlying(ProgramType::Module) == JS_PROGRAM_TYPE_MODULE);

RefPtr<DecodedBytecodeCache> DecodedBytecodeCache::create(Core::ImmutableBytes bytes, ProgramType program_type, ReadonlyBytes source_hash)
{
    // The runtime runs the bytecode in place in the blob's bytes, which stay alive until it releases their owner.
    auto* bytes_owner = new Core::ImmutableBytes(move(bytes));
    JSBytecodeCacheBlobOwner owner {
        .owner = bytes_owner,
        .release = [](void* owner) { delete static_cast<Core::ImmutableBytes*>(owner); },
    };
    auto blob = bytes_owner->bytes();
    return adopt_decoded_bytecode_cache_from_abi(js_bytecode_cache_decode(blob.data(), blob.size(), program_type_to_abi(program_type), source_hash.data(), source_hash.size(), owner));
}

void DecodedBytecodeCache::ref() const
{
    js_bytecode_cache_retain(decoded_bytecode_cache_to_abi(*this));
}

void DecodedBytecodeCache::unref() const
{
    js_bytecode_cache_release(decoded_bytecode_cache_to_abi(*this));
}

}
