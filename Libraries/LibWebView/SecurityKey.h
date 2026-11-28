/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebCommon/WebAuthentication/AuthenticatorMessages.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Why a security key gave no credential.
enum class SecurityKeyError : u8 {
    // The key holds a credential of the exclude list.
    CredentialExcluded,
    // The key has no credential for the request.
    NoCredentials,
    // The key verifies the user with a PIN, which the client must ask for.
    PinRequired,
    // The key did not take the PIN.
    PinInvalid,
    // The key takes no more PINs, until the user connects it again or resets it.
    PinBlocked,
    // The key cannot do what the request needs.
    NotSupported,
    Failed,
};

// NB: The requests go to another thread, so their text is UTF-8 in buffers of their own.
struct SecurityKeyMakeCredentialRequest {
    ByteBuffer client_data_hash;
    ByteBuffer rp_id;
    ByteBuffer rp_name;
    ByteBuffer user_id;
    ByteBuffer user_name;
    ByteBuffer user_display_name;
    Vector<i32> public_key_algorithms;
    Vector<ByteBuffer> exclude_credential_ids;
    Web::WebAuthentication::ResidentKeyRequirement resident_key { Web::WebAuthentication::ResidentKeyRequirement::Discouraged };
    Web::WebAuthentication::UserVerificationRequirement user_verification { Web::WebAuthentication::UserVerificationRequirement::Preferred };
    // The PIN of the security key, when the user gave it. It is empty otherwise.
    ByteBuffer pin;
    // The key that the PIN is for. No other key gets the PIN.
    Optional<u64> pin_device_id {};
};

struct SecurityKeyAttestation {
    ByteBuffer credential_id;
    ByteBuffer authenticator_data;
    // https://w3c.github.io/webauthn/#attestation-object
    ByteBuffer attestation_object;
    // The DER SubjectPublicKeyInfo of the credential public key, when the client knows its algorithm.
    Optional<ByteBuffer> public_key;
    i32 public_key_algorithm { 0 };
    bool is_resident_key { false };
};

struct SecurityKeyGetAssertionRequest {
    ByteBuffer client_data_hash;
    ByteBuffer rp_id;
    Vector<ByteBuffer> allow_credential_ids;
    Web::WebAuthentication::UserVerificationRequirement user_verification { Web::WebAuthentication::UserVerificationRequirement::Preferred };
    // The PIN of the security key, when the user gave it. It is empty otherwise.
    ByteBuffer pin;
    // The key that the PIN is for. No other key gets the PIN.
    Optional<u64> pin_device_id {};
};

struct SecurityKeyAssertion {
    ByteBuffer credential_id;
    ByteBuffer authenticator_data;
    ByteBuffer signature;
    Optional<ByteBuffer> user_handle;
    // NB: UTF-8, as the key gave it.
    ByteBuffer user_name;
    ByteBuffer user_display_name;
};

// One request to the security keys that are connected over USB, or that the user connects while it waits. It goes to
// all of them, and the key that the user touches answers it. The work happens on a thread of its own, and the result
// comes to the event loop that started the session.
class WEBVIEW_API SecurityKeySession : public AtomicRefCounted<SecurityKeySession> {
public:
    using OnAttestation = Function<void(ErrorOr<SecurityKeyAttestation, SecurityKeyError>)>;
    // The assertions of all the credentials of the key that fit the request.
    using OnAssertions = Function<void(ErrorOr<Vector<SecurityKeyAssertion>, SecurityKeyError>)>;

    static NonnullRefPtr<SecurityKeySession> make_credential(SecurityKeyMakeCredentialRequest, OnAttestation);
    static NonnullRefPtr<SecurityKeySession> get_assertion(SecurityKeyGetAssertionRequest, OnAssertions);

    // Stops the request. The session then gives no result.
    void cancel();

    // The key that the user chose, when the session ended because that key needs its PIN or did not take the PIN.
    Optional<u64> device_that_wants_pin() const { return m_device_that_wants_pin; }

    // Whether the client can reach security keys on this platform.
    static bool is_supported();
    static bool is_any_key_connected();

private:
    SecurityKeySession() = default;

    Atomic<bool> m_is_cancelled { false };
    Optional<u64> m_device_that_wants_pin;
    OnAttestation m_on_attestation;
    OnAssertions m_on_assertions;
};

}
