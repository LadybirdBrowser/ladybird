/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Authenticator.h>
#include <RustFFI.h>

namespace WebView {

using Web::WebAuthentication::AuthenticatorAttachment;
using Web::WebAuthentication::AuthenticatorError;

// NB: The operations themselves are in Rust (Libraries/LibWebView/Rust/src/authenticator.rs). The authenticator keeps
//     the credentials, and gives them to each operation.

static FFI::FfiBytes ffi_bytes(ReadonlyBytes bytes)
{
    return { bytes.data(), bytes.size() };
}

static ByteBuffer byte_buffer(FFI::FfiBytes bytes)
{
    return MUST(ByteBuffer::copy(bytes.data, bytes.length));
}

static Vector<FFI::FfiBytes> ffi_byte_strings(Vector<ByteBuffer> const& buffers)
{
    Vector<FFI::FfiBytes> byte_strings;
    byte_strings.ensure_capacity(buffers.size());
    for (auto const& buffer : buffers)
        byte_strings.unchecked_append(ffi_bytes(buffer));
    return byte_strings;
}

static Vector<FFI::FfiCredential> ffi_credentials(Vector<PublicKeyCredentialSource> const& credentials)
{
    Vector<FFI::FfiCredential> ffi_credentials;
    ffi_credentials.ensure_capacity(credentials.size());
    for (auto const& credential : credentials) {
        ffi_credentials.unchecked_append({
            .id = ffi_bytes(credential.id),
            .private_key = ffi_bytes(credential.private_key),
            .rp_id = ffi_bytes(credential.rp_id.bytes()),
            .has_user_handle = credential.user_handle.has_value(),
            .user_handle = credential.user_handle.has_value() ? ffi_bytes(*credential.user_handle) : ffi_bytes({}),
            .is_resident_credential = credential.is_resident_credential,
            .has_sign_count = credential.sign_count.has_value(),
            .sign_count = credential.sign_count.value_or(0),
            .backup_eligibility = credential.backup_eligibility,
            .backup_state = credential.backup_state,
        });
    }
    return ffi_credentials;
}

static FFI::AuthenticatorConfiguration ffi_configuration(AuthenticatorConfiguration const& configuration)
{
    return {
        .has_resident_key = configuration.has_resident_key,
        .has_user_verification = configuration.has_user_verification,
        .has_signature_counter = configuration.has_signature_counter,
        .is_user_consenting = configuration.is_user_consenting,
        .is_user_verified = configuration.is_user_verified,
        .default_backup_eligibility = configuration.default_backup_eligibility,
        .default_backup_state = configuration.default_backup_state,
    };
}

static AuthenticatorError authenticator_error(FFI::AuthenticatorError error)
{
    switch (error) {
    case FFI::AuthenticatorError::NotAllowed:
        return AuthenticatorError::NotAllowed;
    case FFI::AuthenticatorError::InvalidState:
        return AuthenticatorError::InvalidState;
    case FFI::AuthenticatorError::NotSupported:
        return AuthenticatorError::NotSupported;
    case FFI::AuthenticatorError::Constraint:
        return AuthenticatorError::Constraint;
    }
    VERIFY_NOT_REACHED();
}

static FFI::UserVerificationRequirement ffi_user_verification_requirement(Web::WebAuthentication::UserVerificationRequirement requirement)
{
    switch (requirement) {
    case Web::WebAuthentication::UserVerificationRequirement::Required:
        return FFI::UserVerificationRequirement::Required;
    case Web::WebAuthentication::UserVerificationRequirement::Preferred:
        return FFI::UserVerificationRequirement::Preferred;
    case Web::WebAuthentication::UserVerificationRequirement::Discouraged:
        return FFI::UserVerificationRequirement::Discouraged;
    }
    VERIFY_NOT_REACHED();
}

static FFI::ResidentKeyRequirement ffi_resident_key_requirement(Web::WebAuthentication::ResidentKeyRequirement requirement)
{
    switch (requirement) {
    case Web::WebAuthentication::ResidentKeyRequirement::Discouraged:
        return FFI::ResidentKeyRequirement::Discouraged;
    case Web::WebAuthentication::ResidentKeyRequirement::Preferred:
        return FFI::ResidentKeyRequirement::Preferred;
    case Web::WebAuthentication::ResidentKeyRequirement::Required:
        return FFI::ResidentKeyRequirement::Required;
    }
    VERIFY_NOT_REACHED();
}

static FFI::AuthenticatorAttachment ffi_attachment(AuthenticatorAttachment attachment)
{
    return attachment == AuthenticatorAttachment::Platform ? FFI::AuthenticatorAttachment::Platform : FFI::AuthenticatorAttachment::CrossPlatform;
}

static FFI::FfiCeremonyGetAssertionRequest ffi_get_assertion_request(Web::WebAuthentication::GetAssertionRequest const& request, Vector<FFI::FfiBytes> const& allow_credential_ids, ReadonlyBytes client_data_hash)
{
    return {
        .rp_id = ffi_bytes(request.rp_id.bytes()),
        .allow_credential_ids = allow_credential_ids.data(),
        .allow_credential_id_count = allow_credential_ids.size(),
        .user_verification = ffi_user_verification_requirement(request.user_verification),
        .is_conditional = request.is_conditional,
        .client_data_hash = ffi_bytes(client_data_hash),
    };
}

// The authenticators as a ceremony sees them. The credentials that they point to live in the lists beside them.
struct CeremonyAuthenticators {
    Vector<Vector<FFI::FfiCredential>> credentials;
    Vector<FFI::FfiAuthenticator> authenticators;
};

static CeremonyAuthenticators ceremony_authenticators(Vector<Authenticator&> const& authenticators)
{
    CeremonyAuthenticators result;
    result.credentials.ensure_capacity(authenticators.size());
    result.authenticators.ensure_capacity(authenticators.size());
    for (auto const& authenticator : authenticators) {
        result.credentials.unchecked_append(ffi_credentials(authenticator.credentials()));
        auto const& credentials = result.credentials.last();
        result.authenticators.unchecked_append({
            .configuration = ffi_configuration(authenticator.configuration()),
            .attachment = ffi_attachment(authenticator.attachment()),
            .credentials = credentials.data(),
            .credential_count = credentials.size(),
        });
    }
    return result;
}

Authenticator::Authenticator(AuthenticatorConfiguration configuration)
    : m_configuration(move(configuration))
{
}

AuthenticatorAttachment Authenticator::attachment() const
{
    // NB: An authenticator with the internal transport is a platform authenticator, and any other is a roaming one.
    return m_configuration.transport == "internal"sv ? AuthenticatorAttachment::Platform : AuthenticatorAttachment::CrossPlatform;
}

void Authenticator::add_credential(PublicKeyCredentialSource credential)
{
    remove_credential(credential.id);
    m_credentials.append(move(credential));
}

bool Authenticator::remove_credential(ReadonlyBytes credential_id)
{
    return m_credentials.remove_all_matching([&](auto const& credential) { return credential.id.bytes() == credential_id; });
}

Optional<PublicKeyCredentialSource&> Authenticator::look_up_credential(ReadonlyBytes credential_id)
{
    for (auto& credential : m_credentials) {
        if (credential.id.bytes() == credential_id)
            return credential;
    }
    return {};
}

bool Authenticator::is_credential_private_key(ReadonlyBytes private_key)
{
    return FFI::webview_authenticator_is_credential_private_key(ffi_bytes(private_key));
}

// https://w3c.github.io/webauthn/#sctn-createCredential
ErrorOr<Authenticator::MadeCredential, AuthenticatorError> Authenticator::make_credential(Vector<Authenticator&>& authenticators, Web::WebAuthentication::MakeCredentialRequest const& request)
{
    auto ceremony = ceremony_authenticators(authenticators);
    auto exclude_credential_ids = ffi_byte_strings(request.exclude_credential_ids);
    FFI::FfiCeremonyMakeCredentialRequest ffi_request {
        .rp_id = ffi_bytes(request.rp_id.bytes()),
        .user_id = ffi_bytes(request.user_id),
        .public_key_algorithms = request.public_key_algorithms.data(),
        .public_key_algorithm_count = request.public_key_algorithms.size(),
        .exclude_credential_ids = exclude_credential_ids.data(),
        .exclude_credential_id_count = exclude_credential_ids.size(),
        .has_authenticator_attachment = request.authenticator_attachment.has_value(),
        .authenticator_attachment = ffi_attachment(request.authenticator_attachment.value_or(AuthenticatorAttachment::Platform)),
        .resident_key = ffi_resident_key_requirement(request.resident_key),
        .user_verification = ffi_user_verification_requirement(request.user_verification),
    };

    struct Made {
        size_t authenticator { 0 };
        bool is_resident_key { false };
        PublicKeyCredentialSource credential_source;
        ByteBuffer attestation_object;
        ByteBuffer authenticator_data;
        ByteBuffer public_key;
        i32 public_key_algorithm { 0 };
        Vector<size_t> displaced_credentials;
    };
    Optional<Made> made;
    auto outcome = FFI::webview_ceremony_make_credential(ceremony.authenticators.data(), ceremony.authenticators.size(), &ffi_request, &made, [](void* context, FFI::FfiNewCredential const* new_credential) {
        Made made;
        made.authenticator = new_credential->authenticator;
        made.is_resident_key = new_credential->is_resident_key;
        made.credential_source.id = byte_buffer(new_credential->id);
        made.credential_source.private_key = byte_buffer(new_credential->private_key);
        made.credential_source.backup_eligibility = new_credential->backup_eligibility;
        made.credential_source.backup_state = new_credential->backup_state;
        made.attestation_object = byte_buffer(new_credential->attestation_object);
        made.authenticator_data = byte_buffer(new_credential->authenticator_data);
        made.public_key = byte_buffer(new_credential->public_key);
        made.public_key_algorithm = new_credential->public_key_algorithm;
        for (size_t i = 0; i < new_credential->displaced_credential_count; ++i)
            made.displaced_credentials.append(new_credential->displaced_credentials[i]);
        *static_cast<Optional<Made>*>(context) = move(made);
    });
    if (!outcome.is_success)
        return authenticator_error(outcome.error);

    auto& authenticator = authenticators[made->authenticator];
    // NB: The places of the displaced credentials are in rising order, so the last goes first.
    for (auto index : made->displaced_credentials.in_reverse())
        authenticator.m_credentials.remove(index);

    auto& credential_source = made->credential_source;
    auto credential_id = credential_source.id;
    credential_source.rp_id = request.rp_id;
    credential_source.user_handle = request.user_id;
    credential_source.is_resident_credential = made->is_resident_key;
    credential_source.user_name = request.user_name;
    credential_source.user_display_name = request.user_display_name;
    credential_source.creation_time = UnixDateTime::now();
    authenticator.m_credentials.append(move(credential_source));

    return MadeCredential {
        .authenticator = authenticator,
        .is_resident_key = made->is_resident_key,
        .credential_id = move(credential_id),
        .attestation_object = move(made->attestation_object),
        .authenticator_data = move(made->authenticator_data),
        .public_key = move(made->public_key),
        .public_key_algorithm = made->public_key_algorithm,
    };
}

Vector<PublicKeyCredentialSource const&> Authenticator::credential_options_for(Web::WebAuthentication::GetAssertionRequest const& request) const
{
    auto credentials = ffi_credentials(m_credentials);
    auto allow_credential_ids = ffi_byte_strings(request.allow_credential_ids);
    auto ffi_request = ffi_get_assertion_request(request, allow_credential_ids, {});

    Vector<size_t> options;
    FFI::webview_authenticator_credential_options(credentials.data(), credentials.size(), &ffi_request, &options, [](void* context, size_t credential) {
        static_cast<Vector<size_t>*>(context)->append(credential);
    });

    Vector<PublicKeyCredentialSource const&> credential_options;
    for (auto index : options)
        credential_options.append(m_credentials[index]);
    return credential_options;
}

// https://w3c.github.io/webauthn/#sctn-getAssertion
ErrorOr<Authenticator::GotAssertion, AuthenticatorError> Authenticator::get_assertion(Vector<Authenticator&>& authenticators, Web::WebAuthentication::GetAssertionRequest const& request, ReadonlyBytes client_data_hash)
{
    auto ceremony = ceremony_authenticators(authenticators);
    auto allow_credential_ids = ffi_byte_strings(request.allow_credential_ids);
    auto ffi_request = ffi_get_assertion_request(request, allow_credential_ids, client_data_hash);

    struct Got {
        size_t authenticator { 0 };
        size_t selected_credential { 0 };
        Optional<u32> sign_count;
        ByteBuffer authenticator_data;
        ByteBuffer signature;
    };
    Optional<Got> got;
    auto outcome = FFI::webview_ceremony_get_assertion(ceremony.authenticators.data(), ceremony.authenticators.size(), &ffi_request, &got, [](void* context, FFI::FfiAuthenticatorAssertion const* assertion) {
        *static_cast<Optional<Got>*>(context) = Got {
            .authenticator = assertion->authenticator,
            .selected_credential = assertion->selected_credential,
            .sign_count = assertion->has_sign_count ? Optional<u32> { assertion->sign_count } : Optional<u32> {},
            .authenticator_data = byte_buffer(assertion->authenticator_data),
            .signature = byte_buffer(assertion->signature),
        };
    });
    if (!outcome.is_success)
        return authenticator_error(outcome.error);

    auto& authenticator = authenticators[got->authenticator];
    auto& selected_credential = authenticator.m_credentials[got->selected_credential];
    selected_credential.sign_count = got->sign_count;
    selected_credential.last_used_time = UnixDateTime::now();

    return GotAssertion {
        .authenticator = authenticator,
        .credential_id = selected_credential.id,
        .authenticator_data = move(got->authenticator_data),
        .signature = move(got->signature),
        .user_handle = selected_credential.user_handle,
    };
}

}
