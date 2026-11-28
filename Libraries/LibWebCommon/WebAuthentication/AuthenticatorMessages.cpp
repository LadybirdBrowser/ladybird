/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/WebAuthentication/AuthenticatorMessages.h>

// NB: An enum comes over IPC as its number, which a renderer that is compromised can set to any value.
template<typename Enum>
static ErrorOr<Enum> decode_enum(IPC::Decoder& decoder, Enum last_value)
{
    auto value = TRY(decoder.decode<Enum>());
    if (to_underlying(value) > to_underlying(last_value))
        return Error::from_string_literal("Invalid value of a WebAuthn enum");
    return value;
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAuthentication::MakeCredentialRequest const& message)
{
    TRY(encoder.encode(message.challenge));
    TRY(encoder.encode(message.rp_id));
    TRY(encoder.encode(message.rp_name));
    TRY(encoder.encode(message.user_id));
    TRY(encoder.encode(message.user_name));
    TRY(encoder.encode(message.user_display_name));
    TRY(encoder.encode(message.public_key_algorithms));
    TRY(encoder.encode(message.exclude_credential_ids));
    TRY(encoder.encode(message.authenticator_attachment));
    TRY(encoder.encode(message.resident_key));
    TRY(encoder.encode(message.user_verification));
    TRY(encoder.encode(message.same_origin_with_ancestors));
    TRY(encoder.encode(message.timeout));
    return {};
}

template<>
ErrorOr<Web::WebAuthentication::MakeCredentialRequest> IPC::decode(Decoder& decoder)
{
    Web::WebAuthentication::MakeCredentialRequest message;
    message.challenge = TRY(decoder.decode<ByteBuffer>());
    message.rp_id = TRY(decoder.decode<String>());
    message.rp_name = TRY(decoder.decode<Utf16String>());
    message.user_id = TRY(decoder.decode<ByteBuffer>());
    message.user_name = TRY(decoder.decode<Utf16String>());
    message.user_display_name = TRY(decoder.decode<Utf16String>());
    message.public_key_algorithms = TRY(decoder.decode<Vector<i32>>());
    message.exclude_credential_ids = TRY(decoder.decode<Vector<ByteBuffer>>());
    if (TRY(decoder.decode<bool>()))
        message.authenticator_attachment = TRY(decode_enum(decoder, Web::WebAuthentication::AuthenticatorAttachment::CrossPlatform));
    message.resident_key = TRY(decode_enum(decoder, Web::WebAuthentication::ResidentKeyRequirement::Required));
    message.user_verification = TRY(decode_enum(decoder, Web::WebAuthentication::UserVerificationRequirement::Discouraged));
    message.same_origin_with_ancestors = TRY(decoder.decode<bool>());
    message.timeout = TRY(decoder.decode<Optional<u32>>());
    return message;
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAuthentication::MakeCredentialResponse const& message)
{
    TRY(encoder.encode(message.client_data_json));
    TRY(encoder.encode(message.credential_id));
    TRY(encoder.encode(message.attestation_object));
    TRY(encoder.encode(message.authenticator_data));
    TRY(encoder.encode(message.public_key));
    TRY(encoder.encode(message.public_key_algorithm));
    TRY(encoder.encode(message.transports));
    TRY(encoder.encode(message.authenticator_attachment));
    TRY(encoder.encode(message.is_resident_key));
    return {};
}

template<>
ErrorOr<Web::WebAuthentication::MakeCredentialResponse> IPC::decode(Decoder& decoder)
{
    Web::WebAuthentication::MakeCredentialResponse message;
    message.client_data_json = TRY(decoder.decode<ByteBuffer>());
    message.credential_id = TRY(decoder.decode<ByteBuffer>());
    message.attestation_object = TRY(decoder.decode<ByteBuffer>());
    message.authenticator_data = TRY(decoder.decode<ByteBuffer>());
    message.public_key = TRY(decoder.decode<Optional<ByteBuffer>>());
    message.public_key_algorithm = TRY(decoder.decode<i32>());
    message.transports = TRY(decoder.decode<Vector<String>>());
    message.authenticator_attachment = TRY(decoder.decode<Web::WebAuthentication::AuthenticatorAttachment>());
    message.is_resident_key = TRY(decoder.decode<bool>());
    return message;
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAuthentication::GetAssertionRequest const& message)
{
    TRY(encoder.encode(message.challenge));
    TRY(encoder.encode(message.rp_id));
    TRY(encoder.encode(message.allow_credential_ids));
    TRY(encoder.encode(message.user_verification));
    TRY(encoder.encode(message.same_origin_with_ancestors));
    TRY(encoder.encode(message.timeout));
    TRY(encoder.encode(message.is_conditional));
    return {};
}

template<>
ErrorOr<Web::WebAuthentication::GetAssertionRequest> IPC::decode(Decoder& decoder)
{
    Web::WebAuthentication::GetAssertionRequest message;
    message.challenge = TRY(decoder.decode<ByteBuffer>());
    message.rp_id = TRY(decoder.decode<String>());
    message.allow_credential_ids = TRY(decoder.decode<Vector<ByteBuffer>>());
    message.user_verification = TRY(decode_enum(decoder, Web::WebAuthentication::UserVerificationRequirement::Discouraged));
    message.same_origin_with_ancestors = TRY(decoder.decode<bool>());
    message.timeout = TRY(decoder.decode<Optional<u32>>());
    message.is_conditional = TRY(decoder.decode<bool>());
    return message;
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAuthentication::GetAssertionResponse const& message)
{
    TRY(encoder.encode(message.client_data_json));
    TRY(encoder.encode(message.credential_id));
    TRY(encoder.encode(message.authenticator_data));
    TRY(encoder.encode(message.signature));
    TRY(encoder.encode(message.user_handle));
    TRY(encoder.encode(message.authenticator_attachment));
    return {};
}

template<>
ErrorOr<Web::WebAuthentication::GetAssertionResponse> IPC::decode(Decoder& decoder)
{
    Web::WebAuthentication::GetAssertionResponse message;
    message.client_data_json = TRY(decoder.decode<ByteBuffer>());
    message.credential_id = TRY(decoder.decode<ByteBuffer>());
    message.authenticator_data = TRY(decoder.decode<ByteBuffer>());
    message.signature = TRY(decoder.decode<ByteBuffer>());
    message.user_handle = TRY(decoder.decode<Optional<ByteBuffer>>());
    message.authenticator_attachment = TRY(decoder.decode<Web::WebAuthentication::AuthenticatorAttachment>());
    return message;
}
