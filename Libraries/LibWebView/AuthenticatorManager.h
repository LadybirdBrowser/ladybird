/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/WebAuthentication/AuthenticatorMessages.h>
#include <LibWebView/Authenticator.h>
#include <LibWebView/Forward.h>
#include <LibWebView/PasskeyPrompts.h>
#include <LibWebView/PlatformAuthenticatorBackend.h>
#include <LibWebView/SecurityKey.h>

namespace WebView {

// The WebAuthn client's side of a ceremony that must not be left to a renderer: it checks the Relying Party ID against
// the origin that the browser process knows the document by, builds the client data with that origin, and talks to
// the authenticators, which never hand a private key to a renderer.
class WEBVIEW_API AuthenticatorManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static AuthenticatorManager& the();

    // The page can take a request back while the user or an authenticator still works on it. Nothing that the
    // ceremony does for the request takes effect after that.
    class CeremonyCancellation : public RefCounted<CeremonyCancellation> {
    public:
        bool is_cancelled() const { return m_is_cancelled; }

        void cancel()
        {
            if (m_is_cancelled)
                return;
            m_is_cancelled = true;
            auto stop = move(on_cancel);
            on_cancel = nullptr;
            if (stop)
                stop();
        }

        // Stops what the ceremony waits for at the moment, if that can be stopped.
        Function<void()> on_cancel;

    private:
        bool m_is_cancelled { false };
    };

    // What the document is to the ceremony, as the browser process knows it.
    struct Caller {
        u64 view_id { 0 };
        URL::Origin origin;
        Optional<URL::Origin> top_level_origin;
        NonnullRefPtr<CeremonyCancellation> cancellation;
    };

    // How a ceremony talks to the user of the view. A caller with no user interface leaves the functions empty.
    struct UserInterface {
        // Asks the user to choose one of the options, and gives the index of it, or nothing when the user chooses
        // none. With no options for PasskeyChoiceKind::Account, the user is told that there is no passkey.
        Function<void(PasskeyChoiceKind, String const& rp_id, Vector<Utf16String> const& options, Function<void(Optional<u32>)> on_choice)> choose;
        // Tells the user to connect and touch a security key, until dismissed. The user can cancel.
        Function<void(SecurityKeyPromptKind, String const& rp_id, Function<void()> on_cancel)> show_security_key_prompt;
        // Takes away what the user is being shown or asked for the ceremony. A message stays.
        Function<void()> dismiss_passkey_prompt;
        // Asks the user for the PIN of the security key, and gives it, or nothing when the user gives none.
        Function<void(SecurityKeyPinPromptReason, String const& rp_id, Function<void(Optional<String>)> on_pin)> request_security_key_pin;
        Function<void(PasskeyMessage, String const& rp_id)> show_message;
    };

    // https://w3c.github.io/webauthn/#sctn-createCredential
    // NB: The result can come later, when the user has answered an authenticator.
    void make_credential(Caller, Web::WebAuthentication::MakeCredentialRequest, UserInterface, Function<void(Web::WebAuthentication::MakeCredentialResult)> on_complete);

    // https://w3c.github.io/webauthn/#sctn-getAssertion
    void get_assertion(Caller, Web::WebAuthentication::GetAssertionRequest, UserInterface, Function<void(Web::WebAuthentication::GetAssertionResult)> on_complete);

    // A passkey of the built-in authenticator, as the user sees it in the settings.
    struct StoredPasskey {
        // NB: The credential ID in base64url.
        String id;
        String rp_id;
        Utf16String account;
        Optional<UnixDateTime> creation_time;
        Optional<UnixDateTime> last_used_time;
    };
    // Whether this platform has a built-in authenticator that keeps passkeys.
    bool has_platform_passkeys();
    Optional<Vector<StoredPasskey>> platform_passkeys();
    // Whether the passkey is gone, from the platform too.
    bool remove_platform_passkey(StringView id);

    // Stops what the view waits for from a security key. The ceremony then fails.
    void cancel_security_key_ceremony(u64 view_id);
    // Whether the user of the view is asked for the ceremony of this request at the moment.
    bool is_ceremony_with_user(u64 view_id, CeremonyCancellation const&) const;

    // https://w3c.github.io/webauthn/#dom-publickeycredential-isuserverifyingplatformauthenticatoravailable
    bool is_user_verifying_platform_authenticator_available(u64 view_id);

    // Runs a command of the WebAuthn WebDriver extension, https://w3c.github.io/webauthn/#sctn-automation, on the
    // virtual authenticators of the owner: a view whose tests made them, or nothing for those of a WebDriver session.
    // The error is a WebDriver error code.
    ErrorOr<JsonValue, String> run_automation_command(Optional<u64> owner_view_id, StringView command, JsonObject const& parameters);

