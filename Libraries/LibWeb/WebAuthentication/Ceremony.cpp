/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/WebAuthentication/AuthenticatorMessages.h>

// What the Web Authentication code in Bindings/Rust/src/web_authentication asks the page for: The browser process
// holds the authenticators and builds the client data, so a ceremony is a request to it.

namespace Web::WebAuthentication {

namespace {

// NB: These mirror the structs of the same names in Bindings/Rust/src/web_authentication/ceremony.rs.
struct MakeCredentialRequestAbi {
    ReadonlyBytes challenge;
    ReadonlyBytes rp_id;
    FlatPtr rp_name;
    ReadonlyBytes user_id;
    FlatPtr user_name;
    FlatPtr user_display_name;
    ReadonlySpan<i32> public_key_algorithms;
    ReadonlySpan<ReadonlyBytes> exclude_credential_ids;
    // NB: Zero is no attachment, and then the values of AuthenticatorAttachment follow.
    u8 authenticator_attachment;
    u8 resident_key;
    u8 user_verification;
    bool same_origin_with_ancestors;
    bool has_timeout;
    u32 timeout;
};

struct MakeCredentialResponseAbi {
    ReadonlyBytes client_data_json;
    ReadonlyBytes credential_id;
    ReadonlyBytes attestation_object;
    ReadonlyBytes authenticator_data;
    ReadonlyBytes public_key;
    ReadonlySpan<ReadonlyBytes> transports;
    i32 public_key_algorithm;
    bool has_public_key;
    u8 authenticator_attachment;
    bool is_resident_key;
};

struct GetAssertionRequestAbi {
    ReadonlyBytes challenge;
    ReadonlyBytes rp_id;
    ReadonlySpan<ReadonlyBytes> allow_credential_ids;
    u8 user_verification;
    bool same_origin_with_ancestors;
    bool is_conditional;
    bool has_timeout;
    u32 timeout;
};

struct GetAssertionResponseAbi {
    ReadonlyBytes client_data_json;
    ReadonlyBytes credential_id;
    ReadonlyBytes authenticator_data;
    ReadonlyBytes signature;
    ReadonlyBytes user_handle;
    bool has_user_handle;
    u8 authenticator_attachment;
};

}

static_assert(to_underlying(AuthenticatorAttachment::Platform) == 0);
static_assert(to_underlying(AuthenticatorAttachment::CrossPlatform) == 1);
static_assert(to_underlying(ResidentKeyRequirement::Discouraged) == 0);
static_assert(to_underlying(ResidentKeyRequirement::Preferred) == 1);
static_assert(to_underlying(ResidentKeyRequirement::Required) == 2);
static_assert(to_underlying(UserVerificationRequirement::Required) == 0);
static_assert(to_underlying(UserVerificationRequirement::Preferred) == 1);
static_assert(to_underlying(UserVerificationRequirement::Discouraged) == 2);
static_assert(to_underlying(AuthenticatorError::NotAllowed) == 0);
static_assert(to_underlying(AuthenticatorError::InvalidState) == 1);
static_assert(to_underlying(AuthenticatorError::Security) == 2);
static_assert(to_underlying(AuthenticatorError::NotSupported) == 3);
static_assert(to_underlying(AuthenticatorError::Constraint) == 4);

extern "C" {
// Called from Bindings/Rust/src/web_authentication/ceremony.rs.
bool web_authentication_is_same_origin_with_its_ancestors(DOM::Document*);
bool web_authentication_is_user_verifying_platform_authenticator_available(HTML::Window*);
void web_authentication_cancel_ceremony(HTML::Window*, u64 request_id);
u64 web_authentication_make_credential(HTML::Window*, MakeCredentialRequestAbi const*, GC::Cell*, void (*steps)(JSVM*, void*, MakeCredentialResponseAbi const*, u8 error));
u64 web_authentication_get_assertion(HTML::Window*, GetAssertionRequestAbi const*, GC::Cell*, void (*steps)(JSVM*, void*, GetAssertionResponseAbi const*, u8 error));
}

static Vector<ByteBuffer> copy_of(ReadonlySpan<ReadonlyBytes> list)
{
    Vector<ByteBuffer> copies;
    copies.ensure_capacity(list.size());
    for (auto bytes : list)
        copies.unchecked_append(MUST(ByteBuffer::copy(bytes)));
    return copies;
}

// https://w3c.github.io/webappsec-credential-management/#same-origin-with-its-ancestors
bool web_authentication_is_same_origin_with_its_ancestors(DOM::Document* document)
{
    auto navigable = document->navigable();
    if (!navigable)
        return false;
    // 3. For each navigable of document's ancestor navigables: if navigable's active document's origin is not same
    //    origin with origin, return false.
    // NB: An ancestor can be a navigable of another process, whose origin the browser process tells this one.
    for (GC::Ptr<HTML::Navigable> ancestor = navigable->parent(); ancestor; ancestor = ancestor->parent()) {
        auto ancestor_origin = ancestor->active_document_origin();
        if (!ancestor_origin.has_value() || !ancestor_origin->is_same_origin(document->origin()))
            return false;
    }
    return true;
}

bool web_authentication_is_user_verifying_platform_authenticator_available(HTML::Window* window)
{
    return window->page().client().page_did_request_is_user_verifying_platform_authenticator_available();
}

void web_authentication_cancel_ceremony(HTML::Window* window, u64 request_id)
{
    window->page().cancel_web_authentication_request(request_id);
}

u64 web_authentication_make_credential(HTML::Window* window, MakeCredentialRequestAbi const* abi, GC::Cell* cell, void (*steps)(JSVM*, void*, MakeCredentialResponseAbi const*, u8 error))
{
    MakeCredentialRequest request {
        .challenge = MUST(ByteBuffer::copy(abi->challenge)),
        .rp_id = MUST(String::from_utf8(StringView { abi->rp_id })),
        .rp_name = Utf16String::from_raw(abi->rp_name),
        .user_id = MUST(ByteBuffer::copy(abi->user_id)),
        .user_name = Utf16String::from_raw(abi->user_name),
        .user_display_name = Utf16String::from_raw(abi->user_display_name),
        .public_key_algorithms = {},
        .exclude_credential_ids = copy_of(abi->exclude_credential_ids),
        .authenticator_attachment = abi->authenticator_attachment == 0 ? Optional<AuthenticatorAttachment> {} : static_cast<AuthenticatorAttachment>(abi->authenticator_attachment - 1),
        .resident_key = static_cast<ResidentKeyRequirement>(abi->resident_key),
        .user_verification = static_cast<UserVerificationRequirement>(abi->user_verification),
        .same_origin_with_ancestors = abi->same_origin_with_ancestors,
        .timeout = abi->has_timeout ? Optional<u32> { abi->timeout } : Optional<u32> {},
    };
    request.public_key_algorithms.append(abi->public_key_algorithms.data(), abi->public_key_algorithms.size());

    auto callback = GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *cell }, steps), [](auto& cell, auto& steps, MakeCredentialResult const& result) {
        auto* vm = JS::HostABI::vm_to_abi(JS::VM::the());
        if (auto const* error = result.template get_pointer<AuthenticatorError>()) {
            steps(vm, cell.ptr(), nullptr, to_underlying(*error));
            return;
        }
        auto const& response = result.template get<MakeCredentialResponse>();
        Vector<ReadonlyBytes> transports;
        for (auto const& transport : response.transports)
            transports.append(transport.bytes());
        MakeCredentialResponseAbi abi {
            .client_data_json = response.client_data_json,
            .credential_id = response.credential_id,
            .attestation_object = response.attestation_object,
            .authenticator_data = response.authenticator_data,
            .public_key = response.public_key.has_value() ? response.public_key->bytes() : ReadonlyBytes {},
            .transports = transports,
            .public_key_algorithm = response.public_key_algorithm,
            .has_public_key = response.public_key.has_value(),
            .authenticator_attachment = to_underlying(response.authenticator_attachment),
            .is_resident_key = response.is_resident_key,
        };
        steps(vm, cell.ptr(), &abi, 0);
    });
    return window->page().make_credential(window->associated_document().id(), request, callback);
}

