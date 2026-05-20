/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibGfx/PathSkia.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

Path Path::from_serialized_bytes(ReadonlyBytes bytes)
{
    Gfx::Path path;
    path.impl().deserialize_from_bytes(bytes);
    return path;
}

NonnullOwnPtr<Gfx::PathImpl> PathImpl::create()
{
    return PathImplSkia::create();
}

PathImpl::~PathImpl() = default;

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::Path const& path)
{
    return encoder.encode(path.serialize_to_bytes());
}

template<>
ErrorOr<Gfx::Path> decode(Decoder& decoder)
{
    auto path_data = TRY(decoder.decode<Vector<u8>>());
    return Gfx::Path::from_serialized_bytes(path_data);
}

}
