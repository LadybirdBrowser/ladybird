/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/File.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

class SharedBlob {
public:
    static ErrorOr<SharedBlob> create_from_bytes(ReadonlyBytes);
    bool is_valid() const { return m_buffer.is_valid(); }
    size_t size() const { return m_buffer.size(); }
    ReadonlyBytes bytes() const { return m_buffer.bytes(); }
    ErrorOr<IPC::File> clone_file() const;

private:
    Core::AnonymousBuffer m_buffer;
};

}
