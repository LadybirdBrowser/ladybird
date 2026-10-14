/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <LibCore/System.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/HandleBroker.h>
#include <LibIPC/HandleBrokerMessages.h>
#include <LibIPC/Limits.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>

namespace IPC {

static NeverDestroyed<Mutex> s_installed_broker_mutex;
static NeverDestroyed<OwnPtr<HandleBroker>> s_installed_broker;

static constexpr size_t MAX_HANDLE_BROKER_MESSAGE_SIZE = sizeof(HandleBrokerOperation) + sizeof(HandleBrokerToken) + sizeof(HandleBrokerChannel) + sizeof(u32) + MAX_MESSAGE_FD_COUNT_WITH_SHARED_MEMORY * sizeof(u64);
static_assert(MAX_HANDLE_BROKER_MESSAGE_SIZE <= TransportWindows::MAX_INLINE_PAYLOAD_SIZE);

HandleBroker& HandleBroker::the()
{
    MutexLocker locker(*s_installed_broker_mutex);
    if (!*s_installed_broker)
        *s_installed_broker = make<LocalHandleBroker>();
    return **s_installed_broker;
}

void HandleBroker::install(NonnullOwnPtr<HandleBroker> broker)
{
    MutexLocker locker(*s_installed_broker_mutex);
    VERIFY(!*s_installed_broker);
    *s_installed_broker = move(broker);
}

static ErrorOr<void> encode_handle_values(Encoder& encoder, Vector<u64> const& handle_values)
{
    TRY(encoder.encode(static_cast<u32>(handle_values.size())));
    for (auto value : handle_values)
        TRY(encoder.encode(value));
    return {};
}

static ErrorOr<Vector<u64>> decode_handle_values(Decoder& decoder)
{
    auto count = TRY(decoder.decode<u32>());
    if (count > MAX_MESSAGE_FD_COUNT_WITH_SHARED_MEMORY)
        return Error::from_string_literal("Handle broker message names too many handles");

    Vector<u64> handle_values;
    TRY(handle_values.try_ensure_capacity(count));
    for (u32 i = 0; i < count; ++i)
        handle_values.unchecked_append(TRY(decoder.decode<u64>()));
    return handle_values;
}

static ErrorOr<HandleBrokerOperation> decode_operation(Decoder& decoder)
{
    auto operation = TRY(decoder.decode<u8>());
    if (operation > to_underlying(HandleBrokerOperation::ForgetChannel))
        return Error::from_string_literal("Unknown handle broker operation");
    return static_cast<HandleBrokerOperation>(operation);
}

template<>
ErrorOr<void> encode(Encoder& encoder, HandleBrokerRequest const& request)
{
    TRY(encoder.encode(to_underlying(request.operation)));
    TRY(encoder.encode(request.token));
    TRY(encoder.encode(request.channel));
    return encode_handle_values(encoder, request.handle_values);
}

template<>
ErrorOr<HandleBrokerRequest> decode(Decoder& decoder)
{
    HandleBrokerRequest request;
    request.operation = TRY(decode_operation(decoder));
    request.token = TRY(decoder.decode<HandleBrokerToken>());
    request.channel = TRY(decoder.decode<HandleBrokerChannel>());
    request.handle_values = TRY(decode_handle_values(decoder));
    return request;
}

template<>
ErrorOr<void> encode(Encoder& encoder, HandleBrokerResponse const& response)
{
    TRY(encoder.encode(to_underlying(response.operation)));
    TRY(encoder.encode(response.error));
    TRY(encoder.encode(response.token));
    return encode_handle_values(encoder, response.handle_values);
}

template<>
ErrorOr<HandleBrokerResponse> decode(Decoder& decoder)
{
    HandleBrokerResponse response;
    response.operation = TRY(decode_operation(decoder));
    response.error = TRY(decoder.decode<i32>());
    response.token = TRY(decoder.decode<HandleBrokerToken>());
    response.handle_values = TRY(decode_handle_values(decoder));
    return response;
}

ErrorOr<HandleBrokerToken> LocalHandleBroker::park(HandleBrokerChannel channel, Vector<Attachment> attachments)
{
    return park_on_behalf_of(Core::System::getpid(), channel, move(attachments));
}

ErrorOr<HandleBrokerToken> LocalHandleBroker::park_on_behalf_of(pid_t owner, HandleBrokerChannel channel, Vector<Attachment> attachments)
{
    MutexLocker locker(m_mutex);

    // Whoever holds a token may redeem it, so it must not be guessable. Zero stands for no token.
    HandleBrokerToken token = 0;
    while (token == 0 || m_parked_attachments.contains(token))
        token = get_random<HandleBrokerToken>();

    TRY(m_parked_attachments.try_set(token, ParkedAttachments { owner, channel, move(attachments) }));
    return token;
}

ErrorOr<Vector<Attachment>> LocalHandleBroker::redeem(HandleBrokerToken token)
{
    MutexLocker locker(m_mutex);
    auto parked_attachments = m_parked_attachments.take(token);
    if (!parked_attachments.has_value())
        return Error::from_errno(ENOENT);
    return move(parked_attachments->attachments);
}

void LocalHandleBroker::forget_channel(HandleBrokerChannel channel)
{
    forget_channel_of(Core::System::getpid(), channel);
}

void LocalHandleBroker::forget_channel_of(pid_t owner, HandleBrokerChannel channel)
{
    MutexLocker locker(m_mutex);
    m_parked_attachments.remove_all_matching([&](auto const&, auto const& parked_attachments) {
        return parked_attachments.owner == owner && parked_attachments.channel == channel;
    });
}

HandleBrokerClient::HandleBrokerClient(NonnullOwnPtr<Transport> transport)
    : m_transport(move(transport))
{
}

HandleBrokerClient::~HandleBrokerClient() = default;

ErrorOr<HandleBrokerToken> HandleBrokerClient::park(HandleBrokerChannel channel, Vector<Attachment> attachments)
{
    HandleBrokerRequest request;
    request.operation = HandleBrokerOperation::Park;
    request.channel = channel;
    TRY(request.handle_values.try_ensure_capacity(attachments.size()));
    for (auto const& attachment : attachments)
        request.handle_values.unchecked_append(static_cast<u64>(attachment.fd()));

    // The attachments stay open until the response confirms that the Browser holds copies of them.
    auto response = TRY(exchange(request));
    return response.token;
}

ErrorOr<Vector<Attachment>> HandleBrokerClient::redeem(HandleBrokerToken token)
{
    HandleBrokerRequest request;
    request.operation = HandleBrokerOperation::Redeem;
    request.token = token;
    auto response = TRY(exchange(request));

    Vector<Attachment> attachments;
    TRY(attachments.try_ensure_capacity(response.handle_values.size()));
    for (auto value : response.handle_values)
        attachments.unchecked_append(Attachment::from_fd(static_cast<int>(value)));
    return attachments;
}

void HandleBrokerClient::forget_channel(HandleBrokerChannel channel)
{
    HandleBrokerRequest request;
    request.operation = HandleBrokerOperation::ForgetChannel;
    request.channel = channel;
    if (auto result = exchange(request); result.is_error())
        dbgln("HandleBrokerClient: Could not forget channel {}: {}", channel, result.error());
}

ErrorOr<HandleBrokerResponse> HandleBrokerClient::exchange(HandleBrokerRequest const& request)
{
    MutexLocker locker(m_mutex);
    if (!m_transport->is_open())
        return Error::from_string_literal("The handle broker is gone");

    MessageBuffer buffer;
    Encoder encoder(buffer);
    TRY(encoder.encode(request));
    TRY(buffer.transfer_message(*m_transport));

    // Requests take turns, so the next message to arrive answers this one.
    Optional<ErrorOr<HandleBrokerResponse>> response;
    while (!response.has_value()) {
        if (!m_transport->is_open())
            return Error::from_string_literal("The handle broker is gone");
        m_transport->wait_until_readable();

        auto should_shutdown = m_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
            if (response.has_value()) {
                response = ErrorOr<HandleBrokerResponse> { Error::from_string_literal("The handle broker answered a request more than once") };
                return;
            }
            FixedMemoryStream stream { message.bytes.bytes() };
            Decoder decoder { stream, message.attachments };
            response = decoder.decode<HandleBrokerResponse>();
        });

        if (should_shutdown == Transport::ShouldShutdown::Yes) {
            m_transport->close();
            if (!response.has_value())
                return Error::from_string_literal("The handle broker closed the connection");
        }
    }

    auto decoded_response = TRY(response.release_value());
    if (decoded_response.error != 0)
        return Error::from_errno(decoded_response.error);
    if (decoded_response.operation != request.operation)
        return Error::from_string_literal("The handle broker answered a different request");
    return decoded_response;
}

}
