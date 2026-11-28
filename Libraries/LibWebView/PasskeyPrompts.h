/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace WebView {

// What the user chooses among, in a WebAuthn ceremony.
enum class PasskeyChoiceKind : u8 {
    // The accounts of the passkeys to sign in with.
    Account,
    // The same, with a security key to sign in with as the last option.
    AccountOrSecurityKey,
    // The accounts of the passkeys to sign in with, offered for a form control that the user is in. The user can go
    // on typing in the control instead.
    AccountForFormControl,
    // The authenticators to create a passkey on.
    Authenticator,
    // How to go on after the platform did not verify the user: again, or with a security key.
    OtherWaysToSignIn,
    OtherWaysToCreate,
};

// What the user touches a security key for.
enum class SecurityKeyPromptKind : u8 {
    SignIn,
    CreatePasskey,
};

// Why the user is asked for the PIN of a security key.
enum class SecurityKeyPinPromptReason : u8 {
    Required,
    // The key did not take the PIN that the user gave before.
    Wrong,
};

// What the user is told when a ceremony cannot go on.
enum class PasskeyMessage : u8 {
    SecurityKeyHasNoPasskey,
    SecurityKeyAlreadyHasPasskey,
    SecurityKeyPinBlocked,
    SecurityKeyNotSupported,
};

}
