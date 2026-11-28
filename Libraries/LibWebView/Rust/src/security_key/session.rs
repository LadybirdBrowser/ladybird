/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! One request to the security keys that are connected, or that the user connects while it waits. The request goes to
//! all of them, and the key that the user touches answers it.

use super::ctap::{
    self, Assertion, Attestation, HasPin, KeyInfo, Request, Response, SecurityKeyError, parse_assertion,
    parse_attestation, status,
};
use super::ctaphid::{self, Channel, POLL_INTERVAL_IN_MILLISECONDS};
use super::{HidDevice, Host, Secret};
use crate::cbor::Value;
use std::collections::{HashMap, HashSet};

const ENUMERATION_INTERVAL_IN_MILLISECONDS: u64 = 500;
/// A device that did not answer in time when it was connected a moment ago gets a few more tries.
const MAXIMUM_ATTEMPTS_TO_OPEN_A_DEVICE: u8 = 3;

/// The PIN that the user gave for one key. No other key gets it: another key would count it as a wrong PIN, and a
/// device that only looks like a key would learn about it.
pub struct Pin {
    pub bytes: Secret,
    pub device_id: u64,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Outcome {
    Attestation(Attestation),
    /// The assertions of all the credentials of the key that fit the request.
    Assertions(Vec<Assertion>),
    Failed {
        error: SecurityKeyError,
        /// The key that the user chose, when it needs its PIN or did not take the PIN.
        device_that_wants_pin: Option<u64>,
    },
    Cancelled,
}

impl Outcome {
    fn failure(error: SecurityKeyError, device_id: u64) -> Self {
        let is_about_pin = matches!(error, SecurityKeyError::PinRequired | SecurityKeyError::PinInvalid);
        Self::Failed {
            error,
            device_that_wants_pin: is_about_pin.then_some(device_id),
        }
    }
}

/// What a key that got a command answers when the user touches it.
#[derive(Clone, Copy)]
enum Waiting {
    /// The request itself.
    ForRequest { is_resident_key: bool },
    /// The request with a pinUvAuthParam of zero length. A key that gets it waits for a touch, and then answers that
    /// the PIN is not valid or not set. The touch chooses the key, and the client then asks the user for its PIN.
    ToBeChosen,
    /// The next assertion of the key that the user chose, which has this many more after it. The key gives it
    /// without the user, so it has until the deadline.
    ForNextAssertion { remaining: i64, deadline: u64 },
}

struct Key {
    channel: Channel,
    info: KeyInfo,
    waiting: Waiting,
    /// Whether the key said that it needs a user verification for the request, though the request and what the key
    /// says about itself do not call for one.
    needs_user_verification: bool,
}

/// What became of a device that the session opened.
enum Opened {
    Key(Channel, KeyInfo),
    /// The device has no part in the session.
    NotAKey,
    /// The device did not answer. It can be one that the user connected a moment ago.
    NoAnswer,
}

/// Why a key that left the session gave no credential.
#[derive(Clone, Copy)]
struct Refusal {
    error: SecurityKeyError,
    device_id: u64,
}

pub struct Session<'a> {
    request: &'a Request,
    pin: Option<&'a Pin>,
    host: &'a mut dyn Host,
    known_device_ids: HashSet<u64>,
    /// The keys that got no request because the user gave the PIN of another key.
    device_ids_left_out_for_the_pin: HashSet<u64>,
    /// Whether keys that were left out are new to the session again, and no enumeration came after that yet.
    has_devices_to_take_in_again: bool,
    attempts_to_open: HashMap<u64, u8>,
    /// The keys that wait for the user.
    keys: Vec<Key>,
    assertions: Vec<Assertion>,
    /// What to tell the user when no key is left to wait for.
    refusal: Option<Refusal>,
    /// Whether the user chose a key with a touch. No other key gets the request after that.
    has_chosen_key: bool,
    /// Whether one device is connected, which is then the key that the user means.
    is_only_device: bool,
    /// Whether the key that the PIN is for is connected. A key that was connected again is another device, so the PIN
    /// that the user gave for it before is for no key that is there. The session then goes on as one without a PIN,
    /// and asks for it again.
    is_pin_usable: bool,
}

