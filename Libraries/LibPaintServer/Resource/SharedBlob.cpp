/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPaintServer/Resource/SharedBlob.h>

namespace PaintServer {

ErrorOr<SharedBlob> SharedBlob::create_from_bytes(ReadonlyBytes bytes)
{
    if (bytes.is_empty())
        return Error::from_string_literal("SharedBlob cannot be empty");

    SharedBlob blob;
    blob.m_buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size()));
    __builtin_memcpy(blob.m_buffer.data<void>(), bytes.data(), bytes.size());
    return blob;
}

ErrorOr<IPC::File> SharedBlob::clone_file() const
{
    if (!m_buffer.is_valid())
        return Error::from_string_literal("SharedBlob is not valid");

    return IPC::File::clone_fd(m_buffer.fd());
}

}