u64 web_authentication_get_assertion(HTML::Window* window, GetAssertionRequestAbi const* abi, GC::Cell* cell, void (*steps)(JSVM*, void*, GetAssertionResponseAbi const*, u8 error))
{
    GetAssertionRequest request {
        .challenge = MUST(ByteBuffer::copy(abi->challenge)),
        .rp_id = MUST(String::from_utf8(StringView { abi->rp_id })),
        .allow_credential_ids = copy_of(abi->allow_credential_ids),
        .user_verification = static_cast<UserVerificationRequirement>(abi->user_verification),
        .same_origin_with_ancestors = abi->same_origin_with_ancestors,
        .timeout = abi->has_timeout ? Optional<u32> { abi->timeout } : Optional<u32> {},
        .is_conditional = abi->is_conditional,
    };

    auto callback = GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *cell }, steps), [](auto& cell, auto& steps, GetAssertionResult const& result) {
        auto* vm = JS::HostABI::vm_to_abi(JS::VM::the());
        if (auto const* error = result.template get_pointer<AuthenticatorError>()) {
            steps(vm, cell.ptr(), nullptr, to_underlying(*error));
            return;
        }
        auto const& response = result.template get<GetAssertionResponse>();
        GetAssertionResponseAbi abi {
            .client_data_json = response.client_data_json,
            .credential_id = response.credential_id,
            .authenticator_data = response.authenticator_data,
            .signature = response.signature,
            .user_handle = response.user_handle.has_value() ? response.user_handle->bytes() : ReadonlyBytes {},
            .has_user_handle = response.user_handle.has_value(),
            .authenticator_attachment = to_underlying(response.authenticator_attachment),
        };
        steps(vm, cell.ptr(), &abi, 0);
    });
    auto& document = window->associated_document();
    auto request_id = window->page().get_assertion(document.id(), request, callback);
    if (request.is_conditional) {
        if (auto* input = as_if<HTML::HTMLInputElement>(document.focused_area().ptr()))
            input->conditional_web_authentication_request_started();
        else if (auto* textarea = as_if<HTML::HTMLTextAreaElement>(document.focused_area().ptr()))
            textarea->conditional_web_authentication_request_started();
    }
    return request_id;
}

}
