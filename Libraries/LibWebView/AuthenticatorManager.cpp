/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/Memory.h>
#include <AK/Random.h>
#include <LibCore/EventLoop.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibURL/Parser.h>
#include <LibUnicode/Normalize.h>
#include <LibWebCommon/HTML/RegistrableDomainSuffix.h>
#include <LibWebView/Application.h>
#include <LibWebView/AuthenticatorManager.h>
#include <RustFFI.h>

namespace WebView {

using Web::WebAuthentication::AuthenticatorAttachment;
using Web::WebAuthentication::AuthenticatorError;
using Web::WebAuthentication::GetAssertionRequest;
using Web::WebAuthentication::MakeCredentialRequest;
using Web::WebAuthentication::ResidentKeyRequirement;
using Web::WebAuthentication::UserVerificationRequirement;

#if !defined(AK_OS_MACOS)
// FIXME: Keep the credentials with the Secret Service on Linux, and with the platform of the other systems.
OwnPtr<PlatformAuthenticatorBackend> PlatformAuthenticatorBackend::create()
{
    return {};
}
#endif

AuthenticatorManager& AuthenticatorManager::the()
{
    static auto& manager = *new AuthenticatorManager;
    return manager;
}

// https://w3c.github.io/webauthn/#clientdatajson-serialization
static ByteBuffer serialize_client_data(StringView type, ReadonlyBytes challenge, AuthenticatorManager::Caller const& caller, bool same_origin_with_ancestors)
{
    auto ffi_bytes = [](ReadonlyBytes bytes) { return FFI::FfiBytes { bytes.data(), bytes.size() }; };
    auto origin = caller.origin.serialize();
    // NB: crossOrigin is the inverse of the value of the sameOriginWithAncestors argument. topOrigin is the
    //     serialization of callerOrigin's top-level origin if that argument is false, else undefined.
    Optional<String> top_origin;
    if (!same_origin_with_ancestors && caller.top_level_origin.has_value())
        top_origin = caller.top_level_origin->serialize();

    ByteBuffer client_data;
    FFI::webview_serialize_client_data(
        ffi_bytes(type.bytes()), ffi_bytes(challenge), ffi_bytes(origin.bytes()),
        !same_origin_with_ancestors, top_origin.has_value(), ffi_bytes(top_origin.has_value() ? top_origin->bytes() : ReadonlyBytes {}),
        &client_data, [](void* context, FFI::FfiBytes serialized) {
            *static_cast<ByteBuffer*>(context) = MUST(ByteBuffer::copy(serialized.data, serialized.length));
        });
    return client_data;
}

// A ceremony that the built-in authenticator or a security key can answer, from where the user is asked about it to
// where one of the two gets the request. What the next step is comes from the flow in Rust
// (Libraries/LibWebView/Rust/src/flow.rs); this does the steps.
struct CeremonyWithUser : public RefCounted<CeremonyWithUser> {
    FFI::Flow flow {};
    PlatformAuthenticatorBackend* backend { nullptr };
    RefPtr<AuthenticatorManager::CeremonyCancellation> cancellation;
    AuthenticatorManager::UserInterface user_interface;
    String rp_id;
    // What the platform tells the user that they verify for.
    String action;
    // The accounts that the built-in authenticator has a credential for, the newest first.
    Vector<Utf16String> accounts;

    // The built-in authenticator does the operation, with the account that the user chose, when they did.
    Function<void(Optional<u32> account)> on_proceed;
    // The security keys get the request, with the user interface of the ceremony.
    Function<void(AuthenticatorManager::UserInterface)> on_security_key;
    // If the user does not consent or if user verification fails, return an error code equivalent to
    // "NotAllowedError" and terminate the operation.
    Function<void()> on_refused;
};

enum class AfterAnswer : u8 {
    No,
    Yes,
};

static void do_step(NonnullRefPtr<CeremonyWithUser>, FFI::FfiFlowStep, AfterAnswer = AfterAnswer::No);

static void ask_user(NonnullRefPtr<CeremonyWithUser> ceremony, FFI::Question question)
{
    auto kind = PasskeyChoiceKind::Account;
    Vector<Utf16String> options;
    switch (question) {
    case FFI::Question::Authenticator:
        kind = PasskeyChoiceKind::Authenticator;
        options.append("This device"_utf16);
        options.append("Security key"_utf16);
        break;
    case FFI::Question::Account:
        options = ceremony->accounts;
        break;
    case FFI::Question::AccountOrSecurityKey:
        kind = PasskeyChoiceKind::AccountOrSecurityKey;
        options = ceremony->accounts;
        options.append("Use a security key"_utf16);
        break;
    case FFI::Question::AccountForFormControl:
        kind = PasskeyChoiceKind::AccountForFormControl;
        options = ceremony->accounts;
        break;
    case FFI::Question::NoPasskey:
        // NB: With no options for PasskeyChoiceKind::Account, the user is told that there is no passkey.
        break;
    case FFI::Question::OtherWaysToSignIn:
    case FFI::Question::OtherWaysToCreate:
        kind = question == FFI::Question::OtherWaysToSignIn ? PasskeyChoiceKind::OtherWaysToSignIn : PasskeyChoiceKind::OtherWaysToCreate;
        options.append("Try again"_utf16);
        options.append("Use a security key"_utf16);
        break;
    }

    ceremony->user_interface.choose(kind, ceremony->rp_id, options, [ceremony](Optional<u32> answer) {
        // NB: The page can take the request back while the user chooses.
        auto step = ceremony->cancellation->is_cancelled()
            ? FFI::webview_flow_was_cancelled(&ceremony->flow)
            : FFI::webview_flow_user_chose(&ceremony->flow, answer.has_value(), answer.value_or(0));
        do_step(ceremony, step, AfterAnswer::Yes);
    });
}

static void verify_user_with_platform(NonnullRefPtr<CeremonyWithUser> ceremony)
{
    auto user_verification_id = ceremony->backend->verify_user(ceremony->action, [ceremony](bool is_verified) {
        ceremony->cancellation->on_cancel = nullptr;
        // NB: A page that took its request back while the user was asked gets nothing.
        auto step = ceremony->cancellation->is_cancelled()
            ? FFI::webview_flow_was_cancelled(&ceremony->flow)
            : FFI::webview_flow_user_was_verified(&ceremony->flow, is_verified);
        do_step(ceremony, step);
    });
    // NB: The prompt of the platform goes away with a request that is taken back.
    if (!ceremony->cancellation->is_cancelled())
        ceremony->cancellation->on_cancel = [backend = ceremony->backend, user_verification_id] { backend->cancel_user_verification(user_verification_id); };
}

static void do_step(NonnullRefPtr<CeremonyWithUser> ceremony, FFI::FfiFlowStep step, AfterAnswer after_answer)
{
    switch (step.kind) {
    case FFI::FfiFlowStepKind::Refuse:
        ceremony->on_refused();
        return;
    case FFI::FfiFlowStepKind::Ask:
        ask_user(move(ceremony), step.question);
        return;
    case FFI::FfiFlowStepKind::Proceed:
        ceremony->on_proceed(step.has_account ? Optional<u32> { step.account } : Optional<u32> {});
        return;
    case FFI::FfiFlowStepKind::UseSecurityKey:
    case FFI::FfiFlowStepKind::VerifyUser:
        break;
    }

    auto go_on = [ceremony, is_for_security_key = step.kind == FFI::FfiFlowStepKind::UseSecurityKey, after_answer] {
        if (is_for_security_key) {
            ceremony->on_security_key(move(ceremony->user_interface));
            return;
        }
        // NB: The platform asks for the user verification with a prompt of its own.
        if (after_answer == AfterAnswer::Yes && ceremony->user_interface.dismiss_passkey_prompt)
            ceremony->user_interface.dismiss_passkey_prompt();
        verify_user_with_platform(ceremony);
    };
    if (after_answer == AfterAnswer::No) {
        go_on();
        return;
    }
    // NB: The answer is a call from the user interface that the next step asks the user with, so that step starts
    //     after the call.
    Core::EventLoop::current().deferred_invoke([ceremony, go_on = move(go_on)] mutable {
        if (ceremony->cancellation->is_cancelled()) {
            do_step(ceremony, FFI::webview_flow_was_cancelled(&ceremony->flow));
            return;
        }
        go_on();
    });
}

static FFI::Means means_for(PlatformAuthenticatorBackend* backend, bool can_use_security_keys, AuthenticatorManager::UserInterface const& user_interface)
{
    bool can_ask_user = !!user_interface.choose;
    return {
        .can_verify_user_with_platform = backend && backend->can_verify_user(),
        .can_use_security_keys = can_use_security_keys,
        .can_ask_user = can_ask_user,
        .is_security_key_connected = can_use_security_keys && can_ask_user && SecurityKeySession::is_any_key_connected(),
    };
}

// A request that the built-in authenticator or a security key answers, and who gets the result.
template<typename Request, typename Result>
struct PendingRequest : public RefCounted<PendingRequest<Request, Result>> {
    PendingRequest(AuthenticatorManager::Caller caller, Request request, Function<void(Result)> on_complete)
        : caller(move(caller))
        , request(move(request))
        , on_complete(move(on_complete))
    {
    }

