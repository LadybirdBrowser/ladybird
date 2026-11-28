/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How a ceremony goes on with the user, when the authenticator of the platform or a security key can answer it:
//! which of the two gets the request, what the user is asked, and what an answer leads to. The flow gives one step
//! at a time, and who drives it tells it what came of the step.

use crate::authenticator::COSE_ALGORITHM_ES256;
use crate::ceremony::AuthenticatorAttachment;
use crate::rust_panic::abort_on_panic;

/// What the client has to go on with a ceremony.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Means {
    /// Whether the authenticator of the platform is there and can verify the user.
    pub can_verify_user_with_platform: bool,
    /// Whether the client can reach security keys, and can show the user what to do with one.
    pub can_use_security_keys: bool,
    /// Whether the client can ask the user to choose.
    pub can_ask_user: bool,
    pub is_security_key_connected: bool,
}

/// What the user is asked. Each question but the ones about accounts has its own fixed answers.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Question {
    /// Where to create the credential: on this device (0), or on a security key (1).
    Authenticator,
    /// Which account to sign in with, of those that the platform has a credential for, the newest first.
    Account,
    /// As for `Account`, with a security key as the answer after the accounts.
    AccountOrSecurityKey,
    /// As for `Account`, for a request with conditional mediation.
    AccountForFormControl,
    /// Nothing: The user is told that there is no passkey for the site.
    NoPasskey,
    /// How to go on after the platform did not verify the user: try again (0), or use a security key (1).
    OtherWaysToSignIn,
    OtherWaysToCreate,
}

/// What a ceremony does next.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// The ceremony ends with an error equivalent to "NotAllowedError".
    Refuse,
    /// The security keys get the request, as the Relying Party made it.
    UseSecurityKey,
    Ask(Question),
    /// The platform verifies the user, with a prompt of its own.
    VerifyUser,
    /// The authenticator of the platform does the operation, with the account that the user chose, when they did.
    Proceed {
        account: Option<u32>,
    },
}

/// What came of a step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    /// The answer of the user to a question, or nothing when they gave none.
    Chose(Option<u32>),
    /// Whether the platform verified the user.
    Verified(bool),
    /// The page took its request back.
    Cancelled,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    AskingForAuthenticator,
    AskingForAccount,
    TellingThatThereIsNoPasskey,
    VerifyingUser,
    AskingForOtherWays,
    Done,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Flow {
    state: State,
    is_for_assertion: bool,
    /// Whether the user can turn to a security key when the platform did not verify them.
    has_other_ways: bool,
    /// Whether a security key is among the answers to the question about the account.
    offers_security_key: bool,
    account_count: u32,
    has_account: bool,
    account: u32,
}

impl Flow {
    fn new(is_for_assertion: bool) -> Self {
        Self {
            state: State::Done,
            is_for_assertion,
            has_other_ways: false,
            offers_security_key: false,
            account_count: 0,
            has_account: false,
            account: 0,
        }
    }

    fn step(&mut self, state: State, step: Step) -> Step {
        self.state = state;
        step
    }

    fn end(&mut self, step: Step) -> Step {
        self.step(State::Done, step)
    }

    fn verify_user(&mut self) -> Step {
        self.step(State::VerifyingUser, Step::VerifyUser)
    }

    /// The flow of a ceremony that creates a credential, and its first step.
    pub fn for_new_credential(
        means: &Means,
        authenticator_attachment: Option<AuthenticatorAttachment>,
        public_key_algorithms: &[i32],
    ) -> (Self, Step) {
        let mut flow = Self::new(false);

        // If pkOptions.authenticatorSelection.authenticatorAttachment is present and its value is not equal to
        // authenticator's authenticator attachment modality, continue.
        // NB: The built-in authenticator makes ES256 keys only. The user is not asked to verify for a credential that
        //     it then cannot make.
        let can_use_platform_authenticator = means.can_verify_user_with_platform
            && authenticator_attachment != Some(AuthenticatorAttachment::CrossPlatform)
            && public_key_algorithms.contains(&COSE_ALGORITHM_ES256);
        let can_use_security_key =
            means.can_use_security_keys && authenticator_attachment != Some(AuthenticatorAttachment::Platform);

        if !can_use_platform_authenticator {
            let step = if can_use_security_key {
                Step::UseSecurityKey
            } else {
                Step::Refuse
            };
            return (flow, step);
        }

        // NB: The user is asked where to create the credential only when a security key is there to take it. Without
        //     one the built-in authenticator is the place, and the user can still turn to a security key when the
        //     platform did not verify them.
        flow.has_other_ways = can_use_security_key && means.can_ask_user;
        let step = if flow.has_other_ways && means.is_security_key_connected {
            flow.step(State::AskingForAuthenticator, Step::Ask(Question::Authenticator))
        } else {
            flow.verify_user()
        };
        (flow, step)
    }