impl<'a> Session<'a> {
    pub fn new(request: &'a Request, pin: Option<&'a Pin>, host: &'a mut dyn Host) -> Self {
        Self {
            request,
            pin,
            host,
            known_device_ids: HashSet::new(),
            device_ids_left_out_for_the_pin: HashSet::new(),
            has_devices_to_take_in_again: false,
            attempts_to_open: HashMap::new(),
            keys: Vec::new(),
            assertions: Vec::new(),
            refusal: None,
            has_chosen_key: false,
            is_only_device: false,
            is_pin_usable: false,
        }
    }

    pub fn run(mut self) -> Outcome {
        let mut last_enumeration: Option<u64> = None;
        while !self.host.is_cancelled() {
            let now = self.host.now_in_milliseconds();
            let is_time_to_enumerate =
                last_enumeration.is_none_or(|last| now.saturating_sub(last) >= ENUMERATION_INTERVAL_IN_MILLISECONDS);
            if !self.has_chosen_key && is_time_to_enumerate {
                last_enumeration = Some(now);
                if let Some(outcome) = self.take_in_new_devices() {
                    return outcome;
                }
            }

            if self.keys.is_empty() {
                // NB: No key waits, so the key of the PIN does not either, and the keys that were left out for it get
                //     the request before the session ends for what another key said.
                if !self.device_ids_left_out_for_the_pin.is_empty() {
                    self.take_in_devices_left_out_for_the_pin();
                }
                // NB: The next enumeration takes those keys in, at the time that it is due.
                if self.has_devices_to_take_in_again && !self.has_chosen_key {
                    self.host.sleep(POLL_INTERVAL_IN_MILLISECONDS);
                    continue;
                }
                // NB: A key that cannot answer the request ends the session when no other key is there to wait for.
                if let Some(refusal) = self.refusal {
                    return Outcome::failure(refusal.error, refusal.device_id);
                }
                // NB: The key that the user chose is gone.
                if self.has_chosen_key {
                    return Outcome::Failed {
                        error: SecurityKeyError::Failed,
                        device_that_wants_pin: None,
                    };
                }
                self.host.sleep(POLL_INTERVAL_IN_MILLISECONDS);
                continue;
            }

            let mut index = 0;
            while index < self.keys.len() {
                let message = match self.keys[index].channel.poll(POLL_INTERVAL_IN_MILLISECONDS) {
                    Ok(Some(message)) => message,
                    Ok(None) => {
                        if let Waiting::ForNextAssertion { deadline, .. } = self.keys[index].waiting
                            && self.host.now_in_milliseconds() >= deadline
                        {
                            let device_id = self.keys[index].channel.device_id();
                            self.cancel_all_keys();
                            return Outcome::failure(SecurityKeyError::Failed, device_id);
                        }
                        index += 1;
                        continue;
                    }
                    Err(_) => {
                        // NB: The user can connect the key again, so that it is gone is no error of the session.
                        let key = self.keys.remove(index);
                        self.known_device_ids.remove(&key.channel.device_id());
                        continue;
                    }
                };
                // NB: A ceremony that ended while the key answered sends the key no more commands.
                if self.host.is_cancelled() {
                    break;
                }
                let outcome = match ctap::parse_response(&message) {
                    Some(response) => self.handle_response(index, &response),
                    None => self.remove_key(index, SecurityKeyError::Failed),
                };
                if let Some(outcome) = outcome {
                    return outcome;
                }
            }
        }

        self.cancel_all_keys();
        Outcome::Cancelled
    }

