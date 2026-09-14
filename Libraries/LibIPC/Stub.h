/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <LibIPC/Forward.h>

namespace AK {

class BufferStream;

}

namespace IPC {

class Stub {
public:
    virtual ~Stub() = default;

    virtual u32 magic() const = 0;
    virtual ByteString name() const = 0;
    virtual ErrorOr<OwnPtr<MessageBuffer>> handle(NonnullOwnPtr<Message>) = 0;

    // Called when a message fails verification before dispatch. A connection that can act on a
    // misbehaving peer overrides this.
    virtual void did_misbehave(StringView message_name, StringView reason)
    {
        dbgln("IPC: {} rejected: {}", message_name, reason);
    }

protected:
    Stub() = default;
};

}
