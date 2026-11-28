/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The commands of the authenticator API that the client sends, and what it makes of the responses.

use super::ctaphid::{self, Channel, ChannelError};
use super::pin_protocol::{self, SharedSecret, Version};
use super::{Host, Secret};
use crate::cbor::{self, Value};
use libcrypto_rust::{
    OkpCurve, ShaHashKind, ec_export_der, ec_key_from_coordinates, okp_export_der, okp_key_from_raw, rsa_export_der,
    rsa_key_from_components, sha_digest,
};

pub const AUTHENTICATOR_MAKE_CREDENTIAL: u8 = 0x01;
pub const AUTHENTICATOR_GET_ASSERTION: u8 = 0x02;
pub const AUTHENTICATOR_GET_INFO: u8 = 0x04;
pub const AUTHENTICATOR_CLIENT_PIN: u8 = 0x06;
pub const AUTHENTICATOR_GET_NEXT_ASSERTION: u8 = 0x08;

/// The status codes of the authenticator API that the client acts on.
pub mod status {
    pub const OK: u8 = 0x00;
    pub const CTAP1_ERR_INVALID_PARAMETER: u8 = 0x02;
    pub const CREDENTIAL_EXCLUDED: u8 = 0x19;
    pub const KEEP_ALIVE_CANCEL: u8 = 0x2d;
    pub const NO_CREDENTIALS: u8 = 0x2e;
    pub const USER_ACTION_TIMEOUT: u8 = 0x2f;
    pub const PIN_INVALID: u8 = 0x31;
    pub const PIN_BLOCKED: u8 = 0x32;
    pub const PIN_AUTH_INVALID: u8 = 0x33;
    pub const PIN_AUTH_BLOCKED: u8 = 0x34;
    pub const PIN_NOT_SET: u8 = 0x35;
    pub const PUAT_REQUIRED: u8 = 0x36;
    pub const UV_BLOCKED: u8 = 0x3c;
    pub const UV_INVALID: u8 = 0x3f;
}

/// The subcommands of authenticatorClientPIN.
pub mod client_pin {
    pub const GET_KEY_AGREEMENT: i64 = 0x02;
    pub const GET_PIN_TOKEN: i64 = 0x05;
    pub const GET_PIN_UV_AUTH_TOKEN_USING_PIN_WITH_PERMISSIONS: i64 = 0x09;
}

/// <https://w3c.github.io/webauthn/#authdata-flags>
pub const AUTHENTICATOR_DATA_FLAG_USER_PRESENT: u8 = 1 << 0;
pub const AUTHENTICATOR_DATA_FLAG_USER_VERIFIED: u8 = 1 << 2;
pub const AUTHENTICATOR_DATA_FLAG_ATTESTED_CREDENTIAL_DATA_INCLUDED: u8 = 1 << 6;
pub const AUTHENTICATOR_DATA_FLAG_EXTENSION_DATA_INCLUDED: u8 = 1 << 7;
/// <https://www.iana.org/assignments/cose/cose.xhtml#algorithms>
pub const COSE_ALGORITHM_ES256: i64 = -7;
pub const COSE_ALGORITHM_EDDSA: i64 = -8;
pub const COSE_ALGORITHM_RS256: i64 = -257;

/// A key has only so many credentials for one Relying Party; this bounds what it can make the client ask for.
pub const MAXIMUM_ASSERTIONS_OF_ONE_KEY: i64 = 64;