    AuthenticatorManager::Caller caller;
    // The request as the Relying Party made it.
    Request request;
    Function<void(Result)> on_complete;
};

// Whether the document may use the Relying Party ID, which the browser process decides by itself for a renderer that
// may have skipped its own check.
static ErrorOr<void, AuthenticatorError> validate_rp_id(URL::Origin const& caller_origin, StringView rp_id)
{
    // Let callerOrigin be origin. If callerOrigin is an opaque origin, throw a "NotAllowedError" DOMException.
    if (caller_origin.is_opaque())
        return AuthenticatorError::NotAllowed;

    // Let effectiveDomain be the callerOrigin's effective domain. If effective domain is not a valid domain, then
    // throw a "SecurityError" DOMException.
    auto effective_domain = caller_origin.effective_domain();
    if (!effective_domain.has_value() || !effective_domain->is_domain())
        return AuthenticatorError::Security;

    // If pkOptions.rp.id is not a registrable domain suffix of and is not equal to effectiveDomain, and if the client
    // does not support related origin requests, throw a "SecurityError" DOMException.
    auto rp_id_host = URL::Parser::parse_host(rp_id);
    if (!rp_id_host.has_value())
        return AuthenticatorError::Security;
    // NB: The algorithm asserts that the public suffix of the origin, prefixed by U+002E (.), matches the end of the
    //     suffix, which holds for a renderer that ran the algorithm itself. This process does not trust a renderer to
    //     have done so, and an assertion that fails here would end the browser.
    if (*rp_id_host != *effective_domain) {
        auto public_suffix = effective_domain->public_suffix();
        if (!rp_id_host->has<String>() || !public_suffix.has_value() || !rp_id_host->get<String>().ends_with_bytes(MUST(String::formatted(".{}", *public_suffix))))
            return AuthenticatorError::Security;
    }
    if (!Web::DOM::is_a_registrable_domain_suffix_of_or_is_equal_to(*rp_id_host, *effective_domain))
        return AuthenticatorError::Security;
    // NB: The RP ID goes as it is into the hash that scopes a credential and into what the user is shown. One that is
    //     not the serialization of its own host, such as one with letters that the host parser maps to others, would
    //     show the user a name that is not the name of the site.
    if (rp_id_host->serialize() != rp_id)
        return AuthenticatorError::Security;
    return {};
}

Vector<Authenticator&> AuthenticatorManager::authenticators_available_to(u64 view_id)
{
    Vector<Authenticator&> authenticators;
    for (auto& virtual_authenticator : m_virtual_authenticators) {
        if (!virtual_authenticator.owner_view_id.has_value() || virtual_authenticator.owner_view_id == view_id)
            authenticators.append(*virtual_authenticator.authenticator);
    }
    return authenticators;
}

// https://w3c.github.io/webauthn/#sctn-createCredential
ErrorOr<Web::WebAuthentication::MakeCredentialResponse, AuthenticatorError> AuthenticatorManager::make_credential_using(Vector<Authenticator&>& authenticators, Caller const& caller, Web::WebAuthentication::MakeCredentialRequest const& request)
{
    TRY(validate_rp_id(caller.origin, request.rp_id));

    // Let collectedClientData be a new CollectedClientData instance [...]
    // Let clientDataJSON be the JSON-compatible serialization of client data constructed from collectedClientData.
    auto client_data_json = serialize_client_data("webauthn.create"sv, request.challenge, caller, request.same_origin_with_ancestors);

    // NB: The authenticator has no attestation, so it makes no use of the hash of the client data.
    auto made = TRY(Authenticator::make_credential(authenticators, request));

    Vector<String> transports;
    transports.append(made.authenticator.configuration().transport);
    return Web::WebAuthentication::MakeCredentialResponse {
        .client_data_json = move(client_data_json),
        .credential_id = move(made.credential_id),
        .attestation_object = move(made.attestation_object),
        .authenticator_data = move(made.authenticator_data),
        .public_key = move(made.public_key),
        .public_key_algorithm = made.public_key_algorithm,
        .transports = move(transports),
        .authenticator_attachment = made.authenticator.attachment(),
        .is_resident_key = made.is_resident_key,
    };
}

// https://w3c.github.io/webauthn/#sctn-getAssertion
ErrorOr<Web::WebAuthentication::GetAssertionResponse, AuthenticatorError> AuthenticatorManager::get_assertion_using(Vector<Authenticator&>& authenticators, Caller const& caller, Web::WebAuthentication::GetAssertionRequest const& request)
{
    TRY(validate_rp_id(caller.origin, request.rp_id));

    // Let collectedClientData be a new CollectedClientData instance [...]
    // Let clientDataJSON be the JSON-compatible serialization of client data constructed from collectedClientData.
    auto client_data_json = serialize_client_data("webauthn.get"sv, request.challenge, caller, request.same_origin_with_ancestors);

    // Let clientDataHash be the hash of the serialized client data represented by clientDataJSON.
    auto client_data_hash = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, client_data_json);

    auto got = TRY(Authenticator::get_assertion(authenticators, request, client_data_hash.bytes()));
    return Web::WebAuthentication::GetAssertionResponse {
        .client_data_json = move(client_data_json),
        .credential_id = move(got.credential_id),
        .authenticator_data = move(got.authenticator_data),
        .signature = move(got.signature),
        .user_handle = move(got.user_handle),
        .authenticator_attachment = got.authenticator.attachment(),
    };
}

