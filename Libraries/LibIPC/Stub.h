/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <LibIPC/Forward.h>

namespace AK {
class BufferStream;
}

namespace IPC {

class ConnectionBase;

class Stub {
public:
    virtual ~Stub() = default;

    virtual u32 magic() const = 0;
    virtual ByteString name() const = 0;
    virtual void handle_ipc_message(NonnullRefPtr<IPC::ConnectionBase> conn, NonnullOwnPtr<IPC::Message> message) = 0;

protected:
    Stub() = default;

private:
    ByteString m_name;
};

}
