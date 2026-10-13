/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>

#if !defined(AK_OS_WINDOWS)
#    error "The handle broker is only needed on Windows, where a socket cannot carry a handle"
#endif

namespace IPC {

using HandleBrokerToken = u64;
using HandleBrokerChannel = u64;

// Holds the handles of a message while it sits in a socket: the sender parks them and writes the token it gets, and the
// reader redeems that token for them. The Browser holds the broker, so it alone ever places a handle into another
// process, and a token can only name handles that the process parking them owned.
class HandleBroker {
public:
    virtual ~HandleBroker() = default;

    // A process that was not given a client, such as the Browser or a test, serves itself with a LocalHandleBroker.
    static HandleBroker& the();
    static void install(NonnullOwnPtr<HandleBroker>);

    virtual bool is_local() const { return false; }

    // The channel is the transport end that writes the token, so that what it parked can be forgotten once nobody
    // can read from it anymore.
    virtual ErrorOr<HandleBrokerToken> park(HandleBrokerChannel, Vector<Attachment>) = 0;
    virtual ErrorOr<Vector<Attachment>> redeem(HandleBrokerToken) = 0;
    virtual void forget_channel(HandleBrokerChannel) = 0;
};

class LocalHandleBroker final : public HandleBroker {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual bool is_local() const override { return true; }

    virtual ErrorOr<HandleBrokerToken> park(HandleBrokerChannel, Vector<Attachment>) override;
    virtual ErrorOr<Vector<Attachment>> redeem(HandleBrokerToken) override;
    virtual void forget_channel(HandleBrokerChannel) override;

    ErrorOr<HandleBrokerToken> park_on_behalf_of(pid_t owner, HandleBrokerChannel, Vector<Attachment>);
    void forget_channel_of(pid_t owner, HandleBrokerChannel);

private:
    struct ParkedAttachments {
        pid_t owner { -1 };
        HandleBrokerChannel channel { 0 };
        Vector<Attachment> attachments;
    };

    Mutex m_mutex;
    HashMap<HandleBrokerToken, ParkedAttachments> m_parked_attachments;
};

// Asks the Browser's broker over a transport that carries nothing else. Any thread may ask; the requests take turns.
class HandleBrokerClient final : public HandleBroker {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit HandleBrokerClient(NonnullOwnPtr<Transport>);
    virtual ~HandleBrokerClient() override;

    virtual ErrorOr<HandleBrokerToken> park(HandleBrokerChannel, Vector<Attachment>) override;
    virtual ErrorOr<Vector<Attachment>> redeem(HandleBrokerToken) override;
    virtual void forget_channel(HandleBrokerChannel) override;

private:
    ErrorOr<HandleBrokerResponse> exchange(HandleBrokerRequest const&);

    Mutex m_mutex;
    NonnullOwnPtr<Transport> m_transport;
};

}