PlatformAuthenticatorBackend* AuthenticatorManager::platform_backend()
{
    if (!m_did_create_platform_backend) {
        m_did_create_platform_backend = true;
        // NB: A test must neither ask the user of the device nor reach the credentials that the platform keeps.
        if (Application::web_content_options().is_test_mode != IsTestMode::Yes)
            m_platform_backend = PlatformAuthenticatorBackend::create();
    }
    return m_platform_backend.ptr();
}

Authenticator& AuthenticatorManager::platform_authenticator()
{
    if (!m_platform_authenticator) {
        m_platform_authenticator = make<Authenticator>(AuthenticatorConfiguration {
            .protocol = "ctap2"_string,
            .transport = "internal"_string,
            .has_resident_key = true,
            .has_user_verification = true,
            // NB: The built-in authenticator keeps no signature counter, so the counter of a credential stays at zero.
            .has_signature_counter = false,
            // NB: The backend asks and verifies the user before each operation of this authenticator.
            .is_user_consenting = true,
            .is_user_verified = true,
            .default_backup_eligibility = false,
            .default_backup_state = false,
        });
    }
    // NB: A read that failed is done again the next time, so that an error that goes away costs no passkey.
    if (!m_has_loaded_platform_credentials) {
        if (auto credentials = m_platform_backend->load_credentials(); credentials.has_value()) {
            for (auto& credential : *credentials)
                m_platform_authenticator->add_credential(move(credential));
            m_has_loaded_platform_credentials = true;
        }
    }
    return *m_platform_authenticator;
}

template<typename Result, typename Response>
static Result result_from(ErrorOr<Response, AuthenticatorError> response)
{
    if (response.is_error())
        return response.release_error();
    return response.release_value();
}

static ByteBuffer utf8_bytes(Utf16String const& string)
{
    auto utf8 = string.to_utf8();
    return MUST(ByteBuffer::copy(utf8.bytes()));
}

// What a ceremony with a security key needs to start again, such as with the PIN of the user.
struct AuthenticatorManager::SecurityKeyMakeCredentialState : public RefCounted<SecurityKeyMakeCredentialState> {
    static constexpr auto prompt_kind = SecurityKeyPromptKind::CreatePasskey;
    ~SecurityKeyMakeCredentialState() { secure_zero(key_request.pin.data(), key_request.pin.size()); }
    u64 view_id { 0 };
    RefPtr<CeremonyCancellation> cancellation;
    String rp_id;
    SecurityKeyMakeCredentialRequest key_request;
    ByteBuffer client_data_json;
    UserInterface user_interface;
    Function<void(Web::WebAuthentication::MakeCredentialResult)> on_complete;
};

struct AuthenticatorManager::SecurityKeyGetAssertionState : public RefCounted<SecurityKeyGetAssertionState> {
    static constexpr auto prompt_kind = SecurityKeyPromptKind::SignIn;
    ~SecurityKeyGetAssertionState() { secure_zero(key_request.pin.data(), key_request.pin.size()); }
    u64 view_id { 0 };
    RefPtr<CeremonyCancellation> cancellation;
    String rp_id;
    SecurityKeyGetAssertionRequest key_request;
    ByteBuffer client_data_json;
    UserInterface user_interface;
    Function<void(Web::WebAuthentication::GetAssertionResult)> on_complete;
};

template<typename State>
void AuthenticatorManager::handle_security_key_error(NonnullRefPtr<State> state, SecurityKeyError error, void (AuthenticatorManager::*run_again)(NonnullRefPtr<State>))
{
    FFI::SecurityKeyError ffi_error = FFI::SecurityKeyError::Failed;
    switch (error) {
    case SecurityKeyError::CredentialExcluded:
        ffi_error = FFI::SecurityKeyError::CredentialExcluded;
        break;
    case SecurityKeyError::NoCredentials:
        ffi_error = FFI::SecurityKeyError::NoCredentials;
        break;
    case SecurityKeyError::PinRequired:
        ffi_error = FFI::SecurityKeyError::PinRequired;
        break;
    case SecurityKeyError::PinInvalid:
        ffi_error = FFI::SecurityKeyError::PinInvalid;
        break;
    case SecurityKeyError::PinBlocked:
        ffi_error = FFI::SecurityKeyError::PinBlocked;
        break;
    case SecurityKeyError::NotSupported:
        ffi_error = FFI::SecurityKeyError::NotSupported;
        break;
    case SecurityKeyError::Failed:
        break;
    }
    auto what_follows = FFI::webview_after_security_key_error(ffi_error, !!state->user_interface.request_security_key_pin);

    if (what_follows.asks_for_pin) {
        auto reason = what_follows.pin_prompt_reason == FFI::SecurityKeyPinPromptReason::Wrong ? SecurityKeyPinPromptReason::Wrong : SecurityKeyPinPromptReason::Required;
        state->user_interface.request_security_key_pin(reason, state->rp_id, [this, state, run_again](Optional<String> pin) {
            // If the user does not consent, return an error code equivalent to "NotAllowedError" and terminate the
            // operation.
            if (!pin.has_value() || pin->is_empty()) {
                state->on_complete(AuthenticatorError::NotAllowed);
                return;
            }
            // NB: CTAP wants the PIN in Unicode Normalization Form C.
            auto normalized_pin = Unicode::normalize(pin->bytes_as_string_view(), Unicode::NormalizationForm::NFC);
            // NB: A PIN has at least four code points, and takes at most 63 bytes in UTF-8. A key counts anything
            //     else as a wrong PIN, which takes away one of the few tries that the user has, so the key does not
            //     get it.
            if (normalized_pin.code_points().length() < 4 || normalized_pin.bytes().size() > 63) {
                handle_security_key_error(state, SecurityKeyError::PinInvalid, run_again);
                return;
            }
            secure_zero(state->key_request.pin.data(), state->key_request.pin.size());
            state->key_request.pin = MUST(ByteBuffer::copy(normalized_pin.bytes()));
            (this->*run_again)(state);
        });
        return;
    }

    if (what_follows.has_message && state->user_interface.show_message) {
        auto message = PasskeyMessage::SecurityKeyHasNoPasskey;
        switch (what_follows.message) {
        case FFI::PasskeyMessage::SecurityKeyHasNoPasskey:
            break;
        case FFI::PasskeyMessage::SecurityKeyAlreadyHasPasskey:
            message = PasskeyMessage::SecurityKeyAlreadyHasPasskey;
            break;
        case FFI::PasskeyMessage::SecurityKeyPinBlocked:
            message = PasskeyMessage::SecurityKeyPinBlocked;
            break;
        case FFI::PasskeyMessage::SecurityKeyNotSupported:
            message = PasskeyMessage::SecurityKeyNotSupported;
            break;
        }
        state->user_interface.show_message(message, state->rp_id);
    }
    state->on_complete(what_follows.error == FFI::AuthenticatorError::InvalidState ? AuthenticatorError::InvalidState : AuthenticatorError::NotAllowed);
}

// Shows the prompt for a security key and keeps the session, until the key answers or the user cancels.
template<typename State>
static void show_security_key_prompt(NonnullRefPtr<State> const& state, Function<void()> on_cancel)
{
    state->user_interface.show_security_key_prompt(State::prompt_kind, state->rp_id, move(on_cancel));
}

bool AuthenticatorManager::can_use_security_keys(UserInterface const& user_interface) const
{
    // NB: A test must not reach the devices of the machine that it runs on.
    if (Application::web_content_options().is_test_mode == IsTestMode::Yes)
        return false;
    return SecurityKeySession::is_supported() && user_interface.show_security_key_prompt;
}

