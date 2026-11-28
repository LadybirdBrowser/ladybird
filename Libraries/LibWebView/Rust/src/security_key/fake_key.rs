/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A security key for tests: the authenticator side of CTAPHID and CTAP2, as far as the client uses them, with a
//! record of what the client sent it. A test touches the key by the clock of the fake host.

use super::ctap::{self, client_pin, status};
use super::ctaphid;
use super::{DeviceError, HidDevice, Host, REPORT_SIZE, Report};
use crate::cbor::{self, Value};
use libcrypto_rust::{
    EcCurve, KeyPair, ShaHashKind, aes_cbc_cipher_without_padding, ec_compute_coordinate, ec_export_public_coordinates,
    ec_generate_key_pair, ec_key_from_coordinates, hkdf_derive_key, hmac_sign, sha_digest,
};
use std::cell::{Cell, RefCell};
use std::collections::{HashSet, VecDeque};
use std::rc::Rc;

/// The pinUvAuthToken that a fake key gave last.
struct Token {
    version: i64,
    token: Vec<u8>,
    /// The permissions and the Relying Party of a token with permissions.
    scope: Option<(i64, String)>,
}

/// The credential public key that a fake key makes, which is the base point of P-256, so that it is a point of the curve.
pub const CREDENTIAL_PUBLIC_KEY_X: [u8; 32] = [
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d,
    0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
];
pub const CREDENTIAL_PUBLIC_KEY_Y: [u8; 32] = [
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33,
    0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
];

#[derive(Clone)]
pub struct Credential {
    pub rp_id: &'static str,
    pub id: Vec<u8>,
    pub user_id: Vec<u8>,
    pub user_name: &'static str,
}

/// What the key is and how it behaves.
#[derive(Clone)]
pub struct Config {
    pub versions: Vec<&'static str>,
    pub supports_cbor: bool,
    /// Whether the device answers at all.
    pub answers: bool,
    pub resident_key: bool,
    pub user_verification: Option<bool>,
    pub make_credential_uv_not_required: bool,
    pub pin_uv_auth_token: bool,
    pub always_uv: bool,
    /// Whether the key asks for a user verification in its answer, though its info does not say that it will.
    pub asks_for_user_verification_unasked: bool,
    /// Whether the key says in its authenticator data that no user was present.
    pub claims_no_user: bool,
    /// Whether the key makes a credential public key that is not a point of the curve.
    pub makes_public_key_off_the_curve: bool,
    /// Whether the key fails to verify the user by itself.
    pub is_built_in_user_verification_blocked: bool,
    pub pin_uv_auth_protocols: Vec<i64>,
    /// The "clientPin" option is there when the key can take a PIN, and true when this is set.
    pub can_take_pin: bool,
    pub pin: Option<&'static [u8]>,
    pub is_pin_blocked: bool,
    pub credentials: Vec<Credential>,
    /// Whether the key leaves the credential out of an assertion when the allow list has one entry.
    pub leaves_credential_out: bool,
    /// The Relying Party that the key makes its authenticator data for, when not that of the request.
    pub rp_id_of_authenticator_data: Option<&'static str>,
    /// What the key says for numberOfCredentials, when not how many it has.
    pub number_of_credentials: Option<i64>,
    /// When the user touches the key, by the clock of the host.
    pub touch_at: Option<u64>,
    /// When the key stops waiting for a touch, once.
    pub user_action_timeout_at: Option<u64>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            versions: vec!["FIDO_2_0"],
            supports_cbor: true,
            answers: true,
            resident_key: true,
            user_verification: None,
            make_credential_uv_not_required: false,
            pin_uv_auth_token: false,
            always_uv: false,
            asks_for_user_verification_unasked: false,
            claims_no_user: false,
            makes_public_key_off_the_curve: false,
            is_built_in_user_verification_blocked: false,
            pin_uv_auth_protocols: vec![1],
            can_take_pin: true,
            pin: None,
            is_pin_blocked: false,
            credentials: Vec::new(),
            leaves_credential_out: false,
            rp_id_of_authenticator_data: None,
            number_of_credentials: None,
            touch_at: None,
            user_action_timeout_at: None,
        }
    }
}