/// Why a security key gave no credential.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SecurityKeyError {
    /// The key holds a credential of the exclude list.
    CredentialExcluded,
    /// The key has no credential for the request.
    NoCredentials,
    /// The key verifies the user with a PIN, which the client must ask for.
    PinRequired,
    /// The key did not take the PIN.
    PinInvalid,
    /// The key takes no more PINs, until the user connects it again or resets it.
    PinBlocked,
    /// The key cannot do what the request needs.
    NotSupported,
    Failed,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResidentKeyRequirement {
    Discouraged,
    Preferred,
    Required,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UserVerificationRequirement {
    Required,
    Preferred,
    Discouraged,
}

pub struct MakeCredentialRequest {
    pub client_data_hash: Vec<u8>,
    pub rp_id: String,
    pub rp_name: String,
    pub user_id: Vec<u8>,
    pub user_name: String,
    pub user_display_name: String,
    pub public_key_algorithms: Vec<i32>,
    pub exclude_credential_ids: Vec<Vec<u8>>,
    pub resident_key: ResidentKeyRequirement,
    pub user_verification: UserVerificationRequirement,
}

pub struct GetAssertionRequest {
    pub client_data_hash: Vec<u8>,
    pub rp_id: String,
    pub allow_credential_ids: Vec<Vec<u8>>,
    pub user_verification: UserVerificationRequirement,
}

pub enum Request {
    MakeCredential(MakeCredentialRequest),
    GetAssertion(GetAssertionRequest),
}

#[derive(Debug, PartialEq, Eq)]
pub struct Attestation {
    pub credential_id: Vec<u8>,
    pub authenticator_data: Vec<u8>,
    /// <https://w3c.github.io/webauthn/#attestation-object>
    pub attestation_object: Vec<u8>,
    /// The DER SubjectPublicKeyInfo of the credential public key, when the client knows its algorithm.
    pub public_key: Option<Vec<u8>>,
    pub public_key_algorithm: i32,
    pub is_resident_key: bool,
}

#[derive(Debug, PartialEq, Eq)]
pub struct Assertion {
    pub credential_id: Vec<u8>,
    pub authenticator_data: Vec<u8>,
    pub signature: Vec<u8>,
    pub user_handle: Option<Vec<u8>>,
    pub user_name: String,
    pub user_display_name: String,
}

pub struct Response {
    pub status: u8,
    /// Null when the response has no data.
    pub value: Value,
}

pub fn send_command(channel: &mut Channel, command: u8, parameters: Option<&Value>) -> Result<(), ChannelError> {
    let mut payload = vec![command];
    if let Some(parameters) = parameters {
        payload.extend_from_slice(&cbor::encode(parameters));
    }
    channel.send(ctaphid::COMMAND_CBOR, &payload)
}

/// The response in a message of the device, or nothing when the message is no CTAP2 response.
pub fn parse_response(message: &ctaphid::Message) -> Option<Response> {
    if message.command != ctaphid::COMMAND_CBOR {
        return None;
    }
    let (status, data) = message.payload.split_first()?;
    let value = if *status == status::OK && !data.is_empty() {
        // NB: The response is one item, with nothing after it.
        let (value, size) = cbor::decode(data).ok()?;
        if size != data.len() {
            return None;
        }
        value
    } else {
        Value::Null
    };
    Some(Response { status: *status, value })
}

/// Sends the command and waits for its response. This is for the commands that the key answers without the user.
pub fn transact(
    channel: &mut Channel,
    host: &mut dyn Host,
    command: u8,
    parameters: Option<&Value>,
) -> Result<Response, ChannelError> {
    send_command(channel, command, parameters)?;
    // NB: A poll can end early, such as for a report of another channel, so this goes by the clock.
    let deadline = host.now_in_milliseconds() + ctaphid::DEVICE_ANSWER_TIMEOUT_IN_MILLISECONDS;
    while host.now_in_milliseconds() < deadline {
        if let Some(message) = channel.poll(ctaphid::POLL_INTERVAL_IN_MILLISECONDS)? {
            return parse_response(&message).ok_or(ChannelError::Device);
        }
    }
    Err(ChannelError::NoAnswer)
}

/// How long a key has for an answer that it can ask the user for on its own display.
const USER_CONSENT_TIMEOUT_IN_MILLISECONDS: u64 = 30_000;

/// Sends a command that the key can ask the user about, and waits for the response for as long as the user can take.
/// A ceremony that ends in that time takes the command back.
fn transact_with_user(
    channel: &mut Channel,
    host: &mut dyn Host,
    command: u8,
    parameters: Option<&Value>,
) -> Result<Response, ChannelError> {
    send_command(channel, command, parameters)?;
    let deadline = host.now_in_milliseconds() + USER_CONSENT_TIMEOUT_IN_MILLISECONDS;
    while host.now_in_milliseconds() < deadline {
        let message = channel.poll(ctaphid::POLL_INTERVAL_IN_MILLISECONDS)?;
        // NB: A ceremony that ended makes no use of an answer that came at the same time.
        if host.is_cancelled() {
            break;
        }
        if let Some(message) = message {
            return parse_response(&message).ok_or(ChannelError::Device);
        }
    }
    channel.cancel();
    Err(ChannelError::NoAnswer)
}

/// What authenticatorGetInfo says about a key, as far as the client uses it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct KeyInfo {
    pub is_ctap2: bool,
    /// The "rk" option: Whether the key can store discoverable credentials.
    pub has_resident_key: bool,
    /// The "uv" option: Present when the key can verify the user by itself, and true when that is set up.
    pub user_verification: Option<bool>,
    /// The "clientPin" option: Present when the key can take a PIN, and true when one is set.
    pub client_pin: Option<bool>,
    /// The "makeCredUvNotRqd" option: Whether the key makes a non-discoverable credential without user verification.
    pub make_credential_uv_not_required: bool,
    /// The "pinUvAuthToken" option: Whether the key gives tokens with permissions.
    pub has_pin_uv_auth_token: bool,
    /// The "alwaysUv" option: Whether the key verifies the user for every credential that it makes or uses.
    pub always_uv: bool,
    pub pin_uv_auth_protocols: Vec<i64>,
}