u64 AuthenticatorManager::start_security_key_ceremony(u64 view_id, NonnullRefPtr<SecurityKeySession> session, Function<void()> dismiss_prompt, Function<void()> on_cancel)
{
    // NB: Every session talks to the same keys, and a key cannot say which request its user means to answer with a
    //     touch. The ceremony that the user began last is thus the only one that waits for a key, so that a touch
    //     cannot answer the request of a tab that the user has left.
    Vector<u64> other_view_ids;
    for (auto const& ceremony : m_security_key_ceremonies) {
        if (ceremony.key != view_id)
            other_view_ids.append(ceremony.key);
    }
    for (auto other_view_id : other_view_ids)
        cancel_security_key_ceremony(other_view_id);

    auto ceremony_id = m_next_security_key_ceremony_id++;
    m_security_key_ceremonies.set(view_id, SecurityKeyCeremony { ceremony_id, move(session), move(dismiss_prompt), move(on_cancel) });
    return ceremony_id;
}

void AuthenticatorManager::finish_security_key_ceremony(u64 view_id)
{
    auto ceremony = m_security_key_ceremonies.take(view_id);
    if (ceremony.has_value() && ceremony->dismiss_prompt)
        ceremony->dismiss_prompt();
}

void AuthenticatorManager::cancel_security_key_ceremony(u64 view_id)
{
    if (auto ceremony = m_security_key_ceremonies.get(view_id); ceremony.has_value())
        cancel_security_key_ceremony(view_id, ceremony->id);
}

void AuthenticatorManager::cancel_security_key_ceremony(u64 view_id, u64 ceremony_id)
{
    // NB: The prompt of a ceremony that is over can still say that the user cancelled it.
    auto current = m_security_key_ceremonies.get(view_id);
    if (!current.has_value() || current->id != ceremony_id)
        return;
    auto ceremony = m_security_key_ceremonies.take(view_id).release_value();
    ceremony.session->cancel();
    if (ceremony.dismiss_prompt)
        ceremony.dismiss_prompt();
    ceremony.on_cancel();
}

bool AuthenticatorManager::begin_ceremony_with_user(Caller const& caller)
{
    // NB: A ceremony whose request was taken back no longer asks the user.
    if (auto current = m_ceremonies_with_user.get(caller.view_id); current.has_value() && !(*current)->is_cancelled() && *current != caller.cancellation.ptr())
        return false;
    m_ceremonies_with_user.set(caller.view_id, caller.cancellation);
    return true;
}

void AuthenticatorManager::end_ceremony_with_user(u64 view_id, CeremonyCancellation const& cancellation)
{
    if (is_ceremony_with_user(view_id, cancellation))
        m_ceremonies_with_user.remove(view_id);
}

bool AuthenticatorManager::is_ceremony_with_user(u64 view_id, CeremonyCancellation const& cancellation) const
{
    auto current = m_ceremonies_with_user.get(view_id);
    return current.has_value() && *current == &cancellation;
}

// https://w3c.github.io/webauthn/#sctn-createCredential
void AuthenticatorManager::make_credential(Caller caller, Web::WebAuthentication::MakeCredentialRequest request, UserInterface user_interface, Function<void(Web::WebAuthentication::MakeCredentialResult)> on_complete)
{
    using Web::WebAuthentication::AuthenticatorAttachment;
    using Web::WebAuthentication::MakeCredentialResult;

    // NB: Virtual authenticators stand in for the real ones, https://w3c.github.io/webauthn/#sctn-automation
    auto virtual_authenticators = authenticators_available_to(caller.view_id);
    if (!virtual_authenticators.is_empty()) {
        on_complete(result_from<MakeCredentialResult>(make_credential_using(virtual_authenticators, caller, request)));
        return;
    }

    // NB: The user is not asked about a request that the client must refuse.
    if (auto validation = validate_rp_id(caller.origin, request.rp_id); validation.is_error()) {
        on_complete(validation.release_error());
        return;
    }
    // If the length of pkOptions.user.id is not between 1 and 64 bytes (inclusive) then throw a TypeError.
    // NB: A renderer that sends such an id skipped the check that throws the TypeError.
    if (request.user_id.is_empty() || request.user_id.size() > 64) {
        on_complete(AuthenticatorError::NotAllowed);
        return;
    }

    // NB: The user of a view is asked for one ceremony at a time.
    if (!begin_ceremony_with_user(caller)) {
        on_complete(AuthenticatorError::NotAllowed);
        return;
    }
    on_complete = [this, view_id = caller.view_id, cancellation = caller.cancellation, on_complete = move(on_complete)](MakeCredentialResult result) {
        on_complete(move(result));
        end_ceremony_with_user(view_id, *cancellation);
    };

    auto* backend = platform_backend();
    auto means = means_for(backend, can_use_security_keys(user_interface), user_interface);

    auto pending = make_ref_counted<PendingRequest<MakeCredentialRequest, MakeCredentialResult>>(move(caller), move(request), move(on_complete));
    auto ceremony = make_ref_counted<CeremonyWithUser>();
    ceremony->backend = backend;
    ceremony->cancellation = pending->caller.cancellation;
    ceremony->user_interface = move(user_interface);
    ceremony->rp_id = pending->request.rp_id;
    ceremony->action = MUST(String::formatted("create a passkey for {}", pending->request.rp_id));
    ceremony->on_refused = [pending] { pending->on_complete(AuthenticatorError::NotAllowed); };
    ceremony->on_security_key = [this, pending](UserInterface user_interface) {
        make_credential_with_security_key(move(pending->caller), move(pending->request), move(user_interface), move(pending->on_complete));
    };
    ceremony->on_proceed = [this, pending](Optional<u32>) {
        pending->on_complete(make_credential_with_platform_authenticator(pending->caller, pending->request));
    };

    auto const& made_request = pending->request;
    auto attachment = made_request.authenticator_attachment.value_or(AuthenticatorAttachment::Platform) == AuthenticatorAttachment::Platform ? FFI::AuthenticatorAttachment::Platform : FFI::AuthenticatorAttachment::CrossPlatform;
    auto step = FFI::webview_flow_for_new_credential(&means, made_request.authenticator_attachment.has_value(), attachment, made_request.public_key_algorithms.data(), made_request.public_key_algorithms.size(), &ceremony->flow);
    do_step(move(ceremony), step);
}

