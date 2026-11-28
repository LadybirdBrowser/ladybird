/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/PlatformAuthenticatorBackend.h>

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>

namespace WebView {

// The service that the passkeys of the browser are kept under in the keychain of the user. Each passkey is a generic
// password item, whose account is the base64url encoding of the credential id.
// FIXME: Keep each private key as a key of the keychain that cannot be exported, with an access control that ties its
//        use to the verification of the user, and in the Secure Enclave where there is one. The browser process then
//        signs through the keychain and never holds the bytes of a private key. This needs the data protection
//        keychain, which a build has only when it is signed with the entitlements for it.
static NSString* const s_keychain_service = @"Ladybird Passkeys";

static NSString* ns_string(StringView string)
{
    return [[NSString alloc] initWithBytes:string.characters_without_null_termination() length:string.length() encoding:NSUTF8StringEncoding];
}

static NSMutableDictionary* keychain_query(NSString* account)
{
    NSMutableDictionary* query = [NSMutableDictionary dictionary];
    query[(__bridge id)kSecClass] = (__bridge id)kSecClassGenericPassword;
    query[(__bridge id)kSecAttrService] = s_keychain_service;
    if (account)
        query[(__bridge id)kSecAttrAccount] = account;
    return query;
}

static ByteBuffer serialize(PublicKeyCredentialSource const& credential)
{
    JsonObject object;
    object.set("rpId"sv, credential.rp_id);
    object.set("privateKey"sv, MUST(encode_base64url(credential.private_key, AK::OmitPadding::Yes)));
    if (credential.user_handle.has_value())
        object.set("userHandle"sv, MUST(encode_base64url(*credential.user_handle, AK::OmitPadding::Yes)));
    object.set("isResidentCredential"sv, credential.is_resident_credential);
    object.set("signCount"sv, credential.sign_count.value_or(0));
    if (credential.user_name.has_value())
        object.set("userName"sv, credential.user_name->to_utf8());
    if (credential.user_display_name.has_value())
        object.set("userDisplayName"sv, credential.user_display_name->to_utf8());
    if (credential.creation_time.has_value())
        object.set("creationTime"sv, credential.creation_time->milliseconds_since_epoch());
    if (credential.last_used_time.has_value())
        object.set("lastUsedTime"sv, credential.last_used_time->milliseconds_since_epoch());
    auto serialized = object.serialized();
    return MUST(ByteBuffer::copy(serialized.bytes()));
}

static Optional<PublicKeyCredentialSource> deserialize(ByteBuffer credential_id, ReadonlyBytes data)
{
    auto value = JsonValue::from_string(StringView { data });
    if (value.is_error() || !value.value().is_object())
        return {};
    auto const& object = value.value().as_object();

    auto decode_member = [&](StringView key) -> Optional<ByteBuffer> {
        auto encoded = object.get_string(key);
        if (!encoded.has_value())
            return {};
        auto decoded = decode_base64url(*encoded);
        if (decoded.is_error())
            return {};
        return decoded.release_value();
    };
    auto utf16_member = [&](StringView key) -> Optional<Utf16String> {
        if (auto string = object.get_string(key); string.has_value())
            return Utf16String::from_utf8(*string);
        return {};
    };

    auto time_member = [&](StringView key) -> Optional<UnixDateTime> {
        if (auto milliseconds = object.get_i64(key); milliseconds.has_value())
            return UnixDateTime::from_milliseconds_since_epoch(*milliseconds);
        return {};
    };

    auto rp_id = object.get_string("rpId"sv);
    auto private_key = decode_member("privateKey"sv);
    if (!rp_id.has_value() || !private_key.has_value())
        return {};
    return PublicKeyCredentialSource {
        .id = move(credential_id),
        .private_key = private_key.release_value(),
        .rp_id = *rp_id,
        .user_handle = decode_member("userHandle"sv),
        .is_resident_credential = object.get_bool("isResidentCredential"sv).value_or(false),
        .sign_count = object.get_u32("signCount"sv).value_or(0),
        .backup_eligibility = false,
        .backup_state = false,
        .user_name = utf16_member("userName"sv),
        .user_display_name = utf16_member("userDisplayName"sv),
        .creation_time = time_member("creationTime"sv),
        .last_used_time = time_member("lastUsedTime"sv),
    };
}

class PlatformAuthenticatorBackendMacOS final : public PlatformAuthenticatorBackend {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual bool can_verify_user() override
    {
        LAContext* context = [[LAContext alloc] init];
        return [context canEvaluatePolicy:LAPolicyDeviceOwnerAuthentication error:nil];
    }