/// What the key waits for a touch to answer.
enum Pending {
    /// A request with a pinUvAuthParam of zero length.
    Selection { is_excluded: bool },
    MakeCredential {
        rp_id: String,
        is_excluded: bool,
        is_user_verified: bool,
    },
    GetAssertion {
        rp_id: String,
        assertions: Vec<Credential>,
        is_user_verified: bool,
        has_one_allowed: bool,
    },
}

/// What the client did with the key.
#[derive(Default)]
pub struct Record {
    /// The authenticator API commands that the key got, in order.
    pub commands: Vec<u8>,
    /// Whether the PIN of each attempt was the right one.
    pub pin_attempts: Vec<bool>,
    /// The pinUvAuthProtocol of each request with a pinUvAuthParam of zero length.
    pub selection_protocols: Vec<i64>,
    pub cancel_count: usize,
    pub was_touched_for_a_request: bool,
}

pub struct FakeKey {
    pub config: Config,
    pub record: Record,
    key_agreement: KeyPair,
    token: Option<Token>,
    pending: Option<Pending>,
    next_assertions: VecDeque<(String, Credential, bool)>,
    channel_id: u32,
    incoming: Option<(u8, usize, Vec<u8>)>,
    outgoing: VecDeque<Report>,
    did_time_out: bool,
}

pub type SharedKey = Rc<RefCell<FakeKey>>;

fn sha256(bytes: &[u8]) -> Vec<u8> {
    sha_digest(ShaHashKind::Sha256, bytes).unwrap()
}

fn text_map(entries: Vec<(&str, Value)>) -> Value {
    Value::Map(
        entries
            .into_iter()
            .map(|(key, value)| (Value::from(key), value))
            .collect(),
    )
}

fn integer_map(entries: Vec<(i64, Value)>) -> Value {
    Value::Map(
        entries
            .into_iter()
            .map(|(key, value)| (Value::Integer(key), value))
            .collect(),
    )
}

impl FakeKey {
    pub fn shared(config: Config) -> SharedKey {
        Rc::new(RefCell::new(Self {
            config,
            record: Record::default(),
            key_agreement: ec_generate_key_pair(EcCurve::P256).unwrap(),
            token: None,
            pending: None,
            next_assertions: VecDeque::new(),
            channel_id: 0x0102_0304,
            incoming: None,
            outgoing: VecDeque::new(),
            did_time_out: false,
        }))
    }

    fn send(&mut self, channel_id: u32, command: u8, payload: &[u8]) {
        let (first, mut rest) = payload.split_at(payload.len().min(ctaphid::INITIALIZATION_PACKET_DATA_SIZE));
        let mut packet = [0; REPORT_SIZE];
        packet[..4].copy_from_slice(&channel_id.to_be_bytes());
        packet[4] = command | ctaphid::INITIALIZATION_PACKET_FLAG;
        packet[5..7].copy_from_slice(&(payload.len() as u16).to_be_bytes());
        packet[7..7 + first.len()].copy_from_slice(first);
        self.outgoing.push_back(packet);
        let mut sequence = 0;
        while !rest.is_empty() {
            let (chunk, remaining) = rest.split_at(rest.len().min(ctaphid::CONTINUATION_PACKET_DATA_SIZE));
            let mut packet = [0; REPORT_SIZE];
            packet[..4].copy_from_slice(&channel_id.to_be_bytes());
            packet[4] = sequence;
            packet[5..5 + chunk.len()].copy_from_slice(chunk);
            self.outgoing.push_back(packet);
            sequence += 1;
            rest = remaining;
        }
    }

    fn respond(&mut self, status: u8, value: Option<Value>) {
        let mut payload = vec![status];
        if let Some(value) = value {
            payload.extend_from_slice(&cbor::encode(&value));
        }
        self.send(self.channel_id, ctaphid::COMMAND_CBOR, &payload);
    }