impl KeyInfo {
    pub fn parse(response: &Value) -> Self {
        // NB: The response has versions (1), options (4) and pinUvAuthProtocols (6), among others.
        let mut info = Self::default();
        if let Some(versions) = response.get(1).and_then(Value::as_array) {
            info.is_ctap2 = versions
                .iter()
                .filter_map(Value::as_text)
                .any(|version| version.starts_with("FIDO_2_"));
        }
        if let Some(options) = response.get(4) {
            let option = |name| options.get_text(name).and_then(Value::as_boolean);
            info.has_resident_key = option("rk").unwrap_or(false);
            info.user_verification = option("uv");
            info.client_pin = option("clientPin");
            info.make_credential_uv_not_required = option("makeCredUvNotRqd").unwrap_or(false);
            info.has_pin_uv_auth_token = option("pinUvAuthToken").unwrap_or(false);
            info.always_uv = option("alwaysUv").unwrap_or(false);
        }
        if let Some(protocols) = response.get(6).and_then(Value::as_array) {
            info.pin_uv_auth_protocols = protocols.iter().filter_map(Value::as_integer).collect();
        }
        info
    }

    /// The PIN/UV auth protocol to use with the key, of those that it has.
    pub fn pin_uv_auth_protocol(&self) -> Version {
        // NB: A key that does not say which protocols it has is from before protocol two.
        if self.pin_uv_auth_protocols.contains(&2) {
            Version::Two
        } else {
            Version::One
        }
    }
}

pub fn get_info(channel: &mut Channel, host: &mut dyn Host) -> Result<KeyInfo, ChannelError> {
    let response = transact(channel, host, AUTHENTICATOR_GET_INFO, None)?;
    if response.status != status::OK {
        return Err(ChannelError::Device);
    }
    Ok(KeyInfo::parse(&response.value))
}

pub struct PinUvAuthToken {
    pub version: Version,
    pub token: Secret,
}