// Makes the credential with the built-in authenticator, for a user that the platform verified, and gives it to the
// platform to keep.
Web::WebAuthentication::MakeCredentialResult AuthenticatorManager::make_credential_with_platform_authenticator(Caller const& caller, Web::WebAuthentication::MakeCredentialRequest const& request)
{
    using Web::WebAuthentication::MakeCredentialResult;

    auto& authenticator = platform_authenticator();
    // NB: Without the credentials that the platform keeps, the client cannot tell which of them the new one takes
    //     the place of.
    if (!m_has_loaded_platform_credentials)
        return AuthenticatorError::NotAllowed;

    // NB: A discoverable credential takes the place of those that the authenticator has for the same account of
    //     the Relying Party. The platform has each of them under its own credential ID.
    Vector<PublicKeyCredentialSource> credentials_of_account;
    for (auto const& credential : authenticator.credentials()) {
        if (credential.is_resident_credential && credential.rp_id == request.rp_id && credential.user_handle == request.user_id)
            credentials_of_account.append(credential);
    }

    Vector<Authenticator&> authenticators;
    authenticators.append(authenticator);
    auto response = make_credential_using(authenticators, caller, request);

    if (!response.is_error()) {
        // NB: A credential that the platform did not take does not exist, and then the credentials before it stay.
        auto credential = authenticator.look_up_credential(response.value().credential_id);
        if (!credential.has_value() || !m_platform_backend->store_credential(*credential)) {
            authenticator.remove_credential(response.value().credential_id);
            for (auto& displaced : credentials_of_account) {
                if (!authenticator.look_up_credential(displaced.id).has_value())
                    authenticator.add_credential(move(displaced));
            }
            return AuthenticatorError::NotAllowed;
        }
        // NB: A credential that the platform keeps is there again after a restart, beside the one that took its
        //     place. When the platform does not let one go, the new credential does not exist, and the credentials
        //     before it stay.
        Vector<PublicKeyCredentialSource const&> removed_credentials;
        bool removed_all = true;
        for (auto const& displaced : credentials_of_account) {
            if (authenticator.look_up_credential(displaced.id).has_value())
                continue;
            if (!m_platform_backend->remove_credential(displaced.id)) {
                removed_all = false;
                break;
            }
            removed_credentials.append(displaced);
        }
        if (!removed_all) {
            // NB: Putting things back can fail too. The authenticator then has what the platform has, so that a
            //     restart changes nothing: a credential that the platform lost is gone, and a new credential that the
            //     platform kept stays for the user to remove in the settings.
            Vector<ByteBuffer> lost_credential_ids;
            for (auto const& removed : removed_credentials) {
                if (!m_platform_backend->store_credential(removed)) {
                    warnln("Could not put a passkey back in the platform");
                    lost_credential_ids.append(removed.id);
                }
            }
            if (m_platform_backend->remove_credential(response.value().credential_id))
                authenticator.remove_credential(response.value().credential_id);
            else
                warnln("Could not remove a passkey from the platform");
            for (auto& displaced : credentials_of_account) {
                if (!authenticator.look_up_credential(displaced.id).has_value() && !lost_credential_ids.contains_slow(displaced.id))
                    authenticator.add_credential(move(displaced));
            }
            return AuthenticatorError::NotAllowed;
        }
    }
    return result_from<MakeCredentialResult>(move(response));
}

void AuthenticatorManager::make_credential_with_security_key(Caller caller, Web::WebAuthentication::MakeCredentialRequest request, UserInterface user_interface, Function<void(Web::WebAuthentication::MakeCredentialResult)> on_complete)
{
    using Web::WebAuthentication::MakeCredentialResult;

    // Let clientDataJSON be the JSON-compatible serialization of client data constructed from collectedClientData.
    auto client_data_json = serialize_client_data("webauthn.create"sv, request.challenge, caller, request.same_origin_with_ancestors);
    // Let clientDataHash be the hash of the serialized client data represented by clientDataJSON.
    auto client_data_hash = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, client_data_json);

    SecurityKeyMakeCredentialRequest key_request {
        .client_data_hash = MUST(ByteBuffer::copy(client_data_hash.bytes())),
        .rp_id = MUST(ByteBuffer::copy(request.rp_id.bytes())),
        .rp_name = utf8_bytes(request.rp_name),
        .user_id = request.user_id,
        .user_name = utf8_bytes(request.user_name),
        .user_display_name = utf8_bytes(request.user_display_name),
        .public_key_algorithms = request.public_key_algorithms,
        .exclude_credential_ids = request.exclude_credential_ids,
        .resident_key = request.resident_key,
        .user_verification = request.user_verification,
        .pin = {},
        .pin_device_id = {},
    };

    auto state = adopt_ref(*new SecurityKeyMakeCredentialState);
    state->view_id = caller.view_id;
    state->cancellation = caller.cancellation;
    state->rp_id = request.rp_id;
    state->key_request = move(key_request);
    state->client_data_json = move(client_data_json);
    state->user_interface = move(user_interface);
    state->on_complete = move(on_complete);
    run_security_key_make_credential(move(state));
}

void AuthenticatorManager::run_security_key_make_credential(NonnullRefPtr<SecurityKeyMakeCredentialState> state)
{
    // NB: The page can take the request back while the user is asked for the PIN.
    if (state->cancellation->is_cancelled()) {
        state->on_complete(AuthenticatorError::NotAllowed);
        return;
    }
    auto view_id = state->view_id;
    cancel_security_key_ceremony(view_id);

    auto session = SecurityKeySession::make_credential(state->key_request, [this, state](ErrorOr<SecurityKeyAttestation, SecurityKeyError> result) {
        // NB: A PIN that the user then gives is for the key that the user chose, and for no other key.
        if (auto ceremony = m_security_key_ceremonies.get(state->view_id); ceremony.has_value())
            state->key_request.pin_device_id = ceremony->session->device_that_wants_pin();
        finish_security_key_ceremony(state->view_id);
        if (result.is_error()) {
            handle_security_key_error(state, result.error(), &AuthenticatorManager::run_security_key_make_credential);
            return;
        }
        // -> If any authenticator indicates success,
        auto attestation = result.release_value();
        Vector<String> transports;
        transports.append("usb"_string);
        state->on_complete(Web::WebAuthentication::MakeCredentialResponse {
            .client_data_json = move(state->client_data_json),
            .credential_id = move(attestation.credential_id),
            .attestation_object = move(attestation.attestation_object),
            .authenticator_data = move(attestation.authenticator_data),
            .public_key = move(attestation.public_key),
            .public_key_algorithm = attestation.public_key_algorithm,
            .transports = move(transports),
            .authenticator_attachment = Web::WebAuthentication::AuthenticatorAttachment::CrossPlatform,
            .is_resident_key = attestation.is_resident_key,
        });
    });
    auto dismiss_prompt = [state] {
        if (state->user_interface.dismiss_passkey_prompt)
            state->user_interface.dismiss_passkey_prompt();
    };
    auto ceremony_id = start_security_key_ceremony(view_id, move(session), move(dismiss_prompt), [state] {
        // If the user does not consent, return an error code equivalent to "NotAllowedError" and terminate the
        // operation.
        state->on_complete(AuthenticatorError::NotAllowed);
    });
    state->cancellation->on_cancel = [this, view_id, ceremony_id] {
        cancel_security_key_ceremony(view_id, ceremony_id);
    };
    show_security_key_prompt(state, [this, view_id, ceremony_id] {
        cancel_security_key_ceremony(view_id, ceremony_id);
    });
}