    fn usable_pin(&self) -> Option<&'a Pin> {
        self.pin.filter(|_| self.is_pin_usable)
    }

    /// Makes the keys that were left out for the PIN of another key new to the session, for when that key is gone.
    fn take_in_devices_left_out_for_the_pin(&mut self) {
        self.is_pin_usable = false;
        for device_id in self.device_ids_left_out_for_the_pin.drain() {
            self.known_device_ids.remove(&device_id);
            self.has_devices_to_take_in_again = true;
        }
    }

    fn cancel_all_keys(&mut self) {
        for key in &mut self.keys {
            key.channel.cancel();
        }
    }

    /// Gives the request to the devices that were connected since the last time. This ends the session when the key
    /// that the user gave the PIN for refuses the request.
    fn take_in_new_devices(&mut self) -> Option<Outcome> {
        if !self.is_pin_usable {
            self.take_in_devices_left_out_for_the_pin();
        }
        let new_devices = self.host.open_new_devices(&self.known_device_ids);
        self.has_devices_to_take_in_again = false;
        self.is_only_device = self.known_device_ids.len() + new_devices.len() == 1;
        // NB: The PIN is of use while its key has the request, or is about to get it. A device with the ID of that
        //     key that the session could not open, or that is no key, is nothing to wait for.
        self.is_pin_usable = self.pin.is_some_and(|pin| {
            self.keys.iter().any(|key| key.channel.device_id() == pin.device_id)
                || new_devices.iter().any(|device| device.id() == pin.device_id)
        });

        for device in new_devices {
            // NB: Opening a device takes time, and a ceremony that ended has no use for another device.
            if self.host.is_cancelled() {
                return None;
            }
            let device_id = device.id();
            self.known_device_ids.insert(device_id);
            let (channel, info) = match self.open(device) {
                Opened::Key(channel, info) => (channel, info),
                Opened::NotAKey => continue,
                Opened::NoAnswer => {
                    let attempts = self.attempts_to_open.entry(device_id).or_default();
                    *attempts += 1;
                    if *attempts < MAXIMUM_ATTEMPTS_TO_OPEN_A_DEVICE {
                        self.known_device_ids.remove(&device_id);
                    }
                    continue;
                }
            };
            let mut key = Key {
                channel,
                info,
                waiting: Waiting::ToBeChosen,
                needs_user_verification: false,
            };
            match self.offer_request(&mut key) {
                Ok(true) => self.keys.push(key),
                Ok(false) => {}
                Err(error) => {
                    if let Some(outcome) = self.refuse(error, device_id) {
                        return Some(outcome);
                    }
                }
            }
        }
        None
    }

    fn open(&mut self, device: Box<dyn HidDevice>) -> Opened {
        let Ok(mut channel) = Channel::open(device, self.host) else {
            return Opened::NoAnswer;
        };
        if !channel.supports_cbor() {
            return Opened::NotAKey;
        }
        let Ok(info) = ctap::get_info(&mut channel, self.host) else {
            return Opened::NoAnswer;
        };
        // FIXME: Use a key that only has the U2F protocol.
        if !info.is_ctap2 {
            return Opened::NotAKey;
        }
        Opened::Key(channel, info)
    }

    /// Sends the request to the key, and says whether the key now waits for the user. A key that gets no request
    /// either has an error to tell the user, or is left out without one.
    fn offer_request(&mut self, key: &mut Key) -> Result<bool, SecurityKeyError> {
        // NB: Opening the key took time, and a ceremony that ended in that time asks no key for a touch.
        if self.host.is_cancelled() {
            return Ok(false);
        }
        let pin = self.usable_pin();
        let pin_for_key = pin.filter(|pin| pin.device_id == key.channel.device_id());
        let has_pin = if pin_for_key.is_some() { HasPin::Yes } else { HasPin::No };

        let command = self.request.command_for(&key.info, has_pin);
        let needs_pin = matches!(command, Err(SecurityKeyError::PinRequired))
            || (key.needs_user_verification && pin_for_key.is_none());
        if needs_pin {
            // NB: The user gave the PIN of another key, so this key has no part in the session.
            if pin.is_some() {
                self.device_ids_left_out_for_the_pin.insert(key.channel.device_id());
                return Ok(false);
            }
            // NB: With one key, that is the key that the user means. A device that asks for the PIN in its answer,
            //     when nothing that it said about itself called for one, gets the PIN only after a touch, like any
            //     key that the user did not choose yet.
            if self.is_only_device && !key.needs_user_verification {
                return Err(SecurityKeyError::PinRequired);
            }
            // NB: With more keys, the user chooses one with a touch before the client asks for a PIN, so that the PIN
            //     goes to that key. The pinUvAuthProtocol is one that the key has, also with a pinUvAuthParam of zero
            //     length.
            let mut command = self.request.command_for(&key.info, HasPin::Yes)?;
            command.add_pin_uv_auth(&[], key.info.pin_uv_auth_protocol());
            command.send(&mut key.channel).map_err(|_| SecurityKeyError::Failed)?;
            key.waiting = Waiting::ToBeChosen;
            return Ok(true);
        }

        let mut command = command?;
        let is_resident_key = command.is_resident_key;
        if command.needs_pin_uv_auth_token {
            let pin = pin_for_key.ok_or(SecurityKeyError::Failed)?;
            // NB: The key gets a new token for every request, as it can drop a token that it has used.
            let token = ctap::get_pin_uv_auth_token(&mut key.channel, self.host, &key.info, &pin.bytes, self.request)?;
            // NB: The exchange takes time, and a ceremony that ended in that time asks no key for a touch.
            if self.host.is_cancelled() {
                return Ok(false);
            }
            let pin_uv_auth_param = ctap::pin_uv_auth_param(&token, self.request)?;
            command.add_pin_uv_auth(&pin_uv_auth_param, token.version);
        }
        command.send(&mut key.channel).map_err(|_| SecurityKeyError::Failed)?;
        key.waiting = Waiting::ForRequest { is_resident_key };
        Ok(true)
    }

    /// Ends the part of a key whose answer is not one that the client can use. That ends the session when the key is
    /// the one that the user chose.
    fn remove_key_with_unusable_answer(&mut self, index: usize) -> Option<Outcome> {
        if self.has_chosen_key {
            let device_id = self.keys[index].channel.device_id();
            return Some(Outcome::failure(SecurityKeyError::Failed, device_id));
        }
        self.remove_key(index, SecurityKeyError::Failed)
    }

    /// Sends the request to the key again, such as after the key stopped waiting for a touch.
    fn offer_request_again(&mut self, index: usize) -> Option<Outcome> {
        // NB: The assertions that the key gave for the request before are not part of its answer to this one.
        self.assertions.clear();
        let mut key = self.keys.remove(index);
        match self.offer_request(&mut key) {
            Ok(true) => self.keys.insert(index, key),
            Ok(false) => {}
            Err(error) => return self.refuse(error, key.channel.device_id()),
        }
        None
    }

    /// Takes note of why a key that is no longer in the session gave no credential. The session ends with that when
    /// no key is left to wait for, or at once when the key is the one that the user gave the PIN for.
    fn refuse(&mut self, error: SecurityKeyError, device_id: u64) -> Option<Outcome> {
        if self.usable_pin().is_some_and(|pin| pin.device_id == device_id) {
            self.cancel_all_keys();
            return Some(Outcome::failure(error, device_id));
        }
        // NB: The error that says the most to the user wins.
        if self
            .refusal
            .is_none_or(|refusal| refusal.error == SecurityKeyError::Failed)
        {
            self.refusal = Some(Refusal { error, device_id });
        }
        None
    }

    fn remove_key(&mut self, index: usize, error: SecurityKeyError) -> Option<Outcome> {
        let key = self.keys.remove(index);
        self.refuse(error, key.channel.device_id())
    }

    /// The user chose the key with a touch, so the others no longer matter. The key is then the only one.
    fn choose_key(&mut self, index: usize) -> u64 {
        self.has_chosen_key = true;
        // NB: Why another key gave no credential says nothing about the key that the user chose.
        self.refusal = None;
        let chosen = self.keys.remove(index);
        self.cancel_all_keys();
        self.keys.clear();
        let device_id = chosen.channel.device_id();
        self.keys.push(chosen);
        device_id
    }

    /// The outcome of the session, when the response of the key decides it.
    fn handle_response(&mut self, index: usize, response: &Response) -> Option<Outcome> {
        if matches!(self.keys[index].waiting, Waiting::ToBeChosen) {
            let error = match response.status {
                // NB: The user chose this key with a touch, and the client now asks for its PIN.
                status::OK | status::PIN_INVALID | status::PIN_AUTH_INVALID => SecurityKeyError::PinRequired,
                status::PIN_BLOCKED | status::PIN_AUTH_BLOCKED => SecurityKeyError::PinBlocked,
                // NB: The user chose a key that has no PIN to ask for.
                // FIXME: Offer to set a PIN on a key that can take one.
                status::PIN_NOT_SET => SecurityKeyError::NotSupported,
                // NB: A key can look at the request before it looks at the PIN.
                status::CREDENTIAL_EXCLUDED => SecurityKeyError::CredentialExcluded,
                status::NO_CREDENTIALS => return self.remove_key(index, SecurityKeyError::NoCredentials),
                status::USER_ACTION_TIMEOUT => return self.offer_request_again(index),
                _ => return self.remove_key(index, SecurityKeyError::Failed),
            };
            let device_id = self.choose_key(index);
            return Some(Outcome::failure(error, device_id));
        }

        // NB: The key gives its next assertion without the user, so there is nothing to ask it again for.
        if matches!(self.keys[index].waiting, Waiting::ForNextAssertion { .. }) && response.status != status::OK {
            return self.remove_key_with_unusable_answer(index);
        }

        match response.status {
            status::OK => {}
            status::CREDENTIAL_EXCLUDED => {
                let device_id = self.choose_key(index);
                return Some(Outcome::failure(SecurityKeyError::CredentialExcluded, device_id));
            }
            status::PUAT_REQUIRED | status::UV_INVALID | status::UV_BLOCKED => {
                // NB: The key needs a user verification that the request did not ask for, or did not verify the user
                //     by itself, such as with a fingerprint. Its PIN can do both. Any device can answer so at once, so
                //     the answer does not make it the key that the user chose: The user chooses it with a touch
                //     before the client asks for its PIN.
                let key = &mut self.keys[index];
                if key.needs_user_verification || key.info.client_pin != Some(true) {
                    return self.remove_key(index, SecurityKeyError::NotSupported);
                }
                key.needs_user_verification = true;
                return self.offer_request_again(index);
            }
            // NB: The key stops waiting for a touch after a while, and the session waits on.
            status::USER_ACTION_TIMEOUT => return self.offer_request_again(index),
            other => {
                let error = match other {
                    status::NO_CREDENTIALS => SecurityKeyError::NoCredentials,
                    // NB: The key needs a user verification, and has no PIN to do it with.
                    // FIXME: Offer to set a PIN on a key that can take one.
                    status::PIN_NOT_SET => SecurityKeyError::NotSupported,
                    status::PIN_INVALID => SecurityKeyError::PinInvalid,
                    status::PIN_BLOCKED | status::PIN_AUTH_BLOCKED => SecurityKeyError::PinBlocked,
                    _ => SecurityKeyError::Failed,
                };
                return self.remove_key(index, error);
            }
        }

        // NB: Only an answer that the client can use chooses the key. After an answer that it cannot use, the other
        //     keys still wait for the user.
        let waiting = self.keys[index].waiting;
        match self.request {
            Request::MakeCredential(request) => {
                let is_resident_key = matches!(waiting, Waiting::ForRequest { is_resident_key: true });
                let Some(attestation) = parse_attestation(&response.value, request, is_resident_key) else {
                    return self.remove_key_with_unusable_answer(index);
                };
                self.choose_key(index);
                Some(Outcome::Attestation(attestation))
            }
            Request::GetAssertion(request) => {
                let Some(assertion) = parse_assertion(&response.value, request) else {
                    return self.remove_key_with_unusable_answer(index);
                };
                // NB: Every assertion of a key is for another of its credentials.
                if self
                    .assertions
                    .iter()
                    .any(|earlier| earlier.credential_id == assertion.credential_id)
                {
                    return self.remove_key_with_unusable_answer(index);
                }
                let device_id = self.choose_key(index);
                self.assertions.push(assertion);

                let remaining = match waiting {
                    Waiting::ForNextAssertion { remaining, .. } => remaining - 1,
                    _ => {
                        let number_of_credentials = response.value.get(5).and_then(Value::as_integer).unwrap_or(1);
                        number_of_credentials.clamp(1, ctap::MAXIMUM_ASSERTIONS_OF_ONE_KEY) - 1
                    }
                };
                if remaining <= 0 {
                    return Some(Outcome::Assertions(std::mem::take(&mut self.assertions)));
                }
                let key = &mut self.keys[0];
                if ctap::send_command(&mut key.channel, ctap::AUTHENTICATOR_GET_NEXT_ASSERTION, None).is_err() {
                    return Some(Outcome::failure(SecurityKeyError::Failed, device_id));
                }
                let deadline = self.host.now_in_milliseconds() + ctaphid::DEVICE_ANSWER_TIMEOUT_IN_MILLISECONDS;
                key.waiting = Waiting::ForNextAssertion { remaining, deadline };
                None
            }
        }
    }
}
