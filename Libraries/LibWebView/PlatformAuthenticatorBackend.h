/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebView/Authenticator.h>
#include <LibWebView/Forward.h>

namespace WebView {

// What the built-in authenticator needs from the platform: a place for the private keys that other programs cannot
// read, and a way to verify that the user of the device is present and agrees.
class WEBVIEW_API PlatformAuthenticatorBackend {
public:
    AK_ALLOC_WITH_KMALLOC;

    // The backend of this platform, or nothing where there is none yet.
    static OwnPtr<PlatformAuthenticatorBackend> create();

    virtual ~PlatformAuthenticatorBackend() = default;

    // Whether the platform can verify the user, with a biometric or with the password of the device.
    virtual bool can_verify_user() = 0;

    // Asks the user of the device to agree to the action, which completes a sentence that starts with the name of the
    // browser, and verifies who they are. The answer comes later, on the event loop that asked.
    // NB: The platform shows one prompt at a time. A new user verification fails while one is open.
    //     The result is the ID of the user verification, to take it back with.
    virtual u64 verify_user(String const& action, Function<void(bool is_verified)> on_complete) = 0;
    // Takes away the prompt of the user verification if it is still open, which then answers that the user is not
    // verified.
    virtual void cancel_user_verification(u64 user_verification_id) = 0;

    // The credentials that the platform keeps, or nothing when it could not read them.
    virtual Optional<Vector<PublicKeyCredentialSource>> load_credentials() = 0;
    virtual bool store_credential(PublicKeyCredentialSource const&) = 0;
    // Writes a credential that the platform already has again, with what changed in it.
    virtual void update_credential(PublicKeyCredentialSource const&) = 0;
    // Whether the platform no longer has the credential. That is so too when it did not have it.
    virtual bool remove_credential(ReadonlyBytes credential_id) = 0;
};

}