// https://w3c.github.io/webauthn/#sctn-getAssertion
void AuthenticatorManager::get_assertion(Caller caller, Web::WebAuthentication::GetAssertionRequest request, UserInterface user_interface, Function<void(Web::WebAuthentication::GetAssertionResult)> on_complete)
{
    using Web::WebAuthentication::GetAssertionRequest;
    using Web::WebAuthentication::GetAssertionResult;

    // NB: Virtual authenticators stand in for the real ones, https://w3c.github.io/webauthn/#sctn-automation
    auto virtual_authenticators = authenticators_available_to(caller.view_id);
    if (!virtual_authenticators.is_empty()) {
        on_complete(result_from<GetAssertionResult>(get_assertion_using(virtual_authenticators, caller, request)));
        return;
    }

    // NB: The user is not asked about a request that the client must refuse.
    if (auto validation = validate_rp_id(caller.origin, request.rp_id); validation.is_error()) {
        on_complete(validation.release_error());
        return;
    }

    // NB: The user of a view is asked for one ceremony at a time.
    if (!begin_ceremony_with_user(caller)) {
        on_complete(AuthenticatorError::NotAllowed);
        return;
    }
    on_complete = [this, view_id = caller.view_id, cancellation = caller.cancellation, on_complete = move(on_complete)](GetAssertionResult result) {
        on_complete(move(result));
        end_ceremony_with_user(view_id, *cancellation);
    };

    auto* backend = platform_backend();
    auto means = means_for(backend, can_use_security_keys(user_interface), user_interface);

    auto pending = make_ref_counted<PendingRequest<GetAssertionRequest, GetAssertionResult>>(move(caller), move(request), move(on_complete));
    auto ceremony = make_ref_counted<CeremonyWithUser>();
    ceremony->backend = backend;
    ceremony->cancellation = pending->caller.cancellation;
    ceremony->user_interface = move(user_interface);
    ceremony->rp_id = pending->request.rp_id;
    ceremony->action = MUST(String::formatted("sign in to {} with a passkey", pending->request.rp_id));

    // Prompt the user to select a public key credential source selectedCredential from credentialOptions.
    // NB: The newest credential comes first for the user.
    Vector<ByteBuffer> credential_ids;
    if (means.can_verify_user_with_platform) {
        for (auto const& credential : platform_authenticator().credential_options_for(pending->request).in_reverse()) {
            credential_ids.append(credential.id);
            if (credential.user_name.has_value() && !credential.user_name->is_empty())
                ceremony->accounts.append(*credential.user_name);
            else if (credential.user_display_name.has_value() && !credential.user_display_name->is_empty())
                ceremony->accounts.append(*credential.user_display_name);
            else
                ceremony->accounts.append("Passkey without a name"_utf16);
        }
    }

    ceremony->on_refused = [pending] { pending->on_complete(AuthenticatorError::NotAllowed); };
    // NB: The key gets the request as the Relying Party made it. Its ceremony takes the user interface, also to ask
    //     which account of the key to use.
    ceremony->on_security_key = [this, pending](UserInterface user_interface) {
        get_assertion_with_security_key(move(pending->caller), move(pending->request), move(user_interface), move(pending->on_complete));
    };
    ceremony->on_proceed = [this, pending, credential_ids = move(credential_ids)](Optional<u32> account) mutable {
        auto request = pending->request;
        // NB: The authenticator then has one credential to use for the request.
        if (account.has_value()) {
            request.allow_credential_ids.clear();
            request.allow_credential_ids.append(move(credential_ids[*account]));
        }
        auto& authenticator = platform_authenticator();
        Vector<Authenticator&> authenticators;
        authenticators.append(authenticator);
        auto response = get_assertion_using(authenticators, pending->caller, request);
        // NB: The platform keeps when the credential was last used. The assertion does not depend on that.
        if (!response.is_error()) {
            if (auto credential = authenticator.look_up_credential(response.value().credential_id); credential.has_value())
                m_platform_backend->update_credential(*credential);
        }
        pending->on_complete(result_from<GetAssertionResult>(move(response)));
    };

    auto step = FFI::webview_flow_for_assertion(&means, pending->request.is_conditional, static_cast<u32>(ceremony->accounts.size()), &ceremony->flow);
    do_step(move(ceremony), step);
}

void AuthenticatorManager::get_assertion_with_security_key(Caller caller, Web::WebAuthentication::GetAssertionRequest request, UserInterface user_interface, Function<void(Web::WebAuthentication::GetAssertionResult)> on_complete)
{
    using Web::WebAuthentication::GetAssertionResult;

    // Let clientDataJSON be the JSON-compatible serialization of client data constructed from collectedClientData.
    auto client_data_json = serialize_client_data("webauthn.get"sv, request.challenge, caller, request.same_origin_with_ancestors);
    // Let clientDataHash be the hash of the serialized client data represented by clientDataJSON.
    auto client_data_hash = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, client_data_json);

    SecurityKeyGetAssertionRequest key_request {
        .client_data_hash = MUST(ByteBuffer::copy(client_data_hash.bytes())),
        .rp_id = MUST(ByteBuffer::copy(request.rp_id.bytes())),
        .allow_credential_ids = request.allow_credential_ids,
        .user_verification = request.user_verification,
        .pin = {},
        .pin_device_id = {},
    };

    auto state = adopt_ref(*new SecurityKeyGetAssertionState);
    state->view_id = caller.view_id;
    state->cancellation = caller.cancellation;
    state->rp_id = request.rp_id;
    state->key_request = move(key_request);
    state->client_data_json = move(client_data_json);
    state->user_interface = move(user_interface);
    state->on_complete = move(on_complete);
    run_security_key_get_assertion(move(state));
}

void AuthenticatorManager::run_security_key_get_assertion(NonnullRefPtr<SecurityKeyGetAssertionState> state)
{
    // NB: The page can take the request back while the user is asked for the PIN.
    if (state->cancellation->is_cancelled()) {
        state->on_complete(AuthenticatorError::NotAllowed);
        return;
    }
    auto view_id = state->view_id;
    cancel_security_key_ceremony(view_id);

    auto session = SecurityKeySession::get_assertion(state->key_request, [this, state](ErrorOr<Vector<SecurityKeyAssertion>, SecurityKeyError> result) {
        // NB: A PIN that the user then gives is for the key that the user chose, and for no other key.
        if (auto ceremony = m_security_key_ceremonies.get(state->view_id); ceremony.has_value())
            state->key_request.pin_device_id = ceremony->session->device_that_wants_pin();
        finish_security_key_ceremony(state->view_id);
        if (result.is_error()) {
            handle_security_key_error(state, result.error(), &AuthenticatorManager::run_security_key_get_assertion);
            return;
        }
        auto assertions = result.release_value();
        if (assertions.is_empty()) {
            state->on_complete(AuthenticatorError::NotAllowed);
            return;
        }

        auto complete_with = [state](SecurityKeyAssertion assertion) {
            state->on_complete(Web::WebAuthentication::GetAssertionResponse {
                .client_data_json = move(state->client_data_json),
                .credential_id = move(assertion.credential_id),
                .authenticator_data = move(assertion.authenticator_data),
                .signature = move(assertion.signature),
                .user_handle = move(assertion.user_handle),
                .authenticator_attachment = Web::WebAuthentication::AuthenticatorAttachment::CrossPlatform,
            });
        };
        if (assertions.size() == 1 || !state->user_interface.choose) {
            complete_with(move(assertions[0]));
            return;
        }

        // NB: The key gave an assertion for each of its credentials for the Relying Party, and the user selects one.
        Vector<Utf16String> accounts;
        for (auto const& assertion : assertions) {
            auto name = !assertion.user_name.is_empty() ? assertion.user_name.bytes() : assertion.user_display_name.bytes();
            if (name.is_empty())
                accounts.append("Passkey without a name"_utf16);
            else
                accounts.append(Utf16String::from_utf8_with_replacement_character(StringView { name }));
        }
        state->user_interface.choose(PasskeyChoiceKind::Account, state->rp_id, accounts, [state, assertions = move(assertions), complete_with = move(complete_with)](Optional<u32> index) mutable {
            // NB: The page can take the request back while the user chooses.
            if (state->cancellation->is_cancelled() || !index.has_value() || *index >= assertions.size()) {
                state->on_complete(AuthenticatorError::NotAllowed);
                return;
            }
            complete_with(move(assertions[*index]));
        });
    });
    auto dismiss_prompt = [state] {
        if (state->user_interface.dismiss_passkey_prompt)
            state->user_interface.dismiss_passkey_prompt();
    };
    auto ceremony_id = start_security_key_ceremony(view_id, move(session), move(dismiss_prompt), [state] {
        // If the user does not consent, return an error code equivalent to "NotAllowedError" and terminate the
        // operation.
        state->on_complete(AuthenticatorError::NotAllowed);
    });
    state->cancellation->on_cancel = [this, view_id, ceremony_id] {
        cancel_security_key_ceremony(view_id, ceremony_id);
    };
    show_security_key_prompt(state, [this, view_id, ceremony_id] {
        cancel_security_key_ceremony(view_id, ceremony_id);
    });
}