/// Gets a pinUvAuthToken from the key for the PIN of the user.
pub fn get_pin_uv_auth_token(
    channel: &mut Channel,
    host: &mut dyn Host,
    info: &KeyInfo,
    pin: &[u8],
    request: &Request,
) -> Result<PinUvAuthToken, SecurityKeyError> {
    let version = info.pin_uv_auth_protocol();

    // NB: The parameters are pinUvAuthProtocol (1), subCommand (2), keyAgreement (3), pinHashEnc (6),
    //     permissions (9) and rpId (10). The response has keyAgreement (1) and pinUvAuthToken (2).
    let parameters = Value::Map(vec![
        (Value::Integer(1), Value::Integer(version.number())),
        (Value::Integer(2), Value::Integer(client_pin::GET_KEY_AGREEMENT)),
    ]);
    let response =
        transact(channel, host, AUTHENTICATOR_CLIENT_PIN, Some(&parameters)).map_err(|_| SecurityKeyError::Failed)?;
    if response.status != status::OK {
        return Err(SecurityKeyError::Failed);
    }
    let authenticator_key_agreement = response.value.get(1).ok_or(SecurityKeyError::Failed)?;
    let shared_secret =
        SharedSecret::create(version, authenticator_key_agreement).map_err(|_| SecurityKeyError::Failed)?;

    // NB: The key knows the PIN by the first 16 bytes of its SHA-256.
    let pin_hash = Secret::new(sha_digest(ShaHashKind::Sha256, pin).map_err(|_| SecurityKeyError::Failed)?);
    let pin_hash_enc = shared_secret
        .encrypt(&pin_hash[..16], host)
        .map_err(|_| SecurityKeyError::Failed)?;

    let subcommand = if info.has_pin_uv_auth_token {
        client_pin::GET_PIN_UV_AUTH_TOKEN_USING_PIN_WITH_PERMISSIONS
    } else {
        client_pin::GET_PIN_TOKEN
    };
    let mut parameters = vec![
        (Value::Integer(1), Value::Integer(version.number())),
        (Value::Integer(2), Value::Integer(subcommand)),
        (Value::Integer(3), shared_secret.platform_key_agreement()),
        (Value::Integer(6), Value::Bytes(pin_hash_enc)),
    ];
    if info.has_pin_uv_auth_token {
        parameters.push((
            Value::Integer(9),
            Value::Integer(request.pin_uv_auth_token_permission()),
        ));
        parameters.push((Value::Integer(10), Value::from(request.rp_id())));
    }
    // NB: A PIN that the key gets counts as an attempt, so a ceremony that ended sends none.
    if host.is_cancelled() {
        return Err(SecurityKeyError::Failed);
    }
    // NB: A key with a display can ask the user to agree to the permissions of the token.
    let response = transact_with_user(channel, host, AUTHENTICATOR_CLIENT_PIN, Some(&Value::Map(parameters)))
        .map_err(|_| SecurityKeyError::Failed)?;
    match response.status {
        status::OK => {}
        status::PIN_INVALID => return Err(SecurityKeyError::PinInvalid),
        status::PIN_BLOCKED | status::PIN_AUTH_BLOCKED => return Err(SecurityKeyError::PinBlocked),
        _ => return Err(SecurityKeyError::Failed),
    }

    let encrypted_token = response
        .value
        .get(2)
        .and_then(Value::as_bytes)
        .ok_or(SecurityKeyError::Failed)?;
    let token = shared_secret
        .decrypt(encrypted_token)
        .map_err(|_| SecurityKeyError::Failed)?;
    // NB: The token of protocol one is 16 or 32 bytes, and that of protocol two is 32 bytes.
    let has_right_size = match version {
        Version::One => matches!(token.len(), 16 | 32),
        Version::Two => token.len() == 32,
    };
    if !has_right_size {
        return Err(SecurityKeyError::Failed);
    }
    Ok(PinUvAuthToken { version, token })
}

fn credential_descriptors(credential_ids: &[Vec<u8>]) -> Value {
    Value::Array(
        credential_ids
            .iter()
            .map(|credential_id| {
                Value::Map(vec![
                    (Value::from("id"), Value::Bytes(credential_id.clone())),
                    (Value::from("type"), Value::from("public-key")),
                ])
            })
            .collect(),
    )
}

/// How the client asks a key to verify the user for a request.
#[derive(Clone, Copy, PartialEq, Eq)]
enum UserVerificationMethod {
    None,
    /// The key verifies the user by itself, such as with a fingerprint.
    BuiltIn,
    Pin,
}

/// Whether the client has the PIN of the key that a command is for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HasPin {
    No,
    Yes,
}

/// A request as one key gets it.
pub struct Command {
    pub code: u8,
    parameters: Vec<(Value, Value)>,
    pub is_resident_key: bool,
    /// Whether the command needs a pinUvAuthParam, made with a token that the key gives for the PIN.
    pub needs_pin_uv_auth_token: bool,
}

impl Command {
    pub fn add_pin_uv_auth(&mut self, pin_uv_auth_param: &[u8], version: Version) {
        // NB: pinUvAuthParam and pinUvAuthProtocol are the parameters 8 and 9 of authenticatorMakeCredential, and 6
        //     and 7 of authenticatorGetAssertion, which come after the others in both.
        let first_key = if self.code == AUTHENTICATOR_MAKE_CREDENTIAL {
            8
        } else {
            6
        };
        self.parameters
            .push((Value::Integer(first_key), Value::Bytes(pin_uv_auth_param.to_vec())));
        self.parameters
            .push((Value::Integer(first_key + 1), Value::Integer(version.number())));
    }

    pub fn send(self, channel: &mut Channel) -> Result<(), ChannelError> {
        send_command(channel, self.code, Some(&Value::Map(self.parameters)))
    }
}