    /// The flow of a ceremony that gets an assertion, and its first step. The accounts are the credentials that the
    /// authenticator of the platform has for the request.
    pub fn for_assertion(means: &Means, is_conditional: bool, account_count: u32) -> (Self, Step) {
        let mut flow = Self::new(true);
        let account_count = if means.can_verify_user_with_platform {
            account_count
        } else {
            0
        };
        flow.account_count = account_count;

        if account_count == 0 {
            // NB: Without a credential in the built-in authenticator, a security key can have one. A request with
            //     conditional mediation does not ask the user for a key.
            if !is_conditional && means.can_use_security_keys {
                return (flow, Step::UseSecurityKey);
            }
            // NB: Without a credential for the request, the user is told so, and the ceremony fails when they have
            //     seen it: A Relying Party then cannot tell from the time alone that no credential exists. A request
            //     with conditional mediation has nothing to offer in the form control, and tells the user nothing.
            if is_conditional || !means.can_verify_user_with_platform || !means.can_ask_user {
                return (flow, Step::Refuse);
            }
            let step = flow.step(State::TellingThatThereIsNoPasskey, Step::Ask(Question::NoPasskey));
            return (flow, step);
        }

        // NB: The user can turn to a security key when the platform did not verify them, also when none is connected
        //     yet.
        flow.has_other_ways = !is_conditional && means.can_use_security_keys && means.can_ask_user;
        // NB: A security key that is connected can have a credential of its own for the Relying Party, so the user
        //     can choose it over those of the built-in authenticator.
        flow.offers_security_key = !is_conditional && means.can_use_security_keys && means.is_security_key_connected;

        // NB: With conditional mediation the user has not asked to sign in yet, so they are always asked, also about
        //     one credential.
        let needs_no_choice = account_count == 1 && !is_conditional && !flow.offers_security_key;
        if needs_no_choice || !means.can_ask_user {
            let step = flow.verify_user();
            return (flow, step);
        }

        // Prompt the user to select a public key credential source selectedCredential from credentialOptions.
        let question = if is_conditional {
            Question::AccountForFormControl
        } else if flow.offers_security_key {
            Question::AccountOrSecurityKey
        } else {
            Question::Account
        };
        let step = flow.step(State::AskingForAccount, Step::Ask(question));
        (flow, step)
    }

    /// The next step, for what came of the last one.
    pub fn handle(&mut self, event: Event) -> Step {
        match (self.state, event) {
            (State::AskingForAuthenticator | State::AskingForOtherWays, Event::Chose(Some(0))) => self.verify_user(),
            (State::AskingForAuthenticator | State::AskingForOtherWays, Event::Chose(Some(1))) => {
                self.end(Step::UseSecurityKey)
            }
            (State::AskingForAccount, Event::Chose(Some(answer))) if answer < self.account_count => {
                // NB: The authenticator then has one credential to use for the request.
                self.has_account = true;
                self.account = answer;
                self.verify_user()
            }
            (State::AskingForAccount, Event::Chose(Some(answer)))
                if answer == self.account_count && self.offers_security_key =>
            {
                self.end(Step::UseSecurityKey)
            }
            (State::VerifyingUser, Event::Verified(true)) => {
                let account = self.has_account.then_some(self.account);
                self.end(Step::Proceed { account })
            }
            // NB: The prompt of the platform has no room for another way to go on. A user who does not verify there,
            //     such as one who wants to use a security key, is asked how to go on instead.
            (State::VerifyingUser, Event::Verified(false)) if self.has_other_ways => {
                let question = if self.is_for_assertion {
                    Question::OtherWaysToSignIn
                } else {
                    Question::OtherWaysToCreate
                };
                self.step(State::AskingForOtherWays, Step::Ask(question))
            }
            // If the user does not consent or if user verification fails, return an error code equivalent to
            // "NotAllowedError" and terminate the operation.
            // NB: This is also what a page gets that took its request back, and what follows an answer that the user
            //     was not offered.
            _ => self.end(Step::Refuse),
        }
    }
}