bool AuthenticatorManager::has_platform_passkeys()
{
    return platform_backend() != nullptr;
}

Optional<Vector<AuthenticatorManager::StoredPasskey>> AuthenticatorManager::platform_passkeys()
{
    Vector<StoredPasskey> passkeys;
    if (!platform_backend())
        return passkeys;
    auto& authenticator = platform_authenticator();
    // NB: An authenticator that could not read the platform cannot say which passkeys there are.
    if (!m_has_loaded_platform_credentials)
        return {};
    for (auto const& credential : authenticator.credentials()) {
        Utf16String account;
        if (credential.user_name.has_value() && !credential.user_name->is_empty())
            account = *credential.user_name;
        else if (credential.user_display_name.has_value())
            account = *credential.user_display_name;
        passkeys.append({ MUST(encode_base64url(credential.id, AK::OmitPadding::Yes)), credential.rp_id, move(account), credential.creation_time, credential.last_used_time });
    }
    return passkeys;
}

bool AuthenticatorManager::remove_platform_passkey(StringView id)
{
    auto* backend = platform_backend();
    auto credential_id = decode_base64url(id);
    if (!backend || credential_id.is_error())
        return false;
    // NB: Only a credential that the authenticator knows is removed from the platform.
    auto& authenticator = platform_authenticator();
    // NB: An authenticator that could not read the platform cannot say that the credential is not there.
    if (!m_has_loaded_platform_credentials)
        return false;
    if (!authenticator.look_up_credential(credential_id.value()).has_value())
        return true;
    // NB: The platform loses the credential first. One that it keeps is there again after a restart, so the
    //     authenticator keeps it too, and the user is told.
    if (!backend->remove_credential(credential_id.value()))
        return false;
    authenticator.remove_credential(credential_id.value());
    return true;
}

// https://w3c.github.io/webauthn/#dom-publickeycredential-isuserverifyingplatformauthenticatoravailable
bool AuthenticatorManager::is_user_verifying_platform_authenticator_available(u64 view_id)
{
    auto virtual_authenticators = authenticators_available_to(view_id);
    if (virtual_authenticators.is_empty()) {
        auto* backend = platform_backend();
        return backend && backend->can_verify_user();
    }
    for (auto& authenticator : virtual_authenticators) {
        if (authenticator.attachment() == Web::WebAuthentication::AuthenticatorAttachment::Platform && authenticator.configuration().has_user_verification)
            return true;
    }
    return false;
}

void AuthenticatorManager::remove_virtual_authenticators_of_view(u64 view_id)
{
    m_virtual_authenticators.remove_all_matching([&](auto const& virtual_authenticator) {
        return virtual_authenticator.owner_view_id == view_id;
    });
}

Optional<Authenticator&> AuthenticatorManager::virtual_authenticator(Optional<u64> owner_view_id, JsonObject const& parameters)
{
    auto authenticator_id = parameters.get_string("authenticatorId"sv);
    if (!authenticator_id.has_value())
        return {};
    for (auto& virtual_authenticator : m_virtual_authenticators) {
        if (virtual_authenticator.id == *authenticator_id && virtual_authenticator.owner_view_id == owner_view_id)
            return *virtual_authenticator.authenticator;
    }
    return {};
}

ErrorOr<JsonValue, String> AuthenticatorManager::run_automation_command(Optional<u64> owner_view_id, StringView command, JsonObject const& parameters)
{
    if (command == "addVirtualAuthenticator"sv)
        return add_virtual_authenticator(owner_view_id, parameters);
    if (command == "removeVirtualAuthenticator"sv)
        return remove_virtual_authenticator(owner_view_id, parameters);

    // If authenticatorId does not match any Virtual Authenticator stored in the Virtual Authenticator Database, return
    // a WebDriver error with WebDriver error code invalid argument.
    auto authenticator = virtual_authenticator(owner_view_id, parameters);
    if (!authenticator.has_value())
        return "invalid argument"_string;

    if (command == "addCredential"sv)
        return add_credential(*authenticator, parameters);
    if (command == "getCredentials"sv)
        return get_credentials(*authenticator);
    if (command == "removeCredential"sv)
        return remove_credential(*authenticator, parameters);
    if (command == "removeAllCredentials"sv) {
        authenticator->remove_all_credentials();
        return JsonValue {};
    }
    if (command == "setUserVerified"sv)
        return set_user_verified(*authenticator, parameters);
    return "unknown command"_string;
}

// https://w3c.github.io/webauthn/#sctn-automation-add-virtual-authenticator
ErrorOr<JsonValue, String> AuthenticatorManager::add_virtual_authenticator(Optional<u64> owner_view_id, JsonObject const& parameters)
{
    AuthenticatorConfiguration configuration;
    bool has_protocol = false;
    bool has_transport = false;
    bool is_invalid = false;
    bool is_unsupported = false;

    auto read_boolean = [&](JsonValue const& value, bool& target) {
        if (!value.is_bool()) {
            is_invalid = true;
            return;
        }
        target = value.as_bool();
    };

    parameters.for_each_member([&](auto const& key, JsonValue const& value) {
        if (key == "protocol"sv) {
            if (!value.is_string() || !first_is_one_of(value.as_string(), "ctap1/u2f"sv, "ctap2"sv, "ctap2_1"sv)) {
                is_invalid = true;
                return;
            }
            configuration.protocol = value.as_string();
            has_protocol = true;
        } else if (key == "transport"sv) {
            if (!value.is_string() || !first_is_one_of(value.as_string(), "usb"sv, "nfc"sv, "ble"sv, "smart-card"sv, "hybrid"sv, "internal"sv)) {
                is_invalid = true;
                return;
            }
            configuration.transport = value.as_string();
            has_transport = true;
        } else if (key == "hasResidentKey"sv) {
            read_boolean(value, configuration.has_resident_key);
        } else if (key == "hasUserVerification"sv) {
            read_boolean(value, configuration.has_user_verification);
        } else if (key == "isUserConsenting"sv) {
            read_boolean(value, configuration.is_user_consenting);
        } else if (key == "isUserVerified"sv) {
            read_boolean(value, configuration.is_user_verified);
        } else if (key == "defaultBackupEligibility"sv) {
            read_boolean(value, configuration.default_backup_eligibility);
        } else if (key == "defaultBackupState"sv) {
            read_boolean(value, configuration.default_backup_state);
        } else if (key == "extensions"sv) {
            // FIXME: Support the extensions of the virtual authenticators.
            // NB: An extension that the authenticator does not have is an unsupported operation, and it has none.
            if (!value.is_array())
                is_invalid = true;
            else if (!value.as_array().is_empty())
                is_unsupported = true;
        } else {
            // If there is no matching key for key in Authenticator Configuration, return a WebDriver error with
            // WebDriver error code invalid argument.
            is_invalid = true;
        }
    });

    if (is_invalid || !has_protocol || !has_transport)
        return "invalid argument"_string;
    if (is_unsupported)
        return "unsupported operation"_string;

    auto authenticator_id = generate_random_uuid();
    m_virtual_authenticators.append({
        .id = authenticator_id,
        .owner_view_id = owner_view_id,
        .authenticator = make<Authenticator>(move(configuration)),
    });
    return JsonValue { authenticator_id };
}