impl MakeCredentialRequest {
    /// The authenticatorMakeCredential parameters for the key, or why the key gets no request.
    fn command_for(&self, info: &KeyInfo, has_pin: HasPin) -> Result<Command, SecurityKeyError> {
        // https://w3c.github.io/webauthn/#sctn-createCredential
        // If pkOptions.authenticatorSelection.residentKey is present and set to required: If the authenticator is not
        // capable of storing a client-side discoverable public key credential source, continue.
        if self.resident_key == ResidentKeyRequirement::Required && !info.has_resident_key {
            return Err(SecurityKeyError::NotSupported);
        }
        // Let requireResidentKey be the effective resident key requirement for credential creation.
        let require_resident_key = self.resident_key == ResidentKeyRequirement::Required
            || (self.resident_key == ResidentKeyRequirement::Preferred && info.has_resident_key);

        let mut method = UserVerificationMethod::None;
        // NB: A key that can verify the user makes a credential only for a user that it verified, but for a
        //     non-discoverable one when it says otherwise. The client then leaves the verification out only for a
        //     Relying Party that wants none.
        let may_skip = info.make_credential_uv_not_required
            && !info.always_uv
            && !require_resident_key
            && self.user_verification == UserVerificationRequirement::Discouraged;
        if info.client_pin == Some(true) {
            if !may_skip {
                method = if info.user_verification == Some(true) {
                    UserVerificationMethod::BuiltIn
                } else {
                    UserVerificationMethod::Pin
                };
            }
        } else if !may_skip && info.user_verification == Some(true) {
            method = UserVerificationMethod::BuiltIn;
        } else if self.user_verification == UserVerificationRequirement::Required || info.always_uv {
            // If pkOptions.authenticatorSelection.userVerification is set to required and the authenticator is not
            // capable of performing user verification, continue.
            // FIXME: Offer to set a PIN on a key that can take one.
            return Err(SecurityKeyError::NotSupported);
        }
        let method = method_with_pin(method, info, has_pin)?;

        let mut rp = vec![(Value::from("id"), Value::from(self.rp_id.as_str()))];
        if !self.rp_name.is_empty() {
            rp.push((Value::from("name"), Value::from(self.rp_name.as_str())));
        }

        let mut user = vec![(Value::from("id"), Value::Bytes(self.user_id.clone()))];
        if !self.user_name.is_empty() {
            user.push((Value::from("name"), Value::from(self.user_name.as_str())));
        }
        if !self.user_display_name.is_empty() {
            user.push((Value::from("displayName"), Value::from(self.user_display_name.as_str())));
        }

        let public_key_credential_parameters = self
            .public_key_algorithms
            .iter()
            .map(|algorithm| {
                Value::Map(vec![
                    (Value::from("alg"), Value::Integer(i64::from(*algorithm))),
                    (Value::from("type"), Value::from("public-key")),
                ])
            })
            .collect();

        let mut parameters = vec![
            (Value::Integer(1), Value::Bytes(self.client_data_hash.clone())),
            (Value::Integer(2), Value::Map(rp)),
            (Value::Integer(3), Value::Map(user)),
            (Value::Integer(4), Value::Array(public_key_credential_parameters)),
        ];
        // FIXME: Send a long exclude list in parts, as a key takes only so many credentials in one message.
        if !self.exclude_credential_ids.is_empty() {
            parameters.push((Value::Integer(5), credential_descriptors(&self.exclude_credential_ids)));
        }
        let mut options = Vec::new();
        if require_resident_key {
            options.push((Value::from("rk"), Value::Boolean(true)));
        }
        if method == UserVerificationMethod::BuiltIn {
            options.push((Value::from("uv"), Value::Boolean(true)));
        }
        if !options.is_empty() {
            parameters.push((Value::Integer(7), Value::Map(options)));
        }
        Ok(Command {
            code: AUTHENTICATOR_MAKE_CREDENTIAL,
            parameters,
            is_resident_key: require_resident_key,
            needs_pin_uv_auth_token: method == UserVerificationMethod::Pin,
        })
    }
}