    fn receive_report(&mut self, report: &Report) {
        if !self.config.answers {
            return;
        }
        let channel_id = u32::from_be_bytes([report[0], report[1], report[2], report[3]]);
        if report[4] & ctaphid::INITIALIZATION_PACKET_FLAG != 0 {
            let command = report[4] & !ctaphid::INITIALIZATION_PACKET_FLAG;
            let size = usize::from(u16::from_be_bytes([report[5], report[6]]));
            let data = report[7..7 + size.min(ctaphid::INITIALIZATION_PACKET_DATA_SIZE)].to_vec();
            self.incoming = Some((command, size, data));
        } else if let Some((_, size, data)) = &mut self.incoming {
            let remaining = *size - data.len();
            data.extend_from_slice(&report[5..5 + remaining.min(ctaphid::CONTINUATION_PACKET_DATA_SIZE)]);
        }
        if self
            .incoming
            .as_ref()
            .is_some_and(|(_, size, data)| data.len() >= *size)
        {
            let (command, _, payload) = self.incoming.take().unwrap();
            self.receive_message(channel_id, command, &payload);
        }
    }

    fn receive_message(&mut self, channel_id: u32, command: u8, payload: &[u8]) {
        match command {
            ctaphid::COMMAND_INIT => {
                // NB: The nonce, the new channel, and the versions of the protocol and the device.
                let mut answer = payload[..8].to_vec();
                answer.extend_from_slice(&self.channel_id.to_be_bytes());
                answer.extend_from_slice(&[2, 1, 0, 0]);
                answer.push(if self.config.supports_cbor {
                    ctaphid::CAPABILITY_CBOR
                } else {
                    0
                });
                self.send(channel_id, ctaphid::COMMAND_INIT, &answer);
            }
            ctaphid::COMMAND_CANCEL => {
                self.record.cancel_count += 1;
                if self.pending.take().is_some() {
                    self.respond(status::KEEP_ALIVE_CANCEL, None);
                }
            }
            ctaphid::COMMAND_CBOR => {
                let parameters = if payload.len() > 1 {
                    cbor::decode(&payload[1..]).unwrap().0
                } else {
                    Value::Null
                };
                self.record.commands.push(payload[0]);
                // NB: A new command takes the place of the one that the key was waiting on.
                self.pending = None;
                match payload[0] {
                    ctap::AUTHENTICATOR_GET_INFO => self.get_info(),
                    ctap::AUTHENTICATOR_CLIENT_PIN => self.client_pin(&parameters),
                    ctap::AUTHENTICATOR_MAKE_CREDENTIAL => self.make_credential(&parameters),
                    ctap::AUTHENTICATOR_GET_ASSERTION => self.get_assertion(&parameters),
                    ctap::AUTHENTICATOR_GET_NEXT_ASSERTION => self.get_next_assertion(),
                    _ => self.respond(0x01, None),
                }
            }
            _ => {}
        }
    }

    fn get_info(&mut self) {
        let mut options = vec![("rk", Value::Boolean(self.config.resident_key))];
        if let Some(user_verification) = self.config.user_verification {
            options.push(("uv", Value::Boolean(user_verification)));
        }
        if self.config.can_take_pin {
            options.push(("clientPin", Value::Boolean(self.config.pin.is_some())));
        }
        if self.config.make_credential_uv_not_required {
            options.push(("makeCredUvNotRqd", Value::Boolean(true)));
        }
        if self.config.pin_uv_auth_token {
            options.push(("pinUvAuthToken", Value::Boolean(true)));
        }
        if self.config.always_uv {
            options.push(("alwaysUv", Value::Boolean(true)));
        }
        let versions = self
            .config
            .versions
            .iter()
            .map(|version| Value::from(*version))
            .collect();
        let protocols = self
            .config
            .pin_uv_auth_protocols
            .iter()
            .map(|protocol| Value::Integer(*protocol))
            .collect();
        let info = integer_map(vec![
            (1, Value::Array(versions)),
            (3, Value::Bytes(vec![0x11; 16])),
            (4, text_map(options)),
            (6, Value::Array(protocols)),
        ]);
        self.respond(status::OK, Some(info));
    }