    void remove_virtual_authenticators_of_view(u64 view_id);
    bool has_virtual_authenticators_for(u64 view_id) { return !authenticators_available_to(view_id).is_empty(); }

private:
    // https://w3c.github.io/webauthn/#virtual-authenticators
    struct VirtualAuthenticator {
        String id;
        Optional<u64> owner_view_id;
        NonnullOwnPtr<Authenticator> authenticator;
    };

    Vector<Authenticator&> authenticators_available_to(u64 view_id);

    ErrorOr<Web::WebAuthentication::MakeCredentialResponse, Web::WebAuthentication::AuthenticatorError> make_credential_using(Vector<Authenticator&>&, Caller const&, Web::WebAuthentication::MakeCredentialRequest const&);
    ErrorOr<Web::WebAuthentication::GetAssertionResponse, Web::WebAuthentication::AuthenticatorError> get_assertion_using(Vector<Authenticator&>&, Caller const&, Web::WebAuthentication::GetAssertionRequest const&);

    // The authenticator that is built into the browser, which keeps its credentials with the platform and asks the
    // user of the device for every ceremony. There is none in a test, and none on a platform without a backend.
    PlatformAuthenticatorBackend* platform_backend();
    Authenticator& platform_authenticator();
    Optional<Authenticator&> virtual_authenticator(Optional<u64> owner_view_id, JsonObject const& parameters);

    ErrorOr<JsonValue, String> add_virtual_authenticator(Optional<u64> owner_view_id, JsonObject const& parameters);
    ErrorOr<JsonValue, String> remove_virtual_authenticator(Optional<u64> owner_view_id, JsonObject const& parameters);
    ErrorOr<JsonValue, String> add_credential(Authenticator&, JsonObject const& parameters);
    ErrorOr<JsonValue, String> get_credentials(Authenticator&);
    ErrorOr<JsonValue, String> remove_credential(Authenticator&, JsonObject const& parameters);
    ErrorOr<JsonValue, String> set_user_verified(Authenticator&, JsonObject const& parameters);

    // NB: With a user interface, the user can turn to a security key when the platform does not verify them.
    Web::WebAuthentication::MakeCredentialResult make_credential_with_platform_authenticator(Caller const&, Web::WebAuthentication::MakeCredentialRequest const&);
    void make_credential_with_security_key(Caller, Web::WebAuthentication::MakeCredentialRequest, UserInterface, Function<void(Web::WebAuthentication::MakeCredentialResult)> on_complete);
    void get_assertion_with_security_key(Caller, Web::WebAuthentication::GetAssertionRequest, UserInterface, Function<void(Web::WebAuthentication::GetAssertionResult)> on_complete);

    // A ceremony that waits for the user to touch a security key. A view has one at most.
    struct SecurityKeyCeremony {
        u64 id { 0 };
        NonnullRefPtr<SecurityKeySession> session;
        Function<void()> dismiss_prompt;
        Function<void()> on_cancel;
    };
    bool can_use_security_keys(UserInterface const&) const;
    struct SecurityKeyMakeCredentialState;
    struct SecurityKeyGetAssertionState;
    void run_security_key_make_credential(NonnullRefPtr<SecurityKeyMakeCredentialState>);
    void run_security_key_get_assertion(NonnullRefPtr<SecurityKeyGetAssertionState>);
    template<typename State>
    void handle_security_key_error(NonnullRefPtr<State>, SecurityKeyError, void (AuthenticatorManager::*run_again)(NonnullRefPtr<State>));
    u64 start_security_key_ceremony(u64 view_id, NonnullRefPtr<SecurityKeySession>, Function<void()> dismiss_prompt, Function<void()> on_cancel);
    void finish_security_key_ceremony(u64 view_id);
    void cancel_security_key_ceremony(u64 view_id, u64 ceremony_id);
    HashMap<u64, SecurityKeyCeremony> m_security_key_ceremonies;

    // The ceremony of each view that the user is asked for. A view has one such ceremony at a time: The user has
    // one prompt to answer, and a request cannot take away the prompt of another.
    HashMap<u64, NonnullRefPtr<CeremonyCancellation>> m_ceremonies_with_user;
    bool begin_ceremony_with_user(Caller const&);
    void end_ceremony_with_user(u64 view_id, CeremonyCancellation const&);
    u64 m_next_security_key_ceremony_id { 1 };

    Vector<VirtualAuthenticator> m_virtual_authenticators;

    bool m_did_create_platform_backend { false };
    OwnPtr<PlatformAuthenticatorBackend> m_platform_backend;
    OwnPtr<Authenticator> m_platform_authenticator;
    bool m_has_loaded_platform_credentials { false };
};

}