// https://w3c.github.io/webauthn/#sctn-automation-remove-virtual-authenticator
ErrorOr<JsonValue, String> AuthenticatorManager::remove_virtual_authenticator(Optional<u64> owner_view_id, JsonObject const& parameters)
{
    auto authenticator_id = parameters.get_string("authenticatorId"sv);
    if (!authenticator_id.has_value())
        return "invalid argument"_string;
    bool removed = m_virtual_authenticators.remove_all_matching([&](auto const& virtual_authenticator) {
        return virtual_authenticator.id == *authenticator_id && virtual_authenticator.owner_view_id == owner_view_id;
    });
    if (!removed)
        return "invalid argument"_string;
    return JsonValue {};
}

// https://w3c.github.io/webauthn/#rp-id
static bool is_valid_rp_id(String const& rp_id)
{
    // NB: A valid RP ID is a valid domain string. It is also the serialization of its own host here, because that is
    //     the only form that a ceremony takes.
    auto host = URL::Parser::parse_host(rp_id);
    return host.has_value() && host->is_domain() && host->serialize() == rp_id;
}

static Optional<ByteBuffer> decode_base64url_member(JsonObject const& object, StringView key)
{
    auto encoded = object.get_string(key);
    if (!encoded.has_value())
        return {};
    auto decoded = decode_base64url(*encoded);
    if (decoded.is_error())
        return {};
    return decoded.release_value();
}

// https://w3c.github.io/webauthn/#sctn-automation-add-credential
ErrorOr<JsonValue, String> AuthenticatorManager::add_credential(Authenticator& authenticator, JsonObject const& parameters)
{
    auto credential_id = decode_base64url_member(parameters, "credentialId"sv);
    auto is_resident_credential = parameters.get_bool("isResidentCredential"sv);
    auto rp_id = parameters.get_string("rpId"sv);
    auto private_key = decode_base64url_member(parameters, "privateKey"sv);
    // NB: The signCount is a number, or null for a credential without a signature counter.
    auto sign_count_value = parameters.get("signCount"sv);
    auto sign_count = parameters.get_u32("signCount"sv);
    if (!sign_count_value.has_value() || (!sign_count_value->is_null() && !sign_count.has_value()))
        return "invalid argument"_string;
    if (!credential_id.has_value() || !is_resident_credential.has_value() || !rp_id.has_value() || !private_key.has_value())
        return "invalid argument"_string;

    if (!is_valid_rp_id(*rp_id))
        return "invalid argument"_string;

    Optional<ByteBuffer> user_handle;
    if (parameters.has("userHandle"sv) && !parameters.get("userHandle"sv)->is_null()) {
        user_handle = decode_base64url_member(parameters, "userHandle"sv);
        if (!user_handle.has_value() || user_handle->size() > 64)
            return "invalid argument"_string;
    }

    // NB: The private key must be one that the authenticator can sign with.
    if (!Authenticator::is_credential_private_key(*private_key))
        return "invalid argument"_string;

    // A client-side discoverable credential needs an authenticator that can store one, and a user handle.
    auto const& configuration = authenticator.configuration();
    if (*is_resident_credential && (!configuration.has_resident_key || !user_handle.has_value()))
        return "invalid argument"_string;

    auto optional_utf16 = [&](StringView key) -> Optional<Utf16String> {
        if (auto value = parameters.get_string(key); value.has_value())
            return Utf16String::from_utf8(*value);
        return {};
    };

    authenticator.add_credential({
        .id = credential_id.release_value(),
        .private_key = private_key.release_value(),
        .rp_id = *rp_id,
        .user_handle = move(user_handle),
        .is_resident_credential = *is_resident_credential,
        .sign_count = sign_count,
        .backup_eligibility = parameters.get_bool("backupEligibility"sv).value_or(configuration.default_backup_eligibility),
        .backup_state = parameters.get_bool("backupState"sv).value_or(configuration.default_backup_state),
        .user_name = optional_utf16("userName"sv),
        .user_display_name = optional_utf16("userDisplayName"sv),
        .creation_time = {},
        .last_used_time = {},
    });
    return JsonValue {};
}

// https://w3c.github.io/webauthn/#sctn-automation-get-credentials
ErrorOr<JsonValue, String> AuthenticatorManager::get_credentials(Authenticator& authenticator)
{
    JsonArray credentials;
    for (auto const& credential : authenticator.credentials()) {
        JsonObject object;
        object.set("credentialId"sv, MUST(encode_base64url(credential.id, AK::OmitPadding::Yes)));
        object.set("isResidentCredential"sv, credential.is_resident_credential);
        object.set("rpId"sv, credential.rp_id);
        object.set("privateKey"sv, MUST(encode_base64url(credential.private_key, AK::OmitPadding::Yes)));
        if (credential.user_handle.has_value())
            object.set("userHandle"sv, MUST(encode_base64url(*credential.user_handle, AK::OmitPadding::Yes)));
        if (credential.sign_count.has_value())
            object.set("signCount"sv, *credential.sign_count);
        else
            object.set("signCount"sv, JsonValue {});
        object.set("backupEligibility"sv, credential.backup_eligibility);
        object.set("backupState"sv, credential.backup_state);
        if (credential.user_name.has_value())
            object.set("userName"sv, credential.user_name->to_utf8_but_should_be_ported_to_utf16());
        if (credential.user_display_name.has_value())
            object.set("userDisplayName"sv, credential.user_display_name->to_utf8_but_should_be_ported_to_utf16());
        credentials.must_append(move(object));
    }
    return JsonValue { move(credentials) };
}

// https://w3c.github.io/webauthn/#sctn-automation-remove-credential
ErrorOr<JsonValue, String> AuthenticatorManager::remove_credential(Authenticator& authenticator, JsonObject const& parameters)
{
    auto credential_id = decode_base64url_member(parameters, "credentialId"sv);
    if (!credential_id.has_value() || !authenticator.remove_credential(*credential_id))
        return "invalid argument"_string;
    return JsonValue {};
}

// https://w3c.github.io/webauthn/#sctn-automation-set-user-verified
ErrorOr<JsonValue, String> AuthenticatorManager::set_user_verified(Authenticator& authenticator, JsonObject const& parameters)
{
    auto is_user_verified = parameters.get_bool("isUserVerified"sv);
    if (!is_user_verified.has_value())
        return "invalid argument"_string;
    authenticator.set_user_verified(*is_user_verified);
    return JsonValue {};
}

}