    /// The key that the client and the authenticator encrypt with, from the public key of the client.
    fn shared_aes_key(&self, version: i64, platform_key_agreement: &Value) -> Vec<u8> {
        let x = platform_key_agreement.get(-2).and_then(Value::as_bytes).unwrap();
        let y = platform_key_agreement.get(-3).and_then(Value::as_bytes).unwrap();
        let platform_key = ec_key_from_coordinates(x, y, None).unwrap();
        let shared_point_x = ec_compute_coordinate(&self.key_agreement.private_key, &platform_key).unwrap();
        if version == 1 {
            sha256(&shared_point_x)
        } else {
            hkdf_derive_key(ShaHashKind::Sha256, &[0; 32], &shared_point_x, b"CTAP2 AES key", 32)
                .unwrap()
                .into_vec()
        }
    }

    fn client_pin(&mut self, parameters: &Value) {
        let version = parameters.get(1).and_then(Value::as_integer).unwrap();
        if !self.config.pin_uv_auth_protocols.contains(&version) {
            self.respond(status::CTAP1_ERR_INVALID_PARAMETER, None);
            return;
        }
        let subcommand = parameters.get(2).and_then(Value::as_integer).unwrap();
        if subcommand == client_pin::GET_KEY_AGREEMENT {
            let (x, y) = ec_export_public_coordinates(&self.key_agreement.public_key).unwrap();
            let key = integer_map(vec![
                (1, Value::Integer(2)),
                (3, Value::Integer(-25)),
                (-1, Value::Integer(1)),
                (-2, Value::Bytes(x)),
                (-3, Value::Bytes(y)),
            ]);
            self.respond(status::OK, Some(integer_map(vec![(1, key)])));
            return;
        }

        let with_permissions = subcommand == client_pin::GET_PIN_UV_AUTH_TOKEN_USING_PIN_WITH_PERMISSIONS;
        assert!(with_permissions || subcommand == client_pin::GET_PIN_TOKEN);
        // NB: A key gives tokens with permissions only when it says that it does, and wants an rpId for them.
        assert_eq!(with_permissions, self.config.pin_uv_auth_token);
        assert_eq!(
            with_permissions,
            parameters.get(9).is_some() && parameters.get(10).is_some()
        );

        if self.config.is_pin_blocked {
            self.respond(status::PIN_BLOCKED, None);
            return;
        }
        let aes_key = self.shared_aes_key(version, parameters.get(3).unwrap());
        let pin_hash_enc = parameters.get(6).and_then(Value::as_bytes).unwrap();
        let (initialization_vector, ciphertext) = if version == 1 {
            (&[0; 16][..], pin_hash_enc)
        } else {
            pin_hash_enc.split_at(16)
        };
        let pin_hash = aes_cbc_cipher_without_padding(false, &aes_key, ciphertext, initialization_vector).unwrap();
        let is_right_pin = self.config.pin.is_some_and(|pin| sha256(pin)[..16] == pin_hash);
        self.record.pin_attempts.push(is_right_pin);
        if !is_right_pin {
            self.respond(status::PIN_INVALID, None);
            return;
        }

        let token = sha256(&[self.record.pin_attempts.len() as u8, 0x70]);
        let mut encrypted_token = Vec::new();
        let initialization_vector = if version == 1 {
            [0; 16]
        } else {
            encrypted_token.extend_from_slice(&[0x5a; 16]);
            [0x5a; 16]
        };
        encrypted_token.extend_from_slice(
            &aes_cbc_cipher_without_padding(true, &aes_key, &token, &initialization_vector).unwrap(),
        );
        // NB: A token with permissions is for the one operation and the one Relying Party that it was asked for.
        let scope = with_permissions.then(|| {
            (
                parameters.get(9).and_then(Value::as_integer).unwrap(),
                parameters.get(10).and_then(Value::as_text).unwrap().to_owned(),
            )
        });
        self.token = Some(Token { version, token, scope });
        self.respond(status::OK, Some(integer_map(vec![(2, Value::Bytes(encrypted_token))])));
    }