    virtual u64 verify_user(String const& action, Function<void(bool)> on_complete) override
    {
        // NB: The user answers one prompt at a time. A page in another tab does not get to take away the prompt that
        //     the user is looking at, so a new user verification fails while one is open.
        if (m_user_verification_context) {
            Core::EventLoop::current().deferred_invoke([on_complete = move(on_complete)] mutable {
                on_complete(false);
            });
            return 0;
        }
        LAContext* context = [[LAContext alloc] init];
        m_user_verification_context = context;
        auto user_verification_id = ++m_user_verification_id;
        __block auto held_on_complete = move(on_complete);
        __block auto event_loop = Core::EventLoop::current_weak();
        [context evaluatePolicy:LAPolicyDeviceOwnerAuthentication
                localizedReason:ns_string(action)
                          reply:^(BOOL success, NSError*) {
                              // NB: The reply comes on a queue of the framework, and the browser's state is touched
                              //     on its own thread only.
                              auto origin_event_loop = event_loop->take();
                              if (!origin_event_loop.is_alive())
                                  return;
                              bool is_verified = success;
                              origin_event_loop->deferred_invoke([this, user_verification_id, on_complete = move(held_on_complete), is_verified] mutable {
                                  if (m_user_verification_id == user_verification_id)
                                      m_user_verification_context = nil;
                                  on_complete(is_verified);
                              });
                          }];
        return user_verification_id;
    }

    virtual void cancel_user_verification(u64 user_verification_id) override
    {
        if (user_verification_id != m_user_verification_id)
            return;
        [m_user_verification_context invalidate];
        m_user_verification_context = nil;
    }

    virtual Optional<Vector<PublicKeyCredentialSource>> load_credentials() override
    {
        Vector<PublicKeyCredentialSource> credentials;

        // NB: The keychain gives the data of one item at a time, so this lists the accounts first.
        NSMutableDictionary* list_query = keychain_query(nil);
        list_query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitAll;
        list_query[(__bridge id)kSecReturnAttributes] = @YES;
        CFTypeRef list_result = nullptr;
        auto list_status = SecItemCopyMatching((__bridge CFDictionaryRef)list_query, &list_result);
        if (list_status == errSecItemNotFound)
            return credentials;
        if (list_status != errSecSuccess)
            return {};
        NSArray* items = (__bridge_transfer NSArray*)list_result;

        for (NSDictionary* item in items) {
            NSString* account = item[(__bridge id)kSecAttrAccount];
            if (!account)
                continue;
            auto credential_id = decode_base64url(StringView { [account UTF8String], strlen([account UTF8String]) });
            if (credential_id.is_error())
                continue;

            NSMutableDictionary* data_query = keychain_query(account);
            data_query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
            data_query[(__bridge id)kSecReturnData] = @YES;
            CFTypeRef data_result = nullptr;
            auto data_status = SecItemCopyMatching((__bridge CFDictionaryRef)data_query, &data_result);
            // NB: An item can go away between the two queries.
            if (data_status == errSecItemNotFound)
                continue;
            if (data_status != errSecSuccess)
                return {};
            NSData* data = (__bridge_transfer NSData*)data_result;
            if (auto credential = deserialize(credential_id.release_value(), { [data bytes], [data length] }); credential.has_value())
                credentials.append(credential.release_value());
        }
        return credentials;
    }

    virtual bool store_credential(PublicKeyCredentialSource const& credential) override
    {
        auto account = MUST(encode_base64url(credential.id, AK::OmitPadding::Yes));
        auto data = serialize(credential);
        auto label = credential.user_name.has_value() && !credential.user_name->is_empty()
            ? MUST(String::formatted("Ladybird passkey for {} ({})", credential.rp_id, credential.user_name->to_utf8()))
            : MUST(String::formatted("Ladybird passkey for {}", credential.rp_id));

        NSMutableDictionary* attributes = keychain_query(ns_string(account));
        attributes[(__bridge id)kSecAttrLabel] = ns_string(label);
        attributes[(__bridge id)kSecValueData] = [NSData dataWithBytes:data.data() length:data.size()];
        return SecItemAdd((__bridge CFDictionaryRef)attributes, nullptr) == errSecSuccess;
    }

    virtual void update_credential(PublicKeyCredentialSource const& credential) override
    {
        auto account = MUST(encode_base64url(credential.id, AK::OmitPadding::Yes));
        auto data = serialize(credential);
        NSDictionary* changes = @ { (__bridge id)kSecValueData : [NSData dataWithBytes:data.data() length:data.size()] };
        if (auto status = SecItemUpdate((__bridge CFDictionaryRef)keychain_query(ns_string(account)), (__bridge CFDictionaryRef)changes); status != errSecSuccess)
            warnln("Could not update a passkey in the keychain: {}", status);
    }

    virtual bool remove_credential(ReadonlyBytes credential_id) override
    {
        auto account = MUST(encode_base64url(credential_id, AK::OmitPadding::Yes));
        auto status = SecItemDelete((__bridge CFDictionaryRef)keychain_query(ns_string(account)));
        if (status == errSecSuccess || status == errSecItemNotFound)
            return true;
        warnln("Could not remove a passkey from the keychain: {}", status);
        return false;
    }

private:
    // The context of the user verification that is open, if any.
    LAContext* m_user_verification_context { nil };
    u64 m_user_verification_id { 0 };
};

OwnPtr<PlatformAuthenticatorBackend> PlatformAuthenticatorBackend::create()
{
    return make<PlatformAuthenticatorBackendMacOS>();
}

}
