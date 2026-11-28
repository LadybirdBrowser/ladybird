/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::Secret;
use super::ctap::{
    self, GetAssertionRequest, MakeCredentialRequest, Request, ResidentKeyRequirement, SecurityKeyError,
    UserVerificationRequirement,
};
use super::fake_key::{
    CREDENTIAL_PUBLIC_KEY_X, CREDENTIAL_PUBLIC_KEY_Y, Config, Credential, FakeHost, FakeKey, SharedKey,
};
use super::session::{Outcome, Pin, Session};

const RP_ID: &str = "example.com";
const PIN: &[u8] = b"1234";

fn make_credential_request() -> MakeCredentialRequest {
    MakeCredentialRequest {
        client_data_hash: vec![0xcd; 32],
        rp_id: RP_ID.to_owned(),
        rp_name: "Example".to_owned(),
        user_id: vec![1, 2, 3],
        user_name: "user@example.com".to_owned(),
        user_display_name: "User".to_owned(),
        public_key_algorithms: vec![-7],
        exclude_credential_ids: Vec::new(),
        resident_key: ResidentKeyRequirement::Discouraged,
        user_verification: UserVerificationRequirement::Discouraged,
    }
}

fn get_assertion_request() -> GetAssertionRequest {
    GetAssertionRequest {
        client_data_hash: vec![0xcd; 32],
        rp_id: RP_ID.to_owned(),
        allow_credential_ids: Vec::new(),
        user_verification: UserVerificationRequirement::Discouraged,
    }
}

fn credential(id: u8, user_name: &'static str) -> Credential {
    Credential {
        rp_id: RP_ID,
        id: vec![id; 4],
        user_id: vec![id],
        user_name,
    }
}

fn pin_for(device_id: u64, pin: &[u8]) -> Pin {
    Pin {
        bytes: Secret::new(pin.to_vec()),
        device_id,
    }
}

fn key_touched_at(touch_at: u64, config: Config) -> SharedKey {
    FakeKey::shared(Config {
        touch_at: Some(touch_at),
        ..config
    })
}

fn key_with_pin(config: Config) -> Config {
    Config {
        pin: Some(PIN),
        ..config
    }
}

fn run(host: &mut FakeHost, request: &Request, pin: Option<&Pin>) -> Outcome {
    Session::new(request, pin, host).run()
}

fn make_credential(host: &mut FakeHost, pin: Option<&Pin>) -> Outcome {
    run(host, &Request::MakeCredential(make_credential_request()), pin)
}

fn get_assertion(host: &mut FakeHost, pin: Option<&Pin>) -> Outcome {
    run(host, &Request::GetAssertion(get_assertion_request()), pin)
}

fn failed(error: SecurityKeyError, device_that_wants_pin: Option<u64>) -> Outcome {
    Outcome::Failed {
        error,
        device_that_wants_pin,
    }
}

fn user_names(outcome: &Outcome) -> Vec<&str> {
    let Outcome::Assertions(assertions) = outcome else {
        panic!("no assertions: {outcome:?}");
    };
    assertions
        .iter()
        .map(|assertion| assertion.user_name.as_str())
        .collect()
}

#[test]
fn makes_a_credential_with_the_key_that_the_user_touches() {
    let key = key_touched_at(100, Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &key);

    let Outcome::Attestation(attestation) = make_credential(&mut host, None) else {
        panic!("no attestation");
    };
    assert_eq!(attestation.credential_id, [0xc1, 0xc2, 0xc3]);
    assert_eq!(attestation.public_key_algorithm, -7);
    assert!(!attestation.is_resident_key);
    let public_key = attestation.public_key.unwrap();
    assert_eq!(public_key.len(), 91);
    assert_eq!(public_key[27..59], CREDENTIAL_PUBLIC_KEY_X);
    assert_eq!(public_key[59..], CREDENTIAL_PUBLIC_KEY_Y);
}

