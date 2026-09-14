/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <AK/StdLibExtras.h>
#include <LibIPC/Forward.h>

namespace IPC {

// What a receiver must be able to answer about one message argument type. Most types ask nothing of the
// receiver, so this is empty. A type that is a claim by the sender about itself specializes it with the
// question the claim is checked against, and with the answer a receiver that tracks nothing gives.
template<typename T>
class SenderClaimReceiver {
};

namespace Detail {

// What a repeated type contributes instead, one per position so that no two of them are the same base.
template<size_t Position>
class RepeatedSenderClaimReceiver {
};

}

// A generated stub inherits these for the argument types its endpoint carries, so a receiver that
// answers for a claim overrides an inherited function instead of having to remember the question. Two
// spellings of one type contribute one base: a type that appears again later in the list is skipped.
template<typename... Ts>
class SenderClaimReceivers;

template<>
class SenderClaimReceivers<> {
};

template<typename T, typename... Rest>
class SenderClaimReceivers<T, Rest...> : public SenderClaimReceivers<Rest...>
    , public Conditional<(IsSame<T, Rest> || ...), Detail::RepeatedSenderClaimReceiver<sizeof...(Rest)>, SenderClaimReceiver<T>> {
};

// Most argument types say nothing about the sender, so verifying one accepts anything. A type that is a
// claim declares its own verify_sender_claim() beside the type, and argument-dependent lookup finds that
// one instead of this.
template<typename Stub, typename T>
bool verify_sender_claim(Stub&, T const&)
{
    return true;
}

// What generated receive code calls for every decoded argument, before the message is dispatched.
template<typename Stub, typename T>
bool verify_message_argument(Stub& stub, T const& argument)
{
    return verify_sender_claim(stub, argument);
}

// The reply a refused synchronous message is answered with: every output value-initialized, so the
// sender learns nothing from the refusal. A reply that cannot be built empty is not sent at all.
template<typename Response, typename... Outputs>
ErrorOr<OwnPtr<MessageBuffer>> refused_reply()
{
    if constexpr (requires { Response { Outputs {}... }; }) {
        auto response = Response { Outputs {}... };
        return make<MessageBuffer>(TRY(response.encode()));
    } else {
        return Error::from_string_literal("Refused message has no empty reply");
    }
}

}