impl GetAssertionRequest {
    /// The authenticatorGetAssertion parameters for the key, or why the key gets no request.
    fn command_for(&self, info: &KeyInfo, has_pin: HasPin) -> Result<Command, SecurityKeyError> {
        // FIXME: A key can say how many credentials one list may name (maxCredentialCountInList). Ask it in batches
        //        of that size, here and for the exclude list.
        // NB: Without an allow list, only a discoverable credential can answer.
        if self.allow_credential_ids.is_empty() && !info.has_resident_key {
            return Err(SecurityKeyError::NoCredentials);
        }

        // https://w3c.github.io/webauthn/#sctn-getAssertion
        // Let userVerification be the effective user verification requirement for assertion.
        let can_verify_user = info.user_verification == Some(true) || info.client_pin == Some(true);
        let user_verification = self.user_verification == UserVerificationRequirement::Required
            || (self.user_verification == UserVerificationRequirement::Preferred && can_verify_user)
            || info.always_uv;
        // If pkOptions.userVerification is set to required and the authenticator is not capable of performing user
        // verification, continue.
        if user_verification && !can_verify_user {
            return Err(SecurityKeyError::NotSupported);
        }
        let mut method = UserVerificationMethod::None;
        if user_verification {
            method = if info.user_verification == Some(true) {
                UserVerificationMethod::BuiltIn
            } else {
                UserVerificationMethod::Pin
            };
        }
        let method = method_with_pin(method, info, has_pin)?;

        let mut parameters = vec![
            (Value::Integer(1), Value::from(self.rp_id.as_str())),
            (Value::Integer(2), Value::Bytes(self.client_data_hash.clone())),
        ];
        // FIXME: Send a long allow list in parts, as a key takes only so many credentials in one message.
        if !self.allow_credential_ids.is_empty() {
            parameters.push((Value::Integer(3), credential_descriptors(&self.allow_credential_ids)));
        }
        if method == UserVerificationMethod::BuiltIn {
            parameters.push((
                Value::Integer(5),
                Value::Map(vec![(Value::from("uv"), Value::Boolean(true))]),
            ));
        }
        Ok(Command {
            code: AUTHENTICATOR_GET_ASSERTION,
            parameters,
            is_resident_key: false,
            needs_pin_uv_auth_token: method == UserVerificationMethod::Pin,
        })
    }
}

fn method_with_pin(
    method: UserVerificationMethod,
    info: &KeyInfo,
    has_pin: HasPin,
) -> Result<UserVerificationMethod, SecurityKeyError> {
    // NB: A key can need the user verification that the request did not ask for, or fail to verify the user by
    //     itself. It says so in its answer, and the client then has its PIN to use.
    let method = if has_pin == HasPin::Yes && info.client_pin == Some(true) {
        UserVerificationMethod::Pin
    } else {
        method
    };
    if method == UserVerificationMethod::Pin && has_pin == HasPin::No {
        return Err(SecurityKeyError::PinRequired);
    }
    Ok(method)
}

impl Request {
    pub fn command_for(&self, info: &KeyInfo, has_pin: HasPin) -> Result<Command, SecurityKeyError> {
        match self {
            Self::MakeCredential(request) => request.command_for(info, has_pin),
            Self::GetAssertion(request) => request.command_for(info, has_pin),
        }
    }

    pub fn rp_id(&self) -> &str {
        match self {
            Self::MakeCredential(request) => &request.rp_id,
            Self::GetAssertion(request) => &request.rp_id,
        }
    }

    pub fn client_data_hash(&self) -> &[u8] {
        match self {
            Self::MakeCredential(request) => &request.client_data_hash,
            Self::GetAssertion(request) => &request.client_data_hash,
        }
    }

    /// The permission of a pinUvAuthToken that the request needs: mc (0x01) or ga (0x02).
    fn pin_uv_auth_token_permission(&self) -> i64 {
        match self {
            Self::MakeCredential(_) => 0x01,
            Self::GetAssertion(_) => 0x02,
        }
    }
}

/// The pinUvAuthParam of the request for a token that the key gave.
pub fn pin_uv_auth_param(token: &PinUvAuthToken, request: &Request) -> Result<Vec<u8>, SecurityKeyError> {
    pin_protocol::authenticate(token.version, &token.token, request.client_data_hash())
        .map_err(|_| SecurityKeyError::Failed)
}

fn is_authenticator_data_for(authenticator_data: &[u8], rp_id: &str) -> bool {
    // https://w3c.github.io/webauthn/#sctn-authenticator-data
    // rpIdHash (32 bytes), flags (1 byte), signCount (4 bytes).
    if authenticator_data.len() < 37 {
        return false;
    }
    sha_digest(ShaHashKind::Sha256, rp_id.as_bytes()).is_ok_and(|rp_id_hash| authenticator_data[..32] == rp_id_hash)
}

/// Whether the flags of authenticator data, which is for the Relying Party of the request, say what the request called
/// for. The client asks for a test of user presence with every request, and the Relying Party gets no answer that says
/// less about the user than that.
fn says_enough_about_the_user(authenticator_data: &[u8], user_verification: UserVerificationRequirement) -> bool {
    let flags = authenticator_data[32];
    flags & AUTHENTICATOR_DATA_FLAG_USER_PRESENT != 0
        && (user_verification != UserVerificationRequirement::Required
            || flags & AUTHENTICATOR_DATA_FLAG_USER_VERIFIED != 0)
}

