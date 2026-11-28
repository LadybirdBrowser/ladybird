/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::WebAuthentication {

// https://w3c.github.io/webauthn/#enumdef-authenticatorattachment
enum class AuthenticatorAttachment : u8 {
    Platform,
    CrossPlatform,
};

// https://w3c.github.io/webauthn/#enumdef-residentkeyrequirement
enum class ResidentKeyRequirement : u8 {
    Discouraged,
    Preferred,
    Required,
};

// https://w3c.github.io/webauthn/#enumdef-userverificationrequirement
enum class UserVerificationRequirement : u8 {
    Required,
    Preferred,
    Discouraged,
};

// Why a ceremony gave no credential, named after the DOMException that the page gets.
enum class AuthenticatorError : u8 {
    NotAllowed,
    InvalidState,
    Security,
    NotSupported,
    Constraint,
};

// What a page asks for when it creates a credential. The origin isn't among the fields: the browser process takes it
// from the document that it knows the request came from, and builds the client data with it.
struct MakeCredentialRequest {
    ByteBuffer challenge;
    String rp_id;
    Utf16String rp_name;
    ByteBuffer user_id;
    Utf16String user_name;
    Utf16String user_display_name;
    // The COSEAlgorithmIdentifiers of pubKeyCredParams whose type is "public-key", in the order of preference.
    Vector<i32> public_key_algorithms;
    Vector<ByteBuffer> exclude_credential_ids;
    Optional<AuthenticatorAttachment> authenticator_attachment;
    ResidentKeyRequirement resident_key { ResidentKeyRequirement::Discouraged };
    UserVerificationRequirement user_verification { UserVerificationRequirement::Preferred };
    bool same_origin_with_ancestors { true };
    // The time in milliseconds that the Relying Party is willing to wait for the ceremony.
    Optional<u32> timeout {};
};

struct MakeCredentialResponse {
    ByteBuffer client_data_json;
    ByteBuffer credential_id;
    ByteBuffer attestation_object;
    ByteBuffer authenticator_data;
    Optional<ByteBuffer> public_key;
    i32 public_key_algorithm { 0 };
    Vector<String> transports;
    AuthenticatorAttachment authenticator_attachment { AuthenticatorAttachment::CrossPlatform };
    // Whether the new credential is a client-side discoverable credential.
    bool is_resident_key { false };
};

// What a page asks for when it gets an assertion.
struct GetAssertionRequest {
    ByteBuffer challenge;
    String rp_id;
    Vector<ByteBuffer> allow_credential_ids;
    UserVerificationRequirement user_verification { UserVerificationRequirement::Preferred };
    bool same_origin_with_ancestors { true };
    // The time in milliseconds that the Relying Party is willing to wait for the ceremony.
    Optional<u32> timeout {};
    // Whether the mediation is conditional: The request then waits, without a prompt, for the user to use a form
    // control that is tagged with the "webauthn" autofill detail token.
    bool is_conditional { false };
};

struct GetAssertionResponse {
    ByteBuffer client_data_json;
    ByteBuffer credential_id;
    ByteBuffer authenticator_data;
    ByteBuffer signature;
    Optional<ByteBuffer> user_handle;
    AuthenticatorAttachment authenticator_attachment { AuthenticatorAttachment::CrossPlatform };
};

using MakeCredentialResult = Variant<MakeCredentialResponse, AuthenticatorError>;
using GetAssertionResult = Variant<GetAssertionResponse, AuthenticatorError>;

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::WebAuthentication::MakeCredentialRequest const&);
template<>
WEBCOMMON_API ErrorOr<Web::WebAuthentication::MakeCredentialRequest> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::WebAuthentication::MakeCredentialResponse const&);
template<>
WEBCOMMON_API ErrorOr<Web::WebAuthentication::MakeCredentialResponse> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::WebAuthentication::GetAssertionRequest const&);
template<>
WEBCOMMON_API ErrorOr<Web::WebAuthentication::GetAssertionRequest> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::WebAuthentication::GetAssertionResponse const&);
template<>
WEBCOMMON_API ErrorOr<Web::WebAuthentication::GetAssertionResponse> decode(Decoder&);

}