    /// What the pinUvAuthParam of a request says: that the request is one to choose the key with, that the user is
    /// verified, that they are not, or the status that refuses the request.
    fn check_pin_uv_auth(
        &mut self,
        parameters: &Value,
        first_key: i64,
        client_data_hash: &[u8],
        permission: i64,
        rp_id: &str,
    ) -> Result<Option<bool>, u8> {
        let Some(pin_uv_auth_param) = parameters.get(first_key).and_then(Value::as_bytes) else {
            return Ok(Some(false));
        };
        let version = parameters.get(first_key + 1).and_then(Value::as_integer).unwrap();
        if !self.config.pin_uv_auth_protocols.contains(&version) {
            return Err(status::CTAP1_ERR_INVALID_PARAMETER);
        }
        if pin_uv_auth_param.is_empty() {
            self.record.selection_protocols.push(version);
            return Ok(None);
        }
        let Some(token) = &self.token else {
            return Err(status::PIN_AUTH_INVALID);
        };
        let mut expected = hmac_sign(ShaHashKind::Sha256, &token.token, client_data_hash).unwrap();
        if version == 1 {
            expected.truncate(16);
        }
        if token.version != version || expected != pin_uv_auth_param {
            return Err(status::PIN_AUTH_INVALID);
        }
        if let Some((permissions, token_rp_id)) = &token.scope
            && (permissions & permission == 0 || token_rp_id != rp_id)
        {
            return Err(status::PIN_AUTH_INVALID);
        }
        Ok(Some(true))
    }

    fn asked_for_built_in_user_verification(options: Option<&Value>) -> bool {
        options
            .and_then(|options| options.get_text("uv"))
            .and_then(Value::as_boolean)
            .unwrap_or(false)
    }

    fn make_credential(&mut self, parameters: &Value) {
        let client_data_hash = parameters.get(1).and_then(Value::as_bytes).unwrap().to_vec();
        let rp_id = parameters
            .get(2)
            .and_then(|rp| rp.get_text("id"))
            .and_then(Value::as_text)
            .unwrap()
            .to_owned();
        let is_excluded = parameters
            .get(5)
            .and_then(Value::as_array)
            .unwrap_or_default()
            .iter()
            .filter_map(|descriptor| descriptor.get_text("id").and_then(Value::as_bytes))
            .any(|id| {
                self.config
                    .credentials
                    .iter()
                    .any(|credential| credential.id == id && credential.rp_id == rp_id)
            });
        let options = parameters.get(7);
        let is_resident_key = options
            .and_then(|options| options.get_text("rk"))
            .and_then(Value::as_boolean)
            .unwrap_or(false);

        if self.config.is_built_in_user_verification_blocked && Self::asked_for_built_in_user_verification(options) {
            return self.respond(status::UV_BLOCKED, None);
        }
        let is_user_verified = match self.check_pin_uv_auth(parameters, 8, &client_data_hash, 0x01, &rp_id) {
            Err(status) => return self.respond(status, None),
            Ok(None) => {
                self.pending = Some(Pending::Selection { is_excluded });
                return;
            }
            Ok(Some(is_user_verified)) => is_user_verified || Self::asked_for_built_in_user_verification(options),
        };
        let may_skip_user_verification = self.config.make_credential_uv_not_required && !is_resident_key;
        let needs_user_verification = self.config.always_uv
            || self.config.asks_for_user_verification_unasked
            || ((self.config.pin.is_some() || self.config.user_verification == Some(true))
                && !may_skip_user_verification);
        if needs_user_verification && !is_user_verified {
            return self.respond(status::PUAT_REQUIRED, None);
        }
        self.pending = Some(Pending::MakeCredential {
            rp_id,
            is_excluded,
            is_user_verified,
        });
    }