/// What the response of a key to authenticatorMakeCredential holds, or nothing when the response is not one that the
/// client can use.
pub fn parse_attestation(
    response: &Value,
    request: &MakeCredentialRequest,
    is_resident_key: bool,
) -> Option<Attestation> {
    // NB: The response has fmt (1), authData (2) and attStmt (3).
    let mut authenticator_data = response.get(2)?.as_bytes()?.to_vec();
    if !is_authenticator_data_for(&authenticator_data, &request.rp_id)
        || !says_enough_about_the_user(&authenticator_data, request.user_verification)
    {
        return None;
    }

    // https://w3c.github.io/webauthn/#sctn-attested-credential-data
    // aaguid (16 bytes), credentialIdLength (2 bytes), credentialId, credentialPublicKey.
    const ATTESTED_CREDENTIAL_DATA_OFFSET: usize = 37;
    const CREDENTIAL_ID_OFFSET: usize = ATTESTED_CREDENTIAL_DATA_OFFSET + 16 + 2;
    if authenticator_data[32] & AUTHENTICATOR_DATA_FLAG_ATTESTED_CREDENTIAL_DATA_INCLUDED == 0
        || authenticator_data.len() < CREDENTIAL_ID_OFFSET
    {
        return None;
    }
    let credential_id_size = usize::from(u16::from_be_bytes([
        authenticator_data[CREDENTIAL_ID_OFFSET - 2],
        authenticator_data[CREDENTIAL_ID_OFFSET - 1],
    ]));
    // https://w3c.github.io/webauthn/#credential-id
    // NB: A credential ID is at most 1023 bytes.
    if credential_id_size > 1023 {
        return None;
    }
    let after_credential_id = authenticator_data.get(CREDENTIAL_ID_OFFSET..)?;
    let (credential_id, after_credential_id) = after_credential_id.split_at_checked(credential_id_size)?;
    let credential_id = credential_id.to_vec();

    let (credential_public_key, credential_public_key_size) = cbor::decode(after_credential_id).ok()?;
    // NB: After the credential public key come the extensions, which are one CBOR map, when the flags say that the
    //     authenticator data has them, and nothing else.
    let extensions = &after_credential_id[credential_public_key_size..];
    if authenticator_data[32] & AUTHENTICATOR_DATA_FLAG_EXTENSION_DATA_INCLUDED == 0 {
        if !extensions.is_empty() {
            return None;
        }
    } else if !matches!(cbor::decode(extensions), Ok((Value::Map(_), size)) if size == extensions.len()) {
        return None;
    }
    // https://www.rfc-editor.org/rfc/rfc9052#section-7
    // alg (3), and for an EC2 key the coordinates x (-2) and y (-3).
    let algorithm = credential_public_key.get(3)?.as_integer()?;
    // NB: A credential with an algorithm that the Relying Party did not name is of no use to it.
    if !request
        .public_key_algorithms
        .iter()
        .any(|offered| i64::from(*offered) == algorithm)
    {
        return None;
    }
    let public_key_algorithm = i32::try_from(algorithm).ok()?;
    let mut public_key = None;
    if algorithm == COSE_ALGORITHM_ES256 {
        // NB: An EC2 key has the key type kty (1), which is EC2 (2), and the curve crv (-1), which is P-256 (1) here.
        let key_type = credential_public_key.get(1).and_then(Value::as_integer);
        let curve = credential_public_key.get(-1).and_then(Value::as_integer);
        let x = credential_public_key.get(-2).and_then(Value::as_bytes);
        let y = credential_public_key.get(-3).and_then(Value::as_bytes);
        if let (Some(2), Some(1), Some(x), Some(y)) = (key_type, curve, x, y)
            && x.len() == 32
            && y.len() == 32
        {
            public_key = ec_key_from_coordinates(x, y, None).and_then(|key| ec_export_der(&key).ok());
        }
    } else if algorithm == COSE_ALGORITHM_EDDSA {
        // NB: An OKP key has the key type kty (1), which is OKP (1), the curve crv (-1), which is Ed25519 (6) here, and
        //     the public key x (-2).
        let key_type = credential_public_key.get(1).and_then(Value::as_integer);
        let curve = credential_public_key.get(-1).and_then(Value::as_integer);
        let x = credential_public_key.get(-2).and_then(Value::as_bytes);
        if let (Some(1), Some(6), Some(x)) = (key_type, curve, x)
            && x.len() == 32
        {
            public_key = okp_key_from_raw(OkpCurve::Ed25519, false, x)
                .ok()
                .and_then(|key| okp_export_der(&key).ok());
        }
    } else if algorithm == COSE_ALGORITHM_RS256 {
        // NB: An RSA key has the key type kty (1), which is RSA (3), the modulus n (-1) and the exponent e (-2).
        let key_type = credential_public_key.get(1).and_then(Value::as_integer);
        let modulus = credential_public_key.get(-1).and_then(Value::as_bytes);
        let exponent = credential_public_key.get(-2).and_then(Value::as_bytes);
        if let (Some(3), Some(modulus), Some(exponent)) = (key_type, modulus, exponent) {
            public_key = rsa_key_from_components(false, &[modulus, exponent]).and_then(|key| rsa_export_der(&key).ok());
        }
    }

    // NB: The client knows how a key of these algorithms looks, so one that it cannot read is not a key that the
    //     Relying Party can verify an assertion with.
    let is_known_algorithm = matches!(
        algorithm,
        COSE_ALGORITHM_ES256 | COSE_ALGORITHM_EDDSA | COSE_ALGORITHM_RS256
    );
    if is_known_algorithm && public_key.is_none() {
        return None;
    }

    // https://w3c.github.io/webauthn/#sctn-generating-an-attestation-object
    // FIXME: Pass the attestation statement of the key on when the Relying Party asks for attestation and the user
    //        agrees. This is the "none" attestation conveyance.
    // NB: For the "none" attestation conveyance, the client takes away what can identify the key: The aaguid in the
    //     attested credential data becomes 16 zero bytes, the fmt becomes "none" and the attStmt an empty map.
    authenticator_data[ATTESTED_CREDENTIAL_DATA_OFFSET..ATTESTED_CREDENTIAL_DATA_OFFSET + 16].fill(0);
    let attestation_object = cbor::encode(&Value::Map(vec![
        (Value::from("fmt"), Value::from("none")),
        (Value::from("attStmt"), Value::Map(Vec::new())),
        (Value::from("authData"), Value::Bytes(authenticator_data.clone())),
    ]));
    Some(Attestation {
        credential_id,
        authenticator_data,
        attestation_object,
        public_key,
        public_key_algorithm,
        is_resident_key,
    })
}

