/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a ceremony does when the security keys gave no credential: ask the user for a PIN and run again, or end, with
//! or without a message for the user.

use super::ctap::SecurityKeyError;
use crate::authenticator::AuthenticatorError;
use crate::rust_panic::abort_on_panic;

/// Why the user is asked for the PIN of a security key.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SecurityKeyPinPromptReason {
    Required,
    /// The key did not take the PIN that the user gave before.
    Wrong,
}

/// What the user is told when a ceremony cannot go on.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PasskeyMessage {
    SecurityKeyHasNoPasskey,
    SecurityKeyAlreadyHasPasskey,
    SecurityKeyPinBlocked,
    SecurityKeyNotSupported,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AfterError {
    /// The user is asked for the PIN, and the ceremony runs again with it.
    AskForPin(SecurityKeyPinPromptReason),
    /// The ceremony ends with the error, after the user is told why, when there is something to tell.
    End(Option<PasskeyMessage>, AuthenticatorError),
}

pub fn after_error(error: SecurityKeyError, can_ask_for_pin: bool) -> AfterError {
    match error {
        SecurityKeyError::PinRequired if can_ask_for_pin => AfterError::AskForPin(SecurityKeyPinPromptReason::Required),
        SecurityKeyError::PinInvalid if can_ask_for_pin => AfterError::AskForPin(SecurityKeyPinPromptReason::Wrong),
        // -> If any authenticator returns an error status equivalent to "InvalidStateError",
        //    Throw an "InvalidStateError" DOMException.
        SecurityKeyError::CredentialExcluded => AfterError::End(
            Some(PasskeyMessage::SecurityKeyAlreadyHasPasskey),
            AuthenticatorError::InvalidState,
        ),
        // NB: Every other error is one that the Relying Party must not tell apart from a user who did not consent.
        SecurityKeyError::NoCredentials => AfterError::End(
            Some(PasskeyMessage::SecurityKeyHasNoPasskey),
            AuthenticatorError::NotAllowed,
        ),
        SecurityKeyError::PinBlocked => AfterError::End(
            Some(PasskeyMessage::SecurityKeyPinBlocked),
            AuthenticatorError::NotAllowed,
        ),
        SecurityKeyError::NotSupported => AfterError::End(
            Some(PasskeyMessage::SecurityKeyNotSupported),
            AuthenticatorError::NotAllowed,
        ),
        SecurityKeyError::PinRequired | SecurityKeyError::PinInvalid | SecurityKeyError::Failed => {
            AfterError::End(None, AuthenticatorError::NotAllowed)
        }
    }
}

#[repr(C)]
pub struct FfiAfterSecurityKeyError {
    pub asks_for_pin: bool,
    pub pin_prompt_reason: SecurityKeyPinPromptReason,
    pub has_message: bool,
    pub message: PasskeyMessage,
    pub error: AuthenticatorError,
}

#[unsafe(no_mangle)]
pub extern "C" fn webview_after_security_key_error(
    error: SecurityKeyError,
    can_ask_for_pin: bool,
) -> FfiAfterSecurityKeyError {
    abort_on_panic(|| {
        let mut result = FfiAfterSecurityKeyError {
            asks_for_pin: false,
            pin_prompt_reason: SecurityKeyPinPromptReason::Required,
            has_message: false,
            message: PasskeyMessage::SecurityKeyHasNoPasskey,
            error: AuthenticatorError::NotAllowed,
        };
        match after_error(error, can_ask_for_pin) {
            AfterError::AskForPin(reason) => {
                result.asks_for_pin = true;
                result.pin_prompt_reason = reason;
            }
            AfterError::End(message, error) => {
                result.has_message = message.is_some();
                result.message = message.unwrap_or(PasskeyMessage::SecurityKeyHasNoPasskey);
                result.error = error;
            }
        }
        result
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_key_that_wants_a_pin_makes_the_client_ask_for_it() {
        assert_eq!(
            after_error(SecurityKeyError::PinRequired, true),
            AfterError::AskForPin(SecurityKeyPinPromptReason::Required)
        );
        assert_eq!(
            after_error(SecurityKeyError::PinInvalid, true),
            AfterError::AskForPin(SecurityKeyPinPromptReason::Wrong)
        );
    }

    #[test]
    fn a_client_that_cannot_ask_for_a_pin_ends_the_ceremony() {
        for error in [SecurityKeyError::PinRequired, SecurityKeyError::PinInvalid] {
            assert_eq!(
                after_error(error, false),
                AfterError::End(None, AuthenticatorError::NotAllowed)
            );
        }
    }

    #[test]
    fn only_an_excluded_credential_is_told_to_the_relying_party() {
        assert_eq!(
            after_error(SecurityKeyError::CredentialExcluded, true),
            AfterError::End(
                Some(PasskeyMessage::SecurityKeyAlreadyHasPasskey),
                AuthenticatorError::InvalidState
            )
        );
        for (error, message) in [
            (
                SecurityKeyError::NoCredentials,
                Some(PasskeyMessage::SecurityKeyHasNoPasskey),
            ),
            (
                SecurityKeyError::PinBlocked,
                Some(PasskeyMessage::SecurityKeyPinBlocked),
            ),
            (
                SecurityKeyError::NotSupported,
                Some(PasskeyMessage::SecurityKeyNotSupported),
            ),
            (SecurityKeyError::Failed, None),
        ] {
            assert_eq!(
                after_error(error, true),
                AfterError::End(message, AuthenticatorError::NotAllowed)
            );
        }
    }
}