    fn get_assertion(&mut self, parameters: &Value) {
        let rp_id = parameters.get(1).and_then(Value::as_text).unwrap().to_owned();
        let client_data_hash = parameters.get(2).and_then(Value::as_bytes).unwrap().to_vec();
        let allow_list: Option<Vec<&[u8]>> = parameters.get(3).and_then(Value::as_array).map(|descriptors| {
            descriptors
                .iter()
                .filter_map(|descriptor| descriptor.get_text("id").and_then(Value::as_bytes))
                .collect()
        });

        if self.config.is_built_in_user_verification_blocked
            && Self::asked_for_built_in_user_verification(parameters.get(5))
        {
            return self.respond(status::UV_BLOCKED, None);
        }
        let is_user_verified = match self.check_pin_uv_auth(parameters, 6, &client_data_hash, 0x02, &rp_id) {
            Err(status) => return self.respond(status, None),
            Ok(None) => {
                self.pending = Some(Pending::Selection { is_excluded: false });
                return;
            }
            Ok(Some(is_user_verified)) => {
                is_user_verified || Self::asked_for_built_in_user_verification(parameters.get(5))
            }
        };
        if (self.config.always_uv || self.config.asks_for_user_verification_unasked) && !is_user_verified {
            return self.respond(status::PUAT_REQUIRED, None);
        }

        let assertions: Vec<Credential> = self
            .config
            .credentials
            .iter()
            .filter(|credential| credential.rp_id == rp_id)
            .filter(|credential| {
                allow_list
                    .as_ref()
                    .is_none_or(|allow_list| allow_list.contains(&credential.id.as_slice()))
            })
            .cloned()
            .collect();
        if assertions.is_empty() {
            return self.respond(status::NO_CREDENTIALS, None);
        }
        self.pending = Some(Pending::GetAssertion {
            rp_id,
            assertions,
            is_user_verified,
            has_one_allowed: allow_list.is_some_and(|allow_list| allow_list.len() == 1),
        });
    }

    fn authenticator_data(&self, rp_id: &str, is_user_verified: bool, attested: Option<&[u8]>) -> Vec<u8> {
        let rp_id = self.config.rp_id_of_authenticator_data.unwrap_or(rp_id);
        let mut data = sha256(rp_id.as_bytes());
        // NB: The flags UP (bit 0), UV (bit 2) and AT (bit 6), then the signature counter.
        let mut flags = if self.config.claims_no_user { 0 } else { 0x01 };
        if is_user_verified {
            flags |= 0x04;
        }
        if attested.is_some() {
            flags |= ctap::AUTHENTICATOR_DATA_FLAG_ATTESTED_CREDENTIAL_DATA_INCLUDED;
        }
        data.push(flags);
        data.extend_from_slice(&[0, 0, 0, 7]);
        if let Some(credential_id) = attested {
            // NB: The aaguid, the credential ID with its length, and the credential public key.
            data.extend_from_slice(&[0x11; 16]);
            data.extend_from_slice(&(credential_id.len() as u16).to_be_bytes());
            data.extend_from_slice(credential_id);
            data.extend_from_slice(&cbor::encode(&integer_map(vec![
                (1, Value::Integer(2)),
                (3, Value::Integer(ctap::COSE_ALGORITHM_ES256)),
                (-1, Value::Integer(1)),
                (-2, Value::Bytes(CREDENTIAL_PUBLIC_KEY_X.to_vec())),
                (
                    -3,
                    Value::Bytes(if self.config.makes_public_key_off_the_curve {
                        vec![0xbb; 32]
                    } else {
                        CREDENTIAL_PUBLIC_KEY_Y.to_vec()
                    }),
                ),
            ])));
        }
        data
    }