#[test]
fn takes_what_identifies_the_key_out_of_the_attestation() {
    let key = key_touched_at(100, Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &key);

    let Outcome::Attestation(attestation) = make_credential(&mut host, None) else {
        panic!("no attestation");
    };
    assert_eq!(attestation.authenticator_data[37..53], [0; 16]);
    let (attestation_object, _) = crate::cbor::decode(&attestation.attestation_object).unwrap();
    assert_eq!(attestation_object.get_text("fmt").unwrap().as_text(), Some("none"));
    assert_eq!(
        attestation_object.get_text("attStmt"),
        Some(&crate::cbor::Value::Map(Vec::new()))
    );
    assert_eq!(
        attestation_object.get_text("authData").unwrap().as_bytes(),
        Some(&attestation.authenticator_data[..])
    );
}

#[test]
fn makes_a_discoverable_credential_when_the_key_can() {
    let key = key_touched_at(100, Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &key);
    let request = Request::MakeCredential(MakeCredentialRequest {
        resident_key: ResidentKeyRequirement::Preferred,
        ..make_credential_request()
    });

    let Outcome::Attestation(attestation) = run(&mut host, &request, None) else {
        panic!("no attestation");
    };
    assert!(attestation.is_resident_key);
}

#[test]
fn a_key_without_storage_cannot_make_a_required_discoverable_credential() {
    let key = key_touched_at(
        100,
        Config {
            resident_key: false,
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    let request = Request::MakeCredential(MakeCredentialRequest {
        resident_key: ResidentKeyRequirement::Required,
        ..make_credential_request()
    });

    assert_eq!(
        run(&mut host, &request, None),
        failed(SecurityKeyError::NotSupported, None)
    );
    assert!(
        !key.borrow()
            .record
            .commands
            .contains(&ctap::AUTHENTICATOR_MAKE_CREDENTIAL)
    );
}

#[test]
fn the_other_keys_are_cancelled_when_the_user_touches_one() {
    let touched = key_touched_at(300, Config::default());
    let other = FakeKey::shared(Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect(2, &touched);

    assert!(matches!(make_credential(&mut host, None), Outcome::Attestation(_)));
    assert_eq!(other.borrow().record.cancel_count, 1);
    assert_eq!(touched.borrow().record.cancel_count, 0);
}

#[test]
fn waits_for_a_key_that_the_user_connects_later() {
    let key = key_touched_at(3000, Config::default());
    let mut host = FakeHost::new();
    host.connect_at(1, &key, 2000);

    assert!(matches!(make_credential(&mut host, None), Outcome::Attestation(_)));
}

#[test]
fn devices_that_are_no_ctap2_keys_get_no_request() {
    let without_cbor = FakeKey::shared(Config {
        supports_cbor: false,
        ..Config::default()
    });
    let u2f_only = FakeKey::shared(Config {
        versions: vec!["U2F_V2"],
        ..Config::default()
    });
    let silent = FakeKey::shared(Config {
        answers: false,
        ..Config::default()
    });
    let mut host = FakeHost::new();
    host.connect(1, &without_cbor);
    host.connect(2, &u2f_only);
    host.connect(3, &silent);
    host.cancel_at = Some(20_000);

    assert_eq!(make_credential(&mut host, None), Outcome::Cancelled);
    assert!(without_cbor.borrow().record.commands.is_empty());
    assert_eq!(u2f_only.borrow().record.commands, [ctap::AUTHENTICATOR_GET_INFO]);
    assert!(silent.borrow().record.commands.is_empty());
}

#[test]
fn cancel_reaches_the_keys_that_wait() {
    let key = FakeKey::shared(Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &key);
    host.cancel_at = Some(1000);

    assert_eq!(make_credential(&mut host, None), Outcome::Cancelled);
    assert_eq!(key.borrow().record.cancel_count, 1);
}

#[test]
fn the_only_key_with_a_pin_asks_for_it_without_a_touch() {
    let key = FakeKey::shared(key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(7, &key);

    assert_eq!(
        make_credential(&mut host, None),
        failed(SecurityKeyError::PinRequired, Some(7))
    );
    assert_eq!(key.borrow().record.commands, [ctap::AUTHENTICATOR_GET_INFO]);
}

#[test]
fn makes_a_credential_with_the_pin_of_the_key() {
    for (protocols, pin_uv_auth_token) in [(vec![1], false), (vec![1, 2], false), (vec![2], true)] {
        let key = key_touched_at(
            100,
            key_with_pin(Config {
                pin_uv_auth_protocols: protocols,
                pin_uv_auth_token,
                ..Config::default()
            }),
        );
        let mut host = FakeHost::new();
        host.connect(7, &key);

        let outcome = make_credential(&mut host, Some(&pin_for(7, PIN)));
        let Outcome::Attestation(attestation) = outcome else {
            panic!("no attestation: {outcome:?}");
        };
        // NB: The key says in the flags that it verified the user.
        assert_eq!(attestation.authenticator_data[32] & 0x04, 0x04);
        assert_eq!(key.borrow().record.pin_attempts, [true]);
    }
}

#[test]
fn a_wrong_pin_is_for_the_user_to_give_again() {
    let key = key_touched_at(100, key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(7, &key);

    assert_eq!(
        make_credential(&mut host, Some(&pin_for(7, b"0000"))),
        failed(SecurityKeyError::PinInvalid, Some(7))
    );
    assert_eq!(key.borrow().record.pin_attempts, [false]);
    assert!(!key.borrow().record.was_touched_for_a_request);
}

#[test]
fn a_key_that_takes_no_more_pins_says_so() {
    let key = FakeKey::shared(key_with_pin(Config {
        is_pin_blocked: true,
        ..Config::default()
    }));
    let mut host = FakeHost::new();
    host.connect(7, &key);

    assert_eq!(
        make_credential(&mut host, Some(&pin_for(7, PIN))),
        failed(SecurityKeyError::PinBlocked, None)
    );
}

#[test]
fn with_two_keys_a_touch_chooses_the_key_before_the_pin() {
    let touched = key_touched_at(400, key_with_pin(Config::default()));
    let other = FakeKey::shared(key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect(2, &touched);

    assert_eq!(
        make_credential(&mut host, None),
        failed(SecurityKeyError::PinRequired, Some(2))
    );
    assert_eq!(other.borrow().record.cancel_count, 1);
    for key in [&touched, &other] {
        assert!(key.borrow().record.pin_attempts.is_empty());
        assert!(!key.borrow().record.commands.contains(&ctap::AUTHENTICATOR_CLIENT_PIN));
    }
}

#[test]
fn the_pin_goes_only_to_the_key_that_it_is_for() {
    let chosen = key_touched_at(400, key_with_pin(Config::default()));
    let other = key_touched_at(100, key_with_pin(Config::default()));
    let without_pin = key_touched_at(100, Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect(2, &chosen);
    host.connect(3, &without_pin);

    assert!(matches!(
        make_credential(&mut host, Some(&pin_for(2, PIN))),
        Outcome::Attestation(_)
    ));
    assert_eq!(chosen.borrow().record.pin_attempts, [true]);
    // NB: A key with another PIN would count the PIN of the user as a wrong one.
    assert_eq!(other.borrow().record.commands, [ctap::AUTHENTICATOR_GET_INFO]);
}

#[test]
fn a_key_that_was_left_out_for_the_pin_of_another_gets_the_request_when_that_key_is_gone() {
    let key_of_the_pin = FakeKey::shared(key_with_pin(Config::default()));
    let other = key_touched_at(600, key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect_between(2, &key_of_the_pin, 0, Some(300));

    assert_eq!(
        make_credential(&mut host, Some(&pin_for(2, PIN))),
        failed(SecurityKeyError::PinRequired, Some(1))
    );
    assert!(other.borrow().record.pin_attempts.is_empty());
}

#[test]
fn a_wrong_pin_with_two_keys_ends_the_session_for_the_key_of_the_pin() {
    let chosen = FakeKey::shared(key_with_pin(Config::default()));
    let other = key_touched_at(100, Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect(2, &chosen);

    // NB: The other key could make the credential, but the user chose this one.
    assert_eq!(
        make_credential(&mut host, Some(&pin_for(2, b"0000"))),
        failed(SecurityKeyError::PinInvalid, Some(2))
    );
    assert!(!other.borrow().record.was_touched_for_a_request);

    let chosen = key_touched_at(100, key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect(2, &chosen);
    assert!(matches!(
        make_credential(&mut host, Some(&pin_for(2, PIN))),
        Outcome::Attestation(_)
    ));
}

#[test]
fn a_key_is_chosen_with_a_pin_protocol_that_it_has() {
    let touched = key_touched_at(
        400,
        key_with_pin(Config {
            pin_uv_auth_protocols: vec![2],
            ..Config::default()
        }),
    );
    let other = FakeKey::shared(key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect(2, &touched);

    assert_eq!(
        make_credential(&mut host, None),
        failed(SecurityKeyError::PinRequired, Some(2))
    );
    assert_eq!(touched.borrow().record.selection_protocols, [2]);
    assert_eq!(other.borrow().record.selection_protocols, [1]);
}

#[test]
fn a_pin_for_a_key_that_was_connected_again_is_asked_for_again() {
    // NB: The key that the user gave the PIN for was device 2. It is now device 3.
    let key = FakeKey::shared(key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(3, &key);

    assert_eq!(
        make_credential(&mut host, Some(&pin_for(2, PIN))),
        failed(SecurityKeyError::PinRequired, Some(3))
    );
    assert!(key.borrow().record.pin_attempts.is_empty());
}

#[test]
fn a_device_that_asks_for_a_pin_unasked_gets_it_only_after_a_touch() {
    let config = Config {
        pin: Some(PIN),
        make_credential_uv_not_required: true,
        asks_for_user_verification_unasked: true,
        ..Config::default()
    };

    // NB: Nobody touches the device, so it is not the key that the user chose.
    let untouched = FakeKey::shared(config.clone());
    let mut host = FakeHost::new();
    host.connect(1, &untouched);
    host.cancel_at = Some(5000);
    assert_eq!(make_credential(&mut host, None), Outcome::Cancelled);
    assert_eq!(untouched.borrow().record.selection_protocols, [1]);

    let touched = key_touched_at(400, config.clone());
    let mut host = FakeHost::new();
    host.connect(1, &touched);
    assert_eq!(
        make_credential(&mut host, None),
        failed(SecurityKeyError::PinRequired, Some(1))
    );

    let with_pin = key_touched_at(400, config);
    let mut host = FakeHost::new();
    host.connect(1, &with_pin);
    assert!(matches!(
        make_credential(&mut host, Some(&pin_for(1, PIN))),
        Outcome::Attestation(_)
    ));
}

#[test]
fn a_key_that_always_verifies_the_user_asks_for_its_pin() {
    let key = FakeKey::shared(key_with_pin(Config {
        make_credential_uv_not_required: true,
        always_uv: true,
        ..Config::default()
    }));
    let mut host = FakeHost::new();
    host.connect(1, &key);

    assert_eq!(
        make_credential(&mut host, None),
        failed(SecurityKeyError::PinRequired, Some(1))
    );
    assert_eq!(key.borrow().record.commands, [ctap::AUTHENTICATOR_GET_INFO]);
}

#[test]
fn a_key_that_fails_to_verify_the_user_by_itself_takes_its_pin() {
    let config = Config {
        user_verification: Some(true),
        pin: Some(PIN),
        is_built_in_user_verification_blocked: true,
        ..Config::default()
    };

    let key = key_touched_at(400, config.clone());
    let mut host = FakeHost::new();
    host.connect(1, &key);
    assert_eq!(
        make_credential(&mut host, None),
        failed(SecurityKeyError::PinRequired, Some(1))
    );

    let with_pin = key_touched_at(400, config);
    let mut host = FakeHost::new();
    host.connect(1, &with_pin);
    assert!(matches!(
        make_credential(&mut host, Some(&pin_for(1, PIN))),
        Outcome::Attestation(_)
    ));
}

#[test]
fn a_key_that_needs_a_user_verification_it_cannot_do_is_not_supported() {
    let key = FakeKey::shared(Config {
        can_take_pin: false,
        asks_for_user_verification_unasked: true,
        ..Config::default()
    });
    let mut host = FakeHost::new();
    host.connect(1, &key);

    assert_eq!(
        make_credential(&mut host, None),
        failed(SecurityKeyError::NotSupported, None)
    );
}

#[test]
fn a_key_with_a_credential_of_the_exclude_list_ends_the_session() {
    let request = Request::MakeCredential(MakeCredentialRequest {
        exclude_credential_ids: vec![vec![5; 4]],
        ..make_credential_request()
    });
    let holds_credential = Config {
        credentials: vec![credential(5, "five")],
        ..Config::default()
    };

    let key = key_touched_at(100, holds_credential.clone());
    let other = FakeKey::shared(Config::default());
    let mut host = FakeHost::new();
    host.connect(1, &key);
    host.connect(2, &other);
    assert_eq!(
        run(&mut host, &request, None),
        failed(SecurityKeyError::CredentialExcluded, None)
    );
    assert_eq!(other.borrow().record.cancel_count, 1);

    // NB: A key can look at the request before it looks at the PIN.
    let key = key_touched_at(400, key_with_pin(holds_credential));
    let other = FakeKey::shared(key_with_pin(Config::default()));
    let mut host = FakeHost::new();
    host.connect(1, &key);
    host.connect(2, &other);
    assert_eq!(
        run(&mut host, &request, None),
        failed(SecurityKeyError::CredentialExcluded, None)
    );
}

#[test]
fn gets_an_assertion_of_a_discoverable_credential() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five")],
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);

    let Outcome::Assertions(assertions) = get_assertion(&mut host, None) else {
        panic!("no assertions");
    };
    assert_eq!(assertions.len(), 1);
    assert_eq!(assertions[0].credential_id, [5; 4]);
    assert_eq!(assertions[0].user_handle.as_deref(), Some(&[5][..]));
    assert_eq!(assertions[0].user_name, "five");
    assert_eq!(assertions[0].signature, [0x30, 0x02, 0x01, 0x01]);
}

#[test]
fn gets_all_the_assertions_of_the_key_that_the_user_touches() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five"), credential(6, "six"), credential(7, "seven")],
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);

    assert_eq!(user_names(&get_assertion(&mut host, None)), ["five", "six", "seven"]);
}

#[test]
fn a_key_cannot_make_the_client_ask_for_assertions_without_end() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five")],
            number_of_credentials: Some(i64::MAX),
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);

    // NB: The key has no next assertion to give, and what it gave is not used.
    assert_eq!(get_assertion(&mut host, None), failed(SecurityKeyError::Failed, None));
}

#[test]
fn takes_the_credential_from_an_allow_list_of_one() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five")],
            leaves_credential_out: true,
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    let request = Request::GetAssertion(GetAssertionRequest {
        allow_credential_ids: vec![vec![5; 4]],
        ..get_assertion_request()
    });

    let Outcome::Assertions(assertions) = run(&mut host, &request, None) else {
        panic!("no assertions");
    };
    assert_eq!(assertions[0].credential_id, [5; 4]);
}

#[test]
fn a_key_without_credentials_leaves_the_session_to_the_others() {
    let empty = FakeKey::shared(Config::default());
    let with_credential = key_touched_at(
        300,
        Config {
            credentials: vec![credential(5, "five")],
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &empty);
    host.connect(2, &with_credential);
    assert_eq!(user_names(&get_assertion(&mut host, None)), ["five"]);

    let mut host = FakeHost::new();
    host.connect(1, &empty);
    assert_eq!(
        get_assertion(&mut host, None),
        failed(SecurityKeyError::NoCredentials, None)
    );
}

#[test]
fn an_assertion_for_another_relying_party_is_not_used() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five")],
            rp_id_of_authenticator_data: Some("evil.example"),
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    assert_eq!(get_assertion(&mut host, None), failed(SecurityKeyError::Failed, None));

    let mut host = FakeHost::new();
    host.connect(1, &key);
    assert_eq!(make_credential(&mut host, None), failed(SecurityKeyError::Failed, None));
}

#[test]
fn a_credential_without_a_user_is_not_used() {
    let key = key_touched_at(
        100,
        Config {
            claims_no_user: true,
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    assert_eq!(make_credential(&mut host, None), failed(SecurityKeyError::Failed, None));
}

#[test]
fn a_credential_whose_public_key_is_not_on_the_curve_is_not_used() {
    let key = key_touched_at(
        100,
        Config {
            makes_public_key_off_the_curve: true,
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    assert_eq!(make_credential(&mut host, None), failed(SecurityKeyError::Failed, None));
}

#[test]
fn an_assertion_without_a_user_is_not_used() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five")],
            claims_no_user: true,
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    assert_eq!(get_assertion(&mut host, None), failed(SecurityKeyError::Failed, None));
}

#[test]
fn verifies_the_user_with_the_pin_for_an_assertion() {
    let key = key_touched_at(
        100,
        key_with_pin(Config {
            credentials: vec![credential(5, "five")],
            ..Config::default()
        }),
    );
    let request = Request::GetAssertion(GetAssertionRequest {
        user_verification: UserVerificationRequirement::Preferred,
        ..get_assertion_request()
    });

    let mut host = FakeHost::new();
    host.connect(1, &key);
    assert_eq!(
        run(&mut host, &request, None),
        failed(SecurityKeyError::PinRequired, Some(1))
    );

    let mut host = FakeHost::new();
    host.connect(1, &key);
    let Outcome::Assertions(assertions) = run(&mut host, &request, Some(&pin_for(1, PIN))) else {
        panic!("no assertions");
    };
    assert_eq!(assertions[0].authenticator_data[32] & 0x04, 0x04);
}

#[test]
fn a_key_that_cannot_verify_the_user_gets_no_request_that_requires_it() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five")],
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    let request = Request::GetAssertion(GetAssertionRequest {
        user_verification: UserVerificationRequirement::Required,
        ..get_assertion_request()
    });

    assert_eq!(
        run(&mut host, &request, None),
        failed(SecurityKeyError::NotSupported, None)
    );
    assert!(
        !key.borrow()
            .record
            .commands
            .contains(&ctap::AUTHENTICATOR_GET_ASSERTION)
    );
}

#[test]
fn a_key_with_its_own_user_verification_needs_no_pin() {
    let key = key_touched_at(
        100,
        Config {
            user_verification: Some(true),
            credentials: vec![credential(5, "five")],
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);
    let request = Request::GetAssertion(GetAssertionRequest {
        user_verification: UserVerificationRequirement::Required,
        ..get_assertion_request()
    });

    let Outcome::Assertions(assertions) = run(&mut host, &request, None) else {
        panic!("no assertions");
    };
    assert_eq!(assertions[0].authenticator_data[32] & 0x04, 0x04);
}

#[test]
fn waits_on_when_a_key_stops_waiting_for_a_touch() {
    let key = key_touched_at(
        2000,
        Config {
            user_action_timeout_at: Some(1000),
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &key);

    assert!(matches!(make_credential(&mut host, None), Outcome::Attestation(_)));
    let make_credential_commands = key
        .borrow()
        .record
        .commands
        .iter()
        .filter(|command| **command == ctap::AUTHENTICATOR_MAKE_CREDENTIAL)
        .count();
    assert_eq!(make_credential_commands, 2);
}

#[test]
fn a_key_that_goes_away_can_come_back() {
    let key = key_touched_at(3000, Config::default());
    let mut host = FakeHost::new();
    host.connect_between(1, &key, 0, Some(1000));
    host.cancel_at = Some(5000);
    assert_eq!(make_credential(&mut host, None), Outcome::Cancelled);

    let key = key_touched_at(3000, Config::default());
    let mut host = FakeHost::new();
    host.connect_between(1, &key, 0, Some(1000));
    host.connect_at(2, &key, 2000);
    assert!(matches!(make_credential(&mut host, None), Outcome::Attestation(_)));
}

#[test]
fn the_session_fails_when_the_chosen_key_goes_away() {
    let key = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five"), credential(6, "six")],
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    // NB: The key goes away after its first assertion, before the client has the next.
    host.connect_until_command(1, &key, ctap::AUTHENTICATOR_GET_NEXT_ASSERTION);
    let outcome = get_assertion(&mut host, None);
    assert_eq!(outcome, failed(SecurityKeyError::Failed, None));
}

#[test]
fn the_key_of_the_pin_ends_the_session_when_it_has_no_credential() {
    let chosen = FakeKey::shared(key_with_pin(Config::default()));
    let other = key_touched_at(
        100,
        Config {
            credentials: vec![credential(5, "five")],
            ..Config::default()
        },
    );
    let mut host = FakeHost::new();
    host.connect(1, &other);
    host.connect(2, &chosen);
    let request = Request::GetAssertion(GetAssertionRequest {
        user_verification: UserVerificationRequirement::Preferred,
        ..get_assertion_request()
    });

    // NB: The user chose the key with a touch and gave its PIN, so the other key is not what they mean.
    assert_eq!(
        run(&mut host, &request, Some(&pin_for(2, PIN))),
        failed(SecurityKeyError::NoCredentials, None)
    );
    assert_eq!(other.borrow().record.cancel_count, 1);
    assert!(!other.borrow().record.was_touched_for_a_request);
}