#[repr(u8)]
pub enum FfiFlowStepKind {
    Refuse,
    UseSecurityKey,
    Ask,
    VerifyUser,
    Proceed,
}

#[repr(C)]
pub struct FfiFlowStep {
    pub kind: FfiFlowStepKind,
    /// What to ask the user, for `Ask`.
    pub question: Question,
    /// Whether the user chose an account, for `Proceed`.
    pub has_account: bool,
    /// The account that the user chose, counted from the newest.
    pub account: u32,
}

impl From<Step> for FfiFlowStep {
    fn from(step: Step) -> Self {
        let mut ffi_step = Self {
            kind: FfiFlowStepKind::Refuse,
            question: Question::NoPasskey,
            has_account: false,
            account: 0,
        };
        match step {
            Step::Refuse => {}
            Step::UseSecurityKey => ffi_step.kind = FfiFlowStepKind::UseSecurityKey,
            Step::Ask(question) => {
                ffi_step.kind = FfiFlowStepKind::Ask;
                ffi_step.question = question;
            }
            Step::VerifyUser => ffi_step.kind = FfiFlowStepKind::VerifyUser,
            Step::Proceed { account } => {
                ffi_step.kind = FfiFlowStepKind::Proceed;
                ffi_step.has_account = account.is_some();
                ffi_step.account = account.unwrap_or(0);
            }
        }
        ffi_step
    }
}

/// Starts the flow of a ceremony that creates a credential, and gives its first step.
///
/// # Safety
/// `public_key_algorithms` points to `public_key_algorithm_count` items.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_flow_for_new_credential(
    means: &Means,
    has_authenticator_attachment: bool,
    authenticator_attachment: AuthenticatorAttachment,
    public_key_algorithms: *const i32,
    public_key_algorithm_count: usize,
    flow: &mut Flow,
) -> FfiFlowStep {
    abort_on_panic(|| {
        // SAFETY: The caller says so.
        let public_key_algorithms =
            unsafe { ak::slice_from_raw_parts(public_key_algorithms, public_key_algorithm_count) };
        let attachment = has_authenticator_attachment.then_some(authenticator_attachment);
        let (new_flow, step) = Flow::for_new_credential(means, attachment, public_key_algorithms);
        *flow = new_flow;
        step.into()
    })
}

/// Starts the flow of a ceremony that gets an assertion, and gives its first step.
#[unsafe(no_mangle)]
pub extern "C" fn webview_flow_for_assertion(
    means: &Means,
    is_conditional: bool,
    account_count: u32,
    flow: &mut Flow,
) -> FfiFlowStep {
    abort_on_panic(|| {
        let (new_flow, step) = Flow::for_assertion(means, is_conditional, account_count);
        *flow = new_flow;
        step.into()
    })
}

/// The next step after the user answered a question, or gave no answer.
#[unsafe(no_mangle)]
pub extern "C" fn webview_flow_user_chose(flow: &mut Flow, has_answer: bool, answer: u32) -> FfiFlowStep {
    abort_on_panic(|| flow.handle(Event::Chose(has_answer.then_some(answer))).into())
}

/// The next step after the platform verified the user, or did not.
#[unsafe(no_mangle)]
pub extern "C" fn webview_flow_user_was_verified(flow: &mut Flow, is_verified: bool) -> FfiFlowStep {
    abort_on_panic(|| flow.handle(Event::Verified(is_verified)).into())
}

/// The last step, for a page that took its request back.
#[unsafe(no_mangle)]
pub extern "C" fn webview_flow_was_cancelled(flow: &mut Flow) -> FfiFlowStep {
    abort_on_panic(|| flow.handle(Event::Cancelled).into())
}

#[cfg(test)]
mod tests {
    use super::*;

    const ES256: &[i32] = &[COSE_ALGORITHM_ES256];

    fn means() -> Means {
        Means {
            can_verify_user_with_platform: true,
            can_use_security_keys: true,
            can_ask_user: true,
            is_security_key_connected: false,
        }
    }

    fn with_key_connected() -> Means {
        Means {
            is_security_key_connected: true,
            ..means()
        }
    }

    fn without_platform() -> Means {
        Means {
            can_verify_user_with_platform: false,
            ..means()
        }
    }

    fn without_security_keys() -> Means {
        Means {
            can_use_security_keys: false,
            ..means()
        }
    }

