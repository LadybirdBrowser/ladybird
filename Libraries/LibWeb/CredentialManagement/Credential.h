/*
 * Copyright (c) 2024, Saksham Goyal <sakgoy2001@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Runtime/Promise.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {
struct CredentialData {
    String id;
};
struct FederatedCredentialRequestOptions {
    Vector<String> providers;
    Vector<String> protocols;
};

struct FederatedCredentialInit : CredentialData {
    // FIXME optional or not?
    Optional<String> name;
    Optional<String> icon_url;
    Optional<String> origin;
    Optional<String> provider;
    Optional<String> protocol;
};

enum class CredentialMediationRequirement {
    Silent,
    Optional,
    Conditional,
    Required,
};

struct CredentialRequestOptions {
    CredentialMediationRequirement mediation = CredentialMediationRequirement::Optional;
    JS::GCPtr<DOM::AbortSignal> signal;

    union {
        JS::GCPtr<FederatedCredentialRequestOptions> federated;
    };
};
struct CredentialCreationOptions {
    CredentialMediationRequirement mediation = CredentialMediationRequirement::Optional;
    JS::GCPtr<DOM::AbortSignal> signal;

    union {
        JS::GCPtr<FederatedCredentialInit> federated;
        // PasswordCredentialInit password;
    };
};

struct PasswordCredentialData : CredentialData {
    String name;
    String icon_url;
    String origin;
    String password;
};

class Credential : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Credential, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Credential);

public:
    virtual ~Credential() override = default;
    static JS::NonnullGCPtr<WebIDL::Promise> is_conditional_mediation_available(JS::VM&);
    static JS::NonnullGCPtr<WebIDL::Promise> will_request_conditional_creation(JS::VM&);
    String id() const { return m_id; }
    String type() const { return m_type; }

protected:
    virtual void initialize(JS::Realm& realm) override;
    Credential(JS::Realm& realm);

private:
    String m_id;
    String m_type;
};
}
namespace Web::CredentialManagement {

class FederatedCredential : public Credential {
    WEB_PLATFORM_OBJECT(FederatedCredential, Credential);
    JS_DECLARE_ALLOCATOR(FederatedCredential);

public:
    virtual ~FederatedCredential() override = default;

    FederatedCredential(JS::Realm&, FederatedCredentialInit&);
    static JS::NonnullGCPtr<FederatedCredential> create(JS::Realm&, FederatedCredentialInit&);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<FederatedCredential>> construct_impl(JS::Realm&, FederatedCredentialInit&);

    String id() const { return m_id; }
    String type() const { return m_type; }
    String name() const { return m_name; }
    String provider() const { return m_provider; }
    String icon_url() const { return m_icon_url; }
    Optional<String> protocol() const { return m_protocol; }

    static JS::NonnullGCPtr<WebIDL::Promise> will_request_conditional_creation(JS::VM&);
    static JS::NonnullGCPtr<WebIDL::Promise> is_conditional_mediation_available(JS::VM&);

private:
    FederatedCredential(JS::Realm& realm);

    virtual void initialize(JS::Realm& realm) override;

    String m_provider;
    Optional<String> m_protocol;
    String m_id;
    String m_type;

    // Mixin CredentialUserData
    String m_name;
    String m_icon_url;
};
}
