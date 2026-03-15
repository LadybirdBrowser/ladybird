/*
 * Copyright (c) 2026, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CredentialPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {

typedef GC::Function<WebIDL::ExceptionOr<GC::Ref<Credential>>(JS::Object const&)> CreateCredentialAlgorithm;

#define CREDENTIAL_INTERFACE(class_name) \
public:                                  \
    static class_name const& the()       \
    {                                    \
        static class_name s_instance;    \
        return s_instance;               \
    }

// https://www.w3.org/TR/credential-management-1/#credential-internal-methods
class CredentialInterface {
    AK_MAKE_NONCOPYABLE(CredentialInterface);
    AK_MAKE_NONMOVABLE(CredentialInterface);

protected:
    CredentialInterface() { }

public:
    virtual ~CredentialInterface() = default;

    // https://w3c.github.io/webappsec-credential-management/#credential-type-registry-credential-type
    virtual String type() const = 0;

    // https://w3c.github.io/webappsec-credential-management/#credential-type-registry-options-member-identifier
    virtual String options_member_identifier() const = 0;

    // https://w3c.github.io/webappsec-credential-management/#credential-type-registry-get-permissions-policy
    virtual Optional<String> get_permission_policy() const = 0;

    // https://w3c.github.io/webappsec-credential-management/#credential-type-registry-create-permissions-policy
    virtual Optional<String> create_permission_policy() const = 0;

    // https://w3c.github.io/webappsec-credential-management/#dom-credential-discovery-slot
    virtual String discovery() const = 0;

    // NOTE: This is not explicitly present in the spec, it is inferred.
    virtual bool supports_conditional_user_mediation() const = 0;

    // https://w3c.github.io/webappsec-credential-management/#algorithm-create-cred
    virtual WebIDL::ExceptionOr<Variant<Empty, GC::Ref<Credential>, GC::Ref<CreateCredentialAlgorithm>>> create(JS::Realm&, URL::Origin const&, CredentialCreationOptions const&, bool) const;

    // https://w3c.github.io/webappsec-credential-management/#algorithm-store-cred
    virtual WebIDL::ExceptionOr<void> store(JS::Realm& realm, bool) const;

    // https://w3c.github.io/webappsec-credential-management/#algorithm-discover-creds
    virtual WebIDL::ExceptionOr<Variant<Empty, GC::Ref<Credential>>> discover_from_external_source(JS::Realm&, URL::Origin const&, CredentialRequestOptions const&, bool) const;

    // https://w3c.github.io/webappsec-credential-management/#algorithm-collect-creds
    virtual WebIDL::ExceptionOr<Vector<Credential>> collect_from_credential_store(JS::Realm&, URL::Origin const&, CredentialRequestOptions const&, bool) const;
};

// https://www.w3.org/TR/credential-management-1/#credential
class Credential : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Credential, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Credential);

public:
    static GC::Ref<WebIDL::Promise> is_conditional_mediation_available(JS::VM&);

    virtual ~Credential() override;

    String const& id() const { return m_id; }

    virtual String type() const = 0;
    virtual CredentialInterface const& interface() const = 0;

protected:
    explicit Credential(JS::Realm&);
    Credential(JS::Realm&, String id);
    virtual void initialize(JS::Realm&) override;

    String m_id;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-credentialdata
struct CredentialData {
    String id;
};

}
