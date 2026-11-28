/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Memory.h>
#include <AK/Variant.h>
#include <LibCore/EventLoop.h>
#include <LibThreading/Thread.h>
#include <LibWebView/FidoHIDDevice.h>
#include <LibWebView/SecurityKey.h>
#include <RustFFI.h>

namespace WebView {

using Web::WebAuthentication::ResidentKeyRequirement;
using Web::WebAuthentication::UserVerificationRequirement;

#if !defined(AK_OS_MACOS)
// FIXME: Reach the security keys through hidraw on Linux, and through the platform of the other systems.
Vector<NonnullOwnPtr<FidoHIDDevice>> FidoHIDDevice::open_new_devices(HashTable<u64> const&)
{
    return {};
}

bool FidoHIDDevice::is_any_connected()
{
    return false;
}
#endif

using SecurityKeyRequest = Variant<SecurityKeyMakeCredentialRequest, SecurityKeyGetAssertionRequest>;

struct SecurityKeyOutcome {
    Optional<SecurityKeyAttestation> attestation;
    Vector<SecurityKeyAssertion> assertions;
    SecurityKeyError error { SecurityKeyError::Failed };
    // The key that the user chose, when it needs its PIN or did not take the PIN.
    Optional<u64> device_that_wants_pin;
    bool is_success { false };
    bool was_cancelled { false };
};

// NB: The session itself is in Rust (Libraries/LibWebView/Rust/src/security_key). What follows gives it the devices of
//     the machine, and takes its result.

static FFI::FfiBytes ffi_bytes(ReadonlyBytes bytes)
{
    return { bytes.data(), bytes.size() };
}

static ByteBuffer byte_buffer(FFI::FfiBytes bytes)
{
    return MUST(ByteBuffer::copy(bytes.data, bytes.length));
}

static Vector<FFI::FfiBytes> ffi_byte_strings(Vector<ByteBuffer> const& buffers)
{
    Vector<FFI::FfiBytes> byte_strings;
    byte_strings.ensure_capacity(buffers.size());
    for (auto const& buffer : buffers)
        byte_strings.unchecked_append(ffi_bytes(buffer));
    return byte_strings;
}

static FFI::FfiPin ffi_pin(ReadonlyBytes pin, Optional<u64> pin_device_id)
{
    // NB: A PIN that is for no key goes to no key.
    if (!pin_device_id.has_value())
        return { ffi_bytes({}), 0 };
    return { ffi_bytes(pin), *pin_device_id };
}

static FFI::ResidentKeyRequirement ffi_resident_key_requirement(ResidentKeyRequirement requirement)
{
    switch (requirement) {
    case ResidentKeyRequirement::Discouraged:
        return FFI::ResidentKeyRequirement::Discouraged;
    case ResidentKeyRequirement::Preferred:
        return FFI::ResidentKeyRequirement::Preferred;
    case ResidentKeyRequirement::Required:
        return FFI::ResidentKeyRequirement::Required;
    }
    VERIFY_NOT_REACHED();
}

static FFI::UserVerificationRequirement ffi_user_verification_requirement(UserVerificationRequirement requirement)
{
    switch (requirement) {
    case UserVerificationRequirement::Required:
        return FFI::UserVerificationRequirement::Required;
    case UserVerificationRequirement::Preferred:
        return FFI::UserVerificationRequirement::Preferred;
    case UserVerificationRequirement::Discouraged:
        return FFI::UserVerificationRequirement::Discouraged;
    }
    VERIFY_NOT_REACHED();
}

static SecurityKeyError security_key_error(FFI::SecurityKeyError error)
{
    switch (error) {
    case FFI::SecurityKeyError::CredentialExcluded:
        return SecurityKeyError::CredentialExcluded;
    case FFI::SecurityKeyError::NoCredentials:
        return SecurityKeyError::NoCredentials;
    case FFI::SecurityKeyError::PinRequired:
        return SecurityKeyError::PinRequired;
    case FFI::SecurityKeyError::PinInvalid:
        return SecurityKeyError::PinInvalid;
    case FFI::SecurityKeyError::PinBlocked:
        return SecurityKeyError::PinBlocked;
    case FFI::SecurityKeyError::NotSupported:
        return SecurityKeyError::NotSupported;
    case FFI::SecurityKeyError::Failed:
        return SecurityKeyError::Failed;
    }
    VERIFY_NOT_REACHED();
}

static FFI::FfiHost ffi_host(Atomic<bool> const& is_cancelled)
{
    return {
        .context = const_cast<Atomic<bool>*>(&is_cancelled),
        .open_new_devices = [](void*, u64 const* known_device_ids, size_t known_device_count, void* devices, FFI::FfiAddDevice add_device) {
            HashTable<u64> known;
            for (size_t i = 0; i < known_device_count; ++i)
                known.set(known_device_ids[i]);
            for (auto& device : FidoHIDDevice::open_new_devices(known)) {
                auto id = device->id();
                // NB: The session owns the device until it gives it to close_device.
                add_device(devices, device.leak_ptr(), id);
            } },
        .write_report = [](void* device, u8 const* report) { return !static_cast<FidoHIDDevice*>(device)->write_report({ report, FidoHIDDevice::report_size }).is_error(); },
        .read_report = [](void* device, u32 timeout_in_milliseconds, u8* report) -> i32 {
            auto maybe_report = static_cast<FidoHIDDevice*>(device)->read_report(timeout_in_milliseconds);
            if (maybe_report.is_error())
                return -1;
            auto input = maybe_report.release_value();
            if (!input.has_value())
                return 0;
            input->span().copy_to({ report, FidoHIDDevice::report_size });
            return 1;
        },
        .close_device = [](void* device) { delete static_cast<FidoHIDDevice*>(device); },
        .is_cancelled = [](void* context) { return static_cast<Atomic<bool> const*>(context)->load(); },
    };
}

static void take_result(SecurityKeyOutcome& outcome, FFI::FfiOutcome const& result)
{
    switch (result.kind) {
    case FFI::FfiOutcomeKind::Success:
        outcome.is_success = true;
        break;
    case FFI::FfiOutcomeKind::Failed:
        outcome.error = security_key_error(result.error);
        if (result.has_device_that_wants_pin)
            outcome.device_that_wants_pin = result.device_that_wants_pin;
        break;
    case FFI::FfiOutcomeKind::Cancelled:
        outcome.was_cancelled = true;
        break;
    }
}

static SecurityKeyOutcome run_request(SecurityKeyMakeCredentialRequest const& request, Atomic<bool> const& is_cancelled)
{
    auto host = ffi_host(is_cancelled);
    auto exclude_credential_ids = ffi_byte_strings(request.exclude_credential_ids);
    FFI::FfiMakeCredentialRequest ffi_request {
        .client_data_hash = ffi_bytes(request.client_data_hash),
        .rp_id = ffi_bytes(request.rp_id),
        .rp_name = ffi_bytes(request.rp_name),
        .user_id = ffi_bytes(request.user_id),
        .user_name = ffi_bytes(request.user_name),
        .user_display_name = ffi_bytes(request.user_display_name),
        .public_key_algorithms = request.public_key_algorithms.data(),
        .public_key_algorithm_count = request.public_key_algorithms.size(),
        .exclude_credential_ids = exclude_credential_ids.data(),
        .exclude_credential_id_count = exclude_credential_ids.size(),
        .resident_key = ffi_resident_key_requirement(request.resident_key),
        .user_verification = ffi_user_verification_requirement(request.user_verification),
    };
    auto pin = ffi_pin(request.pin, request.pin_device_id);

    SecurityKeyOutcome outcome;
    auto result = FFI::webview_security_key_make_credential(&host, &ffi_request, &pin, &outcome, [](void* context, FFI::FfiAttestation const* attestation) {
        static_cast<SecurityKeyOutcome*>(context)->attestation = SecurityKeyAttestation {
            .credential_id = byte_buffer(attestation->credential_id),
            .authenticator_data = byte_buffer(attestation->authenticator_data),
            .attestation_object = byte_buffer(attestation->attestation_object),
            .public_key = attestation->has_public_key ? Optional<ByteBuffer> { byte_buffer(attestation->public_key) } : Optional<ByteBuffer> {},
            .public_key_algorithm = attestation->public_key_algorithm,
            .is_resident_key = attestation->is_resident_key,
        };
    });
    take_result(outcome, result);
    return outcome;
}

static SecurityKeyOutcome run_request(SecurityKeyGetAssertionRequest const& request, Atomic<bool> const& is_cancelled)
{
    auto host = ffi_host(is_cancelled);
    auto allow_credential_ids = ffi_byte_strings(request.allow_credential_ids);
    FFI::FfiGetAssertionRequest ffi_request {
        .client_data_hash = ffi_bytes(request.client_data_hash),
        .rp_id = ffi_bytes(request.rp_id),
        .allow_credential_ids = allow_credential_ids.data(),
        .allow_credential_id_count = allow_credential_ids.size(),
        .user_verification = ffi_user_verification_requirement(request.user_verification),
    };
    auto pin = ffi_pin(request.pin, request.pin_device_id);

    SecurityKeyOutcome outcome;
    auto result = FFI::webview_security_key_get_assertion(&host, &ffi_request, &pin, &outcome, [](void* context, FFI::FfiAssertion const* assertion) {
        static_cast<SecurityKeyOutcome*>(context)->assertions.append(SecurityKeyAssertion {
            .credential_id = byte_buffer(assertion->credential_id),
            .authenticator_data = byte_buffer(assertion->authenticator_data),
            .signature = byte_buffer(assertion->signature),
            .user_handle = assertion->has_user_handle ? Optional<ByteBuffer> { byte_buffer(assertion->user_handle) } : Optional<ByteBuffer> {},
            .user_name = byte_buffer(assertion->user_name),
            .user_display_name = byte_buffer(assertion->user_display_name),
        });
    });
    take_result(outcome, result);
    return outcome;
}

static void run_session(NonnullRefPtr<SecurityKeySession> session, Atomic<bool> const& is_cancelled, SecurityKeyRequest request, Function<void(SecurityKeyOutcome)> deliver)
{
    // NB: The result goes back to the event loop of this thread, where the callbacks of the session live and die.
    auto thread = Threading::Thread::construct("Security key"sv,
        [session = move(session), &is_cancelled, request = move(request), deliver = move(deliver), event_loop = Core::EventLoop::current_weak()]() mutable -> intptr_t {
            auto outcome = request.visit([&](auto& request_with_pin) {
                auto outcome = run_request(request_with_pin, is_cancelled);
                // NB: The PIN does not stay in memory after its use.
                secure_zero(request_with_pin.pin.data(), request_with_pin.pin.size());
                return outcome;
            });
            auto strong_event_loop = event_loop->take();
            if (!strong_event_loop) {
                // NB: The process is on its way out, and the session must not die on this thread.
                (void)session.leak_ref();
                (void)new Function<void(SecurityKeyOutcome)>(move(deliver));
                return 0;
            }
            strong_event_loop->deferred_invoke([session = move(session), deliver = move(deliver), outcome = move(outcome)]() mutable {
                deliver(move(outcome));
            });
            return 0;
        });
    thread->start();
    thread->detach();
}

NonnullRefPtr<SecurityKeySession> SecurityKeySession::make_credential(SecurityKeyMakeCredentialRequest request, OnAttestation on_attestation)
{
    auto session = adopt_ref(*new SecurityKeySession);
    session->m_on_attestation = move(on_attestation);
    run_session(session, session->m_is_cancelled, move(request), [session = session.ptr()](SecurityKeyOutcome outcome) {
        // NB: The thread keeps the session alive until here.
        auto on_attestation = move(session->m_on_attestation);
        if (outcome.was_cancelled || session->m_is_cancelled.load())
            return;
        session->m_device_that_wants_pin = outcome.device_that_wants_pin;
        if (outcome.is_success)
            on_attestation(outcome.attestation.release_value());
        else
            on_attestation(outcome.error);
    });
    return session;
}

NonnullRefPtr<SecurityKeySession> SecurityKeySession::get_assertion(SecurityKeyGetAssertionRequest request, OnAssertions on_assertions)
{
    auto session = adopt_ref(*new SecurityKeySession);
    session->m_on_assertions = move(on_assertions);
    run_session(session, session->m_is_cancelled, move(request), [session = session.ptr()](SecurityKeyOutcome outcome) {
        // NB: The thread keeps the session alive until here.
        auto on_assertions = move(session->m_on_assertions);
        if (outcome.was_cancelled || session->m_is_cancelled.load())
            return;
        session->m_device_that_wants_pin = outcome.device_that_wants_pin;
        if (outcome.is_success)
            on_assertions(move(outcome.assertions));
        else
            on_assertions(outcome.error);
    });
    return session;
}

void SecurityKeySession::cancel()
{
    m_is_cancelled.store(true);
}

bool SecurityKeySession::is_supported()
{
#if defined(AK_OS_MACOS)
    return true;
#else
    return false;
#endif
}

bool SecurityKeySession::is_any_key_connected()
{
    return FidoHIDDevice::is_any_connected();
}

}