/// What the response of a key to authenticatorGetAssertion or authenticatorGetNextAssertion holds, or nothing when
/// the response is not one that the client can use.
pub fn parse_assertion(response: &Value, request: &GetAssertionRequest) -> Option<Assertion> {
    // NB: The response has credential (1), authData (2), signature (3), user (4) and numberOfCredentials (5).
    // NB: A credential that is there says which one it is.
    let credential_id = match response.get(1) {
        Some(credential) => Some(credential.get_text("id")?.as_bytes()?),
        None => None,
    };
    let credential_id = match (credential_id, request.allow_credential_ids.as_slice()) {
        // NB: A key answers with a credential of the list when there is one. Another credential is not what the
        //     Relying Party asked for.
        (Some(credential_id), allowed) if allowed.is_empty() || allowed.iter().any(|id| id == credential_id) => {
            credential_id.to_vec()
        }
        (Some(_), _) => return None,
        // NB: A key can leave the credential out when the allow list has only the one.
        (None, [only_credential_id]) => only_credential_id.clone(),
        (None, _) => return None,
    };

    let authenticator_data = response.get(2)?.as_bytes()?;
    let signature = response.get(3)?.as_bytes()?;
    if !is_authenticator_data_for(authenticator_data, &request.rp_id)
        || !says_enough_about_the_user(authenticator_data, request.user_verification)
    {
        return None;
    }

    let user = response.get(4);
    let user_text = |name| {
        user.and_then(|user| user.get_text(name))
            .and_then(Value::as_text)
            .unwrap_or_default()
            .to_owned()
    };
    let user_handle = user
        .and_then(|user| user.get_text("id"))
        .and_then(Value::as_bytes)
        .map(<[u8]>::to_vec);
    // NB: Without an allow list, the credential is a discoverable one, and the Relying Party finds the account with
    //     the user handle.
    if request.allow_credential_ids.is_empty() && user_handle.is_none() {
        return None;
    }
    Some(Assertion {
        credential_id,
        authenticator_data: authenticator_data.to_vec(),
        signature: signature.to_vec(),
        user_handle,
        user_name: user_text("name"),
        user_display_name: user_text("displayName"),
    })
}
