/*
 * Copyright (c) 2024, Miguel Sacristán <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CredentialsContainer.h"

#include <AK/Vector.h>
#include <LibWeb/Credentials/Credential.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Credentials {
GC_DEFINE_ALLOCATOR(CredentialsContainer);

WebIDL::ExceptionOr<GC::Ref<CredentialsContainer>> CredentialsContainer::construct_impl(JS::Realm& realm)
{
    return realm.create<CredentialsContainer>(realm);
}

// https://www.w3.org/TR/credential-management-1/#abstract-opdef-request-a-credential
GC::Ref<WebIDL::Promise> CredentialsContainer::get(CredentialRequestOptions const& options)
{
    auto& realm = this->realm();

    // 1. Let settings be the current settings object.
    auto& settings = HTML::current_principal_settings_object();

    // 2. Assert: settings is a secure context.
    VERIFY(HTML::is_secure_context(settings));

    // 3. Let document be settings’s relevant global object's associated Document.
    auto const& document = verify_cast<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 4. If document is not fully active, then return a promise rejected with an "InvalidStateError" DOMException.
    if (!document.is_fully_active()) {
        return create_rejected_promise_from_exception(realm, WebIDL::InvalidStateError::create(realm, "Document is not fully active yet"_string));
    }

    // 5. If options.signal is aborted, then return a promise rejected with options.signal’s abort reason.
    if (options.signal && options.signal->aborted()) {
        return WebIDL::create_rejected_promise(realm, options.signal->reason());
    }

    // 6. Let interfaces be option’s relevant credential interface objects.
    auto const interfaces = relevant_credential_interface_objects(options);

    // 7. If interfaces is empty, then return a promise rejected with a "NotSupportedError" DOMException.
    if (interfaces.is_empty()) {
        return create_rejected_promise_from_exception(realm, WebIDL::DOMException::create(realm, "NotSupportedError"_string, "No credential type is supported"_string));
    }

    // FIXME
    // 8. For each interface of interfaces:
    {
        // FIXME
        // 1. If options.mediation is conditional and interface does not support conditional user mediation, return a promise rejected with a "TypeError" DOMException.
        if (options.mediation == Bindings::CredentialMediationRequirement::Conditional) {
            return create_rejected_promise_from_exception(realm, WebIDL::DOMException::create(realm, "TypeError"_string, "Interface does not support user mediation"_string));
        }

        // FIXME
        // 2. If settings’ active credential types contains interface’s [[type]], return a promise rejected with a "NotAllowedError" DOMException.
        // 3. Append interface’s [[type]] to settings’ active credential types.
    }

    // 9. Let origin be settings’ origin.
    auto const& origin = settings.origin();

    // FIXME
    // 10. Let sameOriginWithAncestors be true if settings is same-origin with its ancestors, and false otherwise.

    // FIXME
    // 11. For each interface in options’ relevant credential interface objects:
    {
        // 1. Let permission be the interface’s [[type]] Get Permissions Policy.
        // 2. If permission is null, continue.
        // 3. If document is not allowed to use permission, return a promise rejected with a "NotAllowedError" DOMException.
    }

    // 12. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // FIXME
    // 13. Run the following steps in parallel:

    // FIXME
    // 14. React to p

    // 15. Return p
    return p;
}

CredentialsContainer::CredentialsContainer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void CredentialsContainer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CredentialsContainer);
}

// https://www.w3.org/TR/credential-management-1/#credentialrequestoptions-dictionary
HashTable<Credential> relevant_credential_interface_objects(Variant<CredentialCreationOptions, CredentialRequestOptions>)
{
    // FIXME
    // 1. Let settings be the current settings object.

    // 2. Let relevant interface objects be an empty set.
    HashTable<Credential> relevant_interface_objects;

    // FIXME
    // 3. For each optionKey → optionValue of options:
    {
        // FIXME
        // 1. Let credentialInterfaceObject be the Appropriate Interface Object(on settings’ global object) whose Options Member Identifier is optionKey.
        // 2. Assert : credentialInterfaceObject’ s [[type]] slot equals the Credential Type whose Options Member Identifier is optionKey.
        // 3. Append credentialInterfaceObject to relevant interface objects.
    }

    // 4. Return relevant interface objects.
    return relevant_interface_objects;
}
}