    fn assertion(&self, rp_id: &str, credential: &Credential, is_user_verified: bool, count: Option<i64>) -> Value {
        let mut entries = Vec::new();
        if !self.config.leaves_credential_out {
            entries.push((
                1,
                text_map(vec![
                    ("id", Value::Bytes(credential.id.clone())),
                    ("type", Value::from("public-key")),
                ]),
            ));
        }
        entries.push((2, Value::Bytes(self.authenticator_data(rp_id, is_user_verified, None))));
        entries.push((3, Value::Bytes(vec![0x30, 0x02, 0x01, 0x01])));
        entries.push((
            4,
            text_map(vec![
                ("id", Value::Bytes(credential.user_id.clone())),
                ("name", Value::from(credential.user_name)),
            ]),
        ));
        if let Some(count) = count {
            entries.push((5, Value::Integer(count)));
        }
        integer_map(entries)
    }

    fn get_next_assertion(&mut self) {
        let Some((rp_id, credential, is_user_verified)) = self.next_assertions.pop_front() else {
            return self.respond(0x30, None);
        };
        let assertion = self.assertion(&rp_id, &credential, is_user_verified, None);
        self.respond(status::OK, Some(assertion));
    }

    /// The user touches the key.
    fn touch(&mut self) {
        let Some(pending) = self.pending.take() else {
            return;
        };
        match pending {
            Pending::Selection { is_excluded: true } => self.respond(status::CREDENTIAL_EXCLUDED, None),
            Pending::Selection { .. } if self.config.is_pin_blocked => self.respond(status::PIN_BLOCKED, None),
            Pending::Selection { .. } if self.config.pin.is_some() => self.respond(status::PIN_INVALID, None),
            Pending::Selection { .. } => self.respond(status::PIN_NOT_SET, None),
            Pending::MakeCredential { is_excluded: true, .. } => {
                self.record.was_touched_for_a_request = true;
                self.respond(status::CREDENTIAL_EXCLUDED, None);
            }
            Pending::MakeCredential {
                rp_id,
                is_user_verified,
                ..
            } => {
                self.record.was_touched_for_a_request = true;
                let authenticator_data = self.authenticator_data(&rp_id, is_user_verified, Some(&[0xc1, 0xc2, 0xc3]));
                let attestation = integer_map(vec![
                    (1, Value::from("packed")),
                    (2, Value::Bytes(authenticator_data)),
                    (3, text_map(vec![("sig", Value::Bytes(vec![1, 2, 3]))])),
                ]);
                self.respond(status::OK, Some(attestation));
            }
            Pending::GetAssertion {
                rp_id,
                mut assertions,
                is_user_verified,
                has_one_allowed,
            } => {
                self.record.was_touched_for_a_request = true;
                assert!(!self.config.leaves_credential_out || has_one_allowed);
                let first = assertions.remove(0);
                let count = self
                    .config
                    .number_of_credentials
                    .or((!assertions.is_empty()).then_some(assertions.len() as i64 + 1));
                let assertion = self.assertion(&rp_id, &first, is_user_verified, count);
                self.next_assertions = assertions
                    .into_iter()
                    .map(|credential| (rp_id.clone(), credential, is_user_verified))
                    .collect();
                self.respond(status::OK, Some(assertion));
            }
        }
    }

    /// What the key does by itself as time passes.
    fn pass_time(&mut self, now: u64) {
        if self.pending.is_none() {
            return;
        }
        if !self.did_time_out && self.config.user_action_timeout_at.is_some_and(|at| now >= at) {
            self.did_time_out = true;
            self.pending = None;
            self.respond(status::USER_ACTION_TIMEOUT, None);
            return;
        }
        if self.config.touch_at.is_some_and(|at| now >= at) {
            self.touch();
        }
    }
}

struct FakeDevice {
    id: u64,
    key: SharedKey,
    clock: Rc<Cell<u64>>,
    disconnect_at: Option<u64>,
    disconnect_after_command: Option<u8>,
}

impl FakeDevice {
    fn is_gone(&self) -> bool {
        self.disconnect_at.is_some_and(|at| self.clock.get() >= at)
            || self
                .disconnect_after_command
                .is_some_and(|command| self.key.borrow().record.commands.contains(&command))
    }
}

impl HidDevice for FakeDevice {
    fn id(&self) -> u64 {
        self.id
    }