    #[test]
    fn a_new_credential_goes_straight_to_the_platform_without_a_key_connected() {
        let (mut flow, step) = Flow::for_new_credential(&means(), None, ES256);
        assert_eq!(step, Step::VerifyUser);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Proceed { account: None });
    }

    #[test]
    fn a_user_whom_the_platform_did_not_verify_can_turn_to_a_security_key() {
        let (mut flow, step) = Flow::for_new_credential(&means(), None, ES256);
        assert_eq!(step, Step::VerifyUser);
        assert_eq!(
            flow.handle(Event::Verified(false)),
            Step::Ask(Question::OtherWaysToCreate)
        );
        assert_eq!(flow.handle(Event::Chose(Some(1))), Step::UseSecurityKey);

        let (mut flow, _) = Flow::for_assertion(&means(), false, 1);
        assert_eq!(
            flow.handle(Event::Verified(false)),
            Step::Ask(Question::OtherWaysToSignIn)
        );
        // NB: The user can try the platform as often as they like.
        assert_eq!(flow.handle(Event::Chose(Some(0))), Step::VerifyUser);
        assert_eq!(
            flow.handle(Event::Verified(false)),
            Step::Ask(Question::OtherWaysToSignIn)
        );
        assert_eq!(flow.handle(Event::Chose(Some(0))), Step::VerifyUser);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Proceed { account: None });
    }

    #[test]
    fn without_another_way_a_failed_verification_ends_the_ceremony() {
        for means in [
            without_security_keys(),
            Means {
                can_ask_user: false,
                ..means()
            },
        ] {
            let (mut flow, step) = Flow::for_new_credential(&means, None, ES256);
            assert_eq!(step, Step::VerifyUser);
            assert_eq!(flow.handle(Event::Verified(false)), Step::Refuse);
        }
        // NB: The Relying Party asked for the authenticator of the platform.
        let (mut flow, _) = Flow::for_new_credential(&means(), Some(AuthenticatorAttachment::Platform), ES256);
        assert_eq!(flow.handle(Event::Verified(false)), Step::Refuse);
        // NB: A request with conditional mediation does not ask the user for a key.
        let (mut flow, _) = Flow::for_assertion(&means(), true, 1);
        assert_eq!(flow.handle(Event::Chose(Some(0))), Step::VerifyUser);
        assert_eq!(flow.handle(Event::Verified(false)), Step::Refuse);
    }

    #[test]
    fn the_user_chooses_where_to_create_when_a_key_is_connected() {
        let start = || Flow::for_new_credential(&with_key_connected(), None, ES256);
        let (mut flow, step) = start();
        assert_eq!(step, Step::Ask(Question::Authenticator));
        assert_eq!(flow.handle(Event::Chose(Some(0))), Step::VerifyUser);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Proceed { account: None });

        let (mut flow, _) = start();
        assert_eq!(flow.handle(Event::Chose(Some(1))), Step::UseSecurityKey);
        for answer in [None, Some(2)] {
            let (mut flow, _) = start();
            assert_eq!(flow.handle(Event::Chose(answer)), Step::Refuse);
        }
    }

    #[test]
    fn a_new_credential_goes_to_the_authenticator_that_can_make_it() {
        let first_step =
            |means: Means, attachment, algorithms| Flow::for_new_credential(&means, attachment, algorithms).1;
        let platform = Some(AuthenticatorAttachment::Platform);
        let cross_platform = Some(AuthenticatorAttachment::CrossPlatform);

        assert_eq!(first_step(without_platform(), None, ES256), Step::UseSecurityKey);
        assert_eq!(first_step(means(), cross_platform, ES256), Step::UseSecurityKey);
        // NB: The built-in authenticator makes ES256 keys only.
        assert_eq!(first_step(means(), None, &[-257]), Step::UseSecurityKey);
        assert_eq!(first_step(with_key_connected(), platform, ES256), Step::VerifyUser);
        assert_eq!(first_step(without_platform(), platform, ES256), Step::Refuse);
        assert_eq!(first_step(without_security_keys(), cross_platform, ES256), Step::Refuse);
    }

    #[test]
    fn an_assertion_without_a_passkey_of_the_platform_goes_to_a_security_key() {
        let first_step =
            |means: Means, is_conditional, account_count| Flow::for_assertion(&means, is_conditional, account_count).1;
        assert_eq!(first_step(means(), false, 0), Step::UseSecurityKey);
        // NB: The passkeys of a platform that cannot verify the user are of no use.
        assert_eq!(first_step(without_platform(), false, 3), Step::UseSecurityKey);
        assert_eq!(first_step(without_platform(), true, 3), Step::Refuse);
        let neither = Means {
            can_use_security_keys: false,
            ..without_platform()
        };
        assert_eq!(first_step(neither, false, 0), Step::Refuse);
    }

    #[test]
    fn the_user_is_told_when_there_is_no_passkey() {
        let (mut flow, step) = Flow::for_assertion(&without_security_keys(), false, 0);
        assert_eq!(step, Step::Ask(Question::NoPasskey));
        assert_eq!(flow.handle(Event::Chose(Some(0))), Step::Refuse);
        // NB: A request with conditional mediation has nothing to offer, and tells the user nothing.
        assert_eq!(Flow::for_assertion(&means(), true, 0).1, Step::Refuse);
        let cannot_ask = Means {
            can_ask_user: false,
            ..without_security_keys()
        };
        assert_eq!(Flow::for_assertion(&cannot_ask, false, 0).1, Step::Refuse);
    }

    #[test]
    fn one_passkey_needs_no_choice_of_account() {
        let (mut flow, step) = Flow::for_assertion(&means(), false, 1);
        assert_eq!(step, Step::VerifyUser);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Proceed { account: None });

        // NB: With conditional mediation the user has not asked to sign in yet.
        assert_eq!(
            Flow::for_assertion(&means(), true, 1).1,
            Step::Ask(Question::AccountForFormControl)
        );
        // NB: A key that is connected can have a credential of its own.
        assert_eq!(
            Flow::for_assertion(&with_key_connected(), false, 1).1,
            Step::Ask(Question::AccountOrSecurityKey)
        );
    }

    #[test]
    fn the_user_chooses_the_account_of_an_assertion() {
        let (mut flow, step) = Flow::for_assertion(&means(), false, 3);
        assert_eq!(step, Step::Ask(Question::Account));
        assert_eq!(flow.handle(Event::Chose(Some(2))), Step::VerifyUser);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Proceed { account: Some(2) });

        // NB: A security key is no answer that the user was offered.
        for answer in [None, Some(3), Some(4)] {
            let (mut flow, _) = Flow::for_assertion(&means(), false, 3);
            assert_eq!(flow.handle(Event::Chose(answer)), Step::Refuse);
        }

        let (mut flow, step) = Flow::for_assertion(&with_key_connected(), false, 3);
        assert_eq!(step, Step::Ask(Question::AccountOrSecurityKey));
        assert_eq!(flow.handle(Event::Chose(Some(3))), Step::UseSecurityKey);
        let (mut flow, _) = Flow::for_assertion(&with_key_connected(), false, 3);
        assert_eq!(flow.handle(Event::Chose(Some(4))), Step::Refuse);
    }

    #[test]
    fn a_client_that_cannot_ask_lets_the_authenticator_select_the_account() {
        let cannot_ask = Means {
            can_ask_user: false,
            ..means()
        };
        let (mut flow, step) = Flow::for_assertion(&cannot_ask, false, 3);
        assert_eq!(step, Step::VerifyUser);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Proceed { account: None });
    }

    #[test]
    fn a_request_that_the_page_took_back_ends_the_ceremony() {
        let (mut flow, _) = Flow::for_assertion(&means(), false, 3);
        assert_eq!(flow.handle(Event::Cancelled), Step::Refuse);
        // NB: An answer that comes after the end changes nothing.
        assert_eq!(flow.handle(Event::Chose(Some(0))), Step::Refuse);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Refuse);

        let (mut flow, _) = Flow::for_new_credential(&means(), None, ES256);
        assert_eq!(flow.handle(Event::Cancelled), Step::Refuse);
        assert_eq!(flow.handle(Event::Verified(true)), Step::Refuse);
    }

    #[test]
    fn what_came_of_another_step_ends_the_ceremony() {
        let (mut flow, step) = Flow::for_assertion(&means(), false, 3);
        assert_eq!(step, Step::Ask(Question::Account));
        assert_eq!(flow.handle(Event::Verified(true)), Step::Refuse);

        let (mut flow, step) = Flow::for_new_credential(&means(), None, ES256);
        assert_eq!(step, Step::VerifyUser);
        assert_eq!(flow.handle(Event::Chose(Some(0))), Step::Refuse);
    }
}
