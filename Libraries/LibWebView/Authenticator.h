/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/WebAuthentication/AuthenticatorMessages.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://w3c.github.io/webauthn/#public-key-credential-source
struct PublicKeyCredentialSource {
    // https://w3c.github.io/webauthn/#public-key-credential-source-id
    ByteBuffer id;
    // https://w3c.github.io/webauthn/#public-key-credential-source-privatekey
    // NB: This is a P-256 key as a DER PKCS #8 PrivateKeyInfo.
    ByteBuffer private_key;
    // https://w3c.github.io/webauthn/#public-key-credential-source-rpid
    String rp_id;
    // https://w3c.github.io/webauthn/#public-key-credential-source-userhandle
    Optional<ByteBuffer> user_handle;
    // Whether this is a client-side discoverable public key credential source.
    bool is_resident_credential { false };
    // https://w3c.github.io/webauthn/#signature-counter
    // NB: A credential of a virtual authenticator can be one without a signature counter.
    Optional<u32> sign_count { 0 };
    // https://w3c.github.io/webauthn/#backup-eligibility
    bool backup_eligibility { false };
    // https://w3c.github.io/webauthn/#backup-state
    bool backup_state { false };
    Optional<Utf16String> user_name;
    Optional<Utf16String> user_display_name;
    // NB: When the authenticator made the credential and last gave an assertion with it, for the user to tell their
    //     credentials apart. A credential from before the authenticator kept these has none.
    Optional<UnixDateTime> creation_time {};
    Optional<UnixDateTime> last_used_time {};
};

// https://w3c.github.io/webauthn/#authenticator-configuration
struct AuthenticatorConfiguration {
    String protocol { "ctap2"_string };
    String transport { "usb"_string };
    bool has_resident_key { false };
    bool has_user_verification { false };
    // Whether the authenticator counts the assertions of a credential, https://w3c.github.io/webauthn/#signature-counter
    bool has_signature_counter { true };
    bool is_user_consenting { true };
    bool is_user_verified { false };
    bool default_backup_eligibility { false };
    bool default_backup_state { false };
};

// A software authenticator model, https://w3c.github.io/webauthn/#sctn-authenticator-model, which keeps its
// credentials in memory. A WebDriver virtual authenticator is one of these.
class WEBVIEW_API Authenticator {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit Authenticator(AuthenticatorConfiguration);

    AuthenticatorConfiguration const& configuration() const { return m_configuration; }
    void set_user_verified(bool is_user_verified) { m_configuration.is_user_verified = is_user_verified; }

    // https://w3c.github.io/webauthn/#authenticator-attachment-modality
    Web::WebAuthentication::AuthenticatorAttachment attachment() const;

    // https://w3c.github.io/webauthn/#credentials-map
    Vector<PublicKeyCredentialSource> const& credentials() const { return m_credentials; }
    void add_credential(PublicKeyCredentialSource);
    bool remove_credential(ReadonlyBytes credential_id);
    void remove_all_credentials() { m_credentials.clear(); }

    // Whether the bytes are a P-256 private key as a DER PKCS #8 PrivateKeyInfo, which is what a credential has.
    static bool is_credential_private_key(ReadonlyBytes);

    // https://w3c.github.io/webauthn/#credential-id-looking-up
    Optional<PublicKeyCredentialSource&> look_up_credential(ReadonlyBytes credential_id);

    struct MadeCredential {
        // The authenticator that made the credential, and now has it.
        Authenticator& authenticator;
        // Whether the new credential is a client-side discoverable credential.
        bool is_resident_key { false };
        ByteBuffer credential_id;
        ByteBuffer attestation_object;
        ByteBuffer authenticator_data;
        ByteBuffer public_key;
        i32 public_key_algorithm { 0 };
    };

    // Goes through the authenticators for one that makes the credential, as a client does it in
    // https://w3c.github.io/webauthn/#sctn-createCredential with https://w3c.github.io/webauthn/#sctn-op-make-cred
    static ErrorOr<MadeCredential, Web::WebAuthentication::AuthenticatorError> make_credential(Vector<Authenticator&>&, Web::WebAuthentication::MakeCredentialRequest const&);

    struct GotAssertion {
        // The authenticator that made the assertion.
        Authenticator& authenticator;
        ByteBuffer credential_id;
        ByteBuffer authenticator_data;
        ByteBuffer signature;
        Optional<ByteBuffer> user_handle;
    };

    // The credentials that an assertion could use for the request, with the one that the authenticator selects by
    // itself last.
    Vector<PublicKeyCredentialSource const&> credential_options_for(Web::WebAuthentication::GetAssertionRequest const&) const;

    // Goes through the authenticators for one that makes the assertion, as a client does it in
    // https://w3c.github.io/webauthn/#sctn-getAssertion with https://w3c.github.io/webauthn/#sctn-op-get-assertion
    static ErrorOr<GotAssertion, Web::WebAuthentication::AuthenticatorError> get_assertion(Vector<Authenticator&>&, Web::WebAuthentication::GetAssertionRequest const&, ReadonlyBytes client_data_hash);

private:
    AuthenticatorConfiguration m_configuration;
    Vector<PublicKeyCredentialSource> m_credentials;
};

}