    fn write_report(&mut self, report: &Report) -> Result<(), DeviceError> {
        if self.is_gone() {
            return Err(DeviceError);
        }
        self.key.borrow_mut().receive_report(report);
        Ok(())
    }

    fn read_report(&mut self, timeout_in_milliseconds: u32) -> Result<Option<Report>, DeviceError> {
        if self.is_gone() {
            return Err(DeviceError);
        }
        let mut key = self.key.borrow_mut();
        key.pass_time(self.clock.get());
        if let Some(report) = key.outgoing.pop_front() {
            // NB: A report takes its time to cross the wire.
            self.clock.set(self.clock.get() + 1);
            return Ok(Some(report));
        }
        self.clock.set(self.clock.get() + u64::from(timeout_in_milliseconds));
        Ok(None)
    }
}

/// A device as the machine has it: a key that is connected from some time on, under an id.
struct Connection {
    id: u64,
    key: SharedKey,
    connect_at: u64,
    /// When the user takes the key out.
    disconnect_at: Option<u64>,
    /// The authenticator API command that the key gets last, as the user takes the key out before it answers.
    disconnect_after_command: Option<u8>,
}

/// A machine with fake keys and a clock that moves only when the session waits.
pub struct FakeHost {
    pub clock: Rc<Cell<u64>>,
    connections: Vec<Connection>,
    /// When whoever started the session takes it back.
    pub cancel_at: Option<u64>,
    random_byte: u8,
}

/// A session that still runs at this time of the fake clock hangs.
pub const TIME_LIMIT: u64 = 120_000;

impl FakeHost {
    pub fn new() -> Self {
        Self {
            clock: Rc::new(Cell::new(0)),
            connections: Vec::new(),
            cancel_at: None,
            random_byte: 0,
        }
    }

    pub fn connect(&mut self, id: u64, key: &SharedKey) {
        self.connect_at(id, key, 0);
    }

    pub fn connect_at(&mut self, id: u64, key: &SharedKey, connect_at: u64) {
        self.connect_between(id, key, connect_at, None);
    }

    pub fn connect_between(&mut self, id: u64, key: &SharedKey, connect_at: u64, disconnect_at: Option<u64>) {
        self.connections.push(Connection {
            id,
            key: key.clone(),
            connect_at,
            disconnect_at,
            disconnect_after_command: None,
        });
    }

    pub fn connect_until_command(&mut self, id: u64, key: &SharedKey, command: u8) {
        self.connect(id, key);
        self.connections.last_mut().unwrap().disconnect_after_command = Some(command);
    }
}

impl Host for FakeHost {
    fn open_new_devices(&mut self, known_device_ids: &HashSet<u64>) -> Vec<Box<dyn HidDevice>> {
        let now = self.clock.get();
        self.connections
            .iter()
            .filter(|connection| connection.connect_at <= now && !known_device_ids.contains(&connection.id))
            .filter(|connection| connection.disconnect_at.is_none_or(|at| now < at))
            .map(|connection| {
                Box::new(FakeDevice {
                    id: connection.id,
                    key: connection.key.clone(),
                    clock: self.clock.clone(),
                    disconnect_at: connection.disconnect_at,
                    disconnect_after_command: connection.disconnect_after_command,
                }) as Box<dyn HidDevice>
            })
            .collect()
    }

    fn now_in_milliseconds(&mut self) -> u64 {
        self.clock.get()
    }

    fn sleep(&mut self, milliseconds: u32) {
        self.clock.set(self.clock.get() + u64::from(milliseconds));
    }

    fn is_cancelled(&mut self) -> bool {
        let now = self.clock.get();
        assert!(now < TIME_LIMIT, "the session hangs");
        self.cancel_at.is_some_and(|at| now >= at)
    }

    fn fill_with_random(&mut self, bytes: &mut [u8]) {
        for byte in bytes {
            self.random_byte = self.random_byte.wrapping_mul(31).wrapping_add(17);
            *byte = self.random_byte;
        }
    }
}
