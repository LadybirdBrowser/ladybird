/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(CredentialsContainer);

GC::Ref<CredentialsContainer> CredentialsContainer::create(JS::Realm& realm)
{
    return realm.create<CredentialsContainer>(realm);
}

CredentialsContainer::~CredentialsContainer() { }

// https://w3c.github.io/webappsec-credential-management/#algorithm-same-origin-with-ancestors
static bool is_same_origin_with_its_ancestors(HTML::EnvironmentSettingsObject& settings)
{
    auto& global = settings.global_object();

    // TODO: 1. If settings’s relevant global object has no associated Document, return false.
    // 2. Let document be settings’ relevant global object's associated Document.
    auto& document = as<HTML::Window>(global).associated_document();

    // 3. If document has no browsing context, return false.
    if (!document.browsing_context())
        return false;

    // 4. Let origin be settings’ origin.
    auto origin = settings.origin();

    // 5. Let navigable be document’s node navigable.
    auto navigable = document.navigable();

    // 6. While navigable has a non-null parent:
    while (navigable->parent()) {
        // 1. Set navigable to navigable’s parent.
        navigable = navigable->parent();

        // 2. If navigable’s active document's origin is not same origin with origin, return false.
        if (!origin.is_same_origin(navigable->active_document()->origin()))
            return false;
    }

    // 7. Return true.
    return true;
}

// https://w3c.github.io/webappsec-credential-management/#credentialrequestoptions-relevant-credential-interface-objects
template<typename OptionsType>
static Vector<CredentialInterface const*> relevant_credential_interface_objects(OptionsType const& options)
{
    // 1. Let settings be the current settings object.
    // 2. Let relevant interface objects be an empty set.
    Vector<CredentialInterface const*> interfaces;

    // 3. For each optionKey → optionValue of options:
    // NOTE: We cannot iterate like the spec says.
    //      1. Let credentialInterfaceObject be the Appropriate Interface Object (on settings’ global object) whose Options Member Identifier is optionKey.
    //      2. Assert: credentialInterfaceObject’s [[type]] slot equals the Credential Type whose Options Member Identifier is optionKey.
    //      3. Append credentialInterfaceObject to relevant interface objects.

#define APPEND_CREDENTIAL_INTERFACE_OBJECT(key, type_)                            \
    if (options.key.has_value()) {                                                \
        auto credential_interface_object = type_##Interface::the();               \
        VERIFY(credential_interface_object->options_member_identifier() == #key); \
        interfaces.append(credential_interface_object);                           \
    }

    // https://w3c.github.io/webappsec-credential-management/#credential-type-registry-appropriate-interface-object
    APPEND_CREDENTIAL_INTERFACE_OBJECT(password, PasswordCredential);
    APPEND_CREDENTIAL_INTERFACE_OBJECT(federated, FederatedCredential);
    // TODO: digital
    // TODO: identity
    // TODO: otp
    // TODO: publicKey

#undef APPEND_CREDENTIAL_INTERFACE_OBJECT

    // 4. Return relevant interface objects.
    return interfaces;
}

// https://w3c.github.io/webappsec-credential-management/#algorithm-collect-known
static JS::ThrowCompletionOr<Vector<GC::Ref<Credential>>> collect_credentials_from_store(JS::Realm& realm, URL::Origin const& origin, CredentialRequestOptions const& options, bool same_origin_with_ancestors)
{
    // 1. Let possible matches be an empty set.
    Vector<GC::Ref<Credential>> possible_matches;

    // 2. For each interface in options’ relevant credential interface objects:
    for (auto& interface : relevant_credential_interface_objects(options)) {
        // 1. Let r be the result of executing interface’s [[CollectFromCredentialStore]](origin, options, sameOriginWithAncestors)
        //    internal method on origin, options, and sameOriginWithAncestors. If that threw an exception, rethrow that exception.
        auto maybe_r = interface->collect_from_credential_store(realm, origin, options, same_origin_with_ancestors);
        if (maybe_r.is_error())
            return maybe_r.error();

        auto r = maybe_r.release_value();

        // TODO: 2. Assert: r is a list of interface objects.

        // 3. For each c in r:
        for (auto& c : r) {
            // 1. Append c to possible matches.
            possible_matches.append(c);
        }
    }

    // 3. Return possible matches.
    return possible_matches;
}

// https://w3c.github.io/webappsec-credential-management/#abstract-opdef-ask-the-user-to-choose-a-credential
static Variant<Empty, GC::Ref<Credential>, CredentialInterface*> ask_the_user_to_choose_a_credential(CredentialRequestOptions const&, Vector<GC::Ref<Credential>> const&)
{
    // TODO: This algorithm returns either null if the user chose not to share a credential with the site,
    //       a Credential object if the user chose a specific credential, or a Credential interface object
    //       if the user chose a type of credential.
    return {};
}

// https://w3c.github.io/webappsec-credential-management/#credentialrequestoptions-matchable-a-priori
static bool is_matchable_a_priori(CredentialRequestOptions const& options)
{
    // 1. For each interface in options’ relevant credential interface objects:
    for (auto& interface : relevant_credential_interface_objects(options)) {
        // 1. If interface’s [[discovery]] slot’s value is not "credential store", return false.
        if (interface->discovery() != "credential store")
            return false;
    }

    // 2. Return true.
    return true;
}

// https://w3c.github.io/webappsec-credential-management/#algorithm-request
GC::Ref<WebIDL::Promise> CredentialsContainer::get(CredentialRequestOptions const& options)
{
    // 1. Let settings be the current settings object.
    auto& settings = HTML::current_principal_settings_object();

    // 2. Assert: settings is a secure context.
    VERIFY(HTML::is_secure_context(settings));

    // 3. Let document be settings’s relevant global object's associated Document.
    auto& document = as<HTML::Window>(settings.global_object()).associated_document();

    // 4. If document is not fully active, then return a promise rejected with an "InvalidStateError" DOMException.
    if (!document.is_fully_active())
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::InvalidStateError::create(realm(), "Document is not fully active"_string));

    // 5. If options.signal is aborted, then return a promise rejected with options.signal’s abort reason.
    if (options.signal && options.signal->aborted())
        return WebIDL::create_rejected_promise(realm(), options.signal->reason());

    // 6. Let interfaces be options’s relevant credential interface objects.
    auto interfaces = relevant_credential_interface_objects(options);

    // 7. If interfaces is empty, then return a promise rejected with a "NotSupportedError" DOMException.
    if (interfaces.is_empty())
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::NotSupportedError::create(realm(), "No credential types"_string));

    // 8. For each interface of interfaces:
    for (auto& interface : interfaces) {
        // 1. If options.mediation is conditional and interface does not support conditional user mediation,
        //    return a promise rejected with a "TypeError" DOMException.
        if (options.mediation == Bindings::CredentialMediationRequirement::Conditional && !interface->supports_conditional_user_mediation())
            return WebIDL::create_rejected_promise(realm(), JS::TypeError::create(realm(), "Conditional user mediation is not supported"sv));

        // 2. If settings’ active credential types contains interface’s [[type]],
        //    return a promise rejected with a "NotAllowedError" DOMException.
        if (settings.active_credential_types().contains_slow(interface->type()))
            return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::NotAllowedError::create(realm(), "Credential type is not allowed"_string));

        // 3. Append interface’s [[type]] to settings’ active credential types.
        settings.active_credential_types().append(interface->type());
    }

    // 9. Let origin be settings’ origin.
    auto origin = settings.origin();

    // 10. Let sameOriginWithAncestors be true if settings is same-origin with its ancestors, and false otherwise.
    auto same_origin_with_ancestors = is_same_origin_with_its_ancestors(settings);

    // 11. For each interface in options’ relevant credential interface objects:
    for (auto& interface : interfaces) {
        // 1. Let permission be the interface’s [[type]] Get Permissions Policy.
        auto permission = interface->get_permission_policy();

        // 2. If permission is null, continue.
        if (!permission.has_value())
            continue;

        // TODO: 3. If document is not allowed to use permission, return a promise rejected with a "NotAllowedError" DOMException.
    }

    // 12. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm());

    // 13. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm().heap(), [this, promise = GC::Root(promise), origin, options = move(options), same_origin_with_ancestors, &settings] {
        HTML::TemporaryExecutionContext execution_context { realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let credentials be the result of collecting Credentials from the credential store, given origin, options, and sameOriginWithAncestors.
        auto maybe_credentials = collect_credentials_from_store(realm(), origin, options, same_origin_with_ancestors);

        // 2. If credentials is an exception, reject p with credentials.
        if (maybe_credentials.is_error()) {
            WebIDL::reject_promise(realm(), *promise, maybe_credentials.error_value());
            return;
        }

        auto credentials = maybe_credentials.release_value();

        // 3. If all of the following statements are true, resolve p with credentials[0] and skip the remaining steps:
        //      1. credentials’ size is 1
        //      TODO: 2. origin does not require user mediation
        //      3. options is matchable a priori.
        //      4. options.mediation is not "required".
        //      5. options.mediation is not "conditional".
        if (credentials.size() == 1
            && is_matchable_a_priori(options)
            && options.mediation != Bindings::CredentialMediationRequirement::Required
            && options.mediation != Bindings::CredentialMediationRequirement::Conditional) {
            WebIDL::resolve_promise(realm(), *promise, credentials[0]);
            return;
        }

        // 4. If options’ mediation is "silent", resolve p with null, and skip the remaining steps.
        if (options.mediation == Bindings::CredentialMediationRequirement::Silent) {
            WebIDL::resolve_promise(realm(), *promise, JS::js_null());
            return;
        }

        // 5. Let result be the result of asking the user to choose a Credential, given options and credentials.
        auto result = ask_the_user_to_choose_a_credential(options, credentials);

        // 6. If result is an interface object:
        if (result.has<CredentialInterface*>()) {
            // 1. Set result to the result of executing result’s [[DiscoverFromExternalSource]](origin, options, sameOriginWithAncestors),
            //    given origin, options, and sameOriginWithAncestors.
            auto maybe_result = result.get<CredentialInterface*>()->discover_from_external_source(realm(), origin, options, same_origin_with_ancestors);
            // If that threw an exception:
            if (maybe_result.is_error()) {
                // 1. Let e be the thrown exception.
                auto e = maybe_result.error_value();
                // 2. Queue a task on global’s DOM manipulation task source to run the following substeps:
                queue_global_task(HTML::Task::Source::DOMManipulation, settings.global_object(), GC::create_function(realm().heap(), [&] {
                    HTML::TemporaryExecutionContext execution_context { realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    // 1. Reject p with e.
                    WebIDL::reject_promise(realm(), *promise, e);
                }));
                // 3. Terminate these substeps.
                return;
            }
        }

        // 7. Assert: result is null, or a Credential.
        VERIFY(result.has<Empty>() || result.has<GC::Ref<Credential>>());

        // 8. If result is a Credential, resolve p with result.
        if (result.has<GC::Ref<Credential>>()) {
            WebIDL::resolve_promise(realm(), *promise, result.get<GC::Ref<Credential>>());
            return;
        }

        // 9. If result is null and options.mediation is not conditional, resolve p with result.
        if (result.has<Empty>() && options.mediation != Bindings::CredentialMediationRequirement::Conditional)
            WebIDL::resolve_promise(realm(), *promise, JS::js_null());
    }));

    // 14. React to p:
    auto on_completion = GC::create_function(realm().heap(), [&settings, interfaces = move(interfaces)](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. For each interface in interfaces:
        for (auto const& interface : interfaces) {
            // 1. Remove interface’s [[type]] from settings’ active credential types.
            settings.active_credential_types().remove_first_matching([&](auto& v) { return v == interface->type(); });
        }

        return JS::js_undefined();
    });
    WebIDL::react_to_promise(*promise, on_completion, on_completion);

    // 15. Return p.
    return promise;
}

// https://w3c.github.io/webappsec-credential-management/#algorithm-store
GC::Ref<WebIDL::Promise> CredentialsContainer::store(Credential const& credential)
{
    // 1. Let settings be the current settings object.
    auto& settings = HTML::current_principal_settings_object();

    // 2. Assert: settings is a secure context.
    VERIFY(HTML::is_secure_context(settings));

    // 3. If settings’s relevant global object's associated Document is not fully active,
    //    then return a promise rejected with an "InvalidStateError" DOMException.
    if (!as<HTML::Window>(settings.global_object()).associated_document().is_fully_active())
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::InvalidStateError::create(realm(), "Document is not fully active"_string));

    // 4. Let sameOriginWithAncestors be true if the current settings object is same-origin with its ancestors, and false otherwise.
    auto same_origin_with_ancestors = is_same_origin_with_its_ancestors(settings);

    // 5. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm());

    // 6. If settings’ active credential types contains credential’s [[type]], return a promise rejected with a "NotAllowedError" DOMException.
    if (settings.active_credential_types().contains_slow(credential.type()))
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::NotAllowedError::create(realm(), "Credential type is not allowed"_string));

    // 7. Append credential’s [[type]] to settings’ active credential types.
    settings.active_credential_types().append(credential.type());

    // 8. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm().heap(), [this, promise = GC::Root(promise), &settings, &credential, same_origin_with_ancestors] {
        HTML::TemporaryExecutionContext execution_context { realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Execute credential’s interface object's [[Store]](credential, sameOriginWithAncestors)
        //    internal method on credential and sameOriginWithAncestors.
        auto maybe_error = credential.interface()->store(realm(), same_origin_with_ancestors);
        // If that threw an exception:
        if (maybe_error.is_error()) {
            // 1. Let e be the thrown exception.
            auto e = maybe_error.error_value();
            // 2. Queue a task on global’s DOM manipulation task source to run the following substeps:
            queue_global_task(HTML::Task::Source::DOMManipulation, settings.global_object(), GC::create_function(realm().heap(), [&] {
                HTML::TemporaryExecutionContext execution_context { realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                // 1. Reject p with e.
                WebIDL::reject_promise(realm(), *promise, e);
            }));
        }
        // Otherwise, resolve p with undefined.
        else {
            WebIDL::resolve_promise(realm(), *promise, JS::js_undefined());
        }
    }));

    // 9. React to p:
    auto on_completion = GC::create_function(realm().heap(), [&settings, &credential](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Remove credential’s [[type]] from settings’ active credential types.
        settings.active_credential_types().remove_first_matching([&](auto& v) { return v == credential.type(); });

        return JS::js_undefined();
    });
    WebIDL::react_to_promise(*promise, on_completion, on_completion);

    // 10. Return p.
    return promise;
}

// https://w3c.github.io/webappsec-credential-management/#algorithm-create
GC::Ref<WebIDL::Promise> CredentialsContainer::create(CredentialCreationOptions const& options)
{
    // 1. Let settings be the current settings object.
    auto& settings = HTML::current_principal_settings_object();

    // 2. Assert: settings is a secure context.
    VERIFY(HTML::is_secure_context(settings));

    // 3. Let global be settings’ global object.
    auto& global = settings.global_object();

    // 4. Let document be the relevant global object's associated Document.
    auto& document = as<HTML::Window>(global).associated_document();

    // 5. If document is not fully active, then return a promise rejected with an "InvalidStateError" DOMException.
    if (!document.is_fully_active())
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::InvalidStateError::create(realm(), "Document is not fully active"_string));

    // 6. Let sameOriginWithAncestors be true if the current settings object is same-origin with its ancestors, and false otherwise.
    auto same_origin_with_ancestors = is_same_origin_with_its_ancestors(settings);

    // 7. Let interfaces be the set of options’ relevant credential interface objects.
    auto interfaces = relevant_credential_interface_objects(options);

    // 8. Return a promise rejected with NotSupportedError if any of the following statements are true:
    //    TODO: 1. global does not have an associated Document.
    //    2. interfaces’ size is greater than 1.
    if (interfaces.size() > 1)
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::NotSupportedError::create(realm(), "Too many crendetial types"_string));

    // 9. For each interface in interfaces:
    for (auto& interface : interfaces) {
        // 1. Let permission be the interface’s [[type]] Create Permissions Policy.
        auto permission = interface->create_permission_policy();

        // 2. If permission is null, continue.
        if (!permission.has_value())
            continue;

        // TODO: 3. If document is not allowed to use permission, return a promise rejected with a "NotAllowedError" DOMException.
    }

    // 10. If options.signal is aborted, then return a promise rejected with options.signal’s abort reason.
    if (options.signal && options.signal->aborted())
        return WebIDL::create_rejected_promise(realm(), options.signal->reason());

    // NOTE: The spec does not mention this check
    if (interfaces.size() < 1)
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::NotSupportedError::create(realm(), "No credential types"_string));

    // 11. Let type be interfaces[0]'s [[type]].
    auto type = interfaces[0]->type();

    // 12. If settings’ active credential types contains type, return a promise rejected with a "NotAllowedError" DOMException.
    if (settings.active_credential_types().contains_slow(type))
        return WebIDL::create_rejected_promise_from_exception(realm(), WebIDL::NotAllowedError::create(realm(), "Credential type is not allowed"_string));

    // 13. Append type to settings’ active credential types.
    settings.active_credential_types().append(type);

    // 14. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 15. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm());

    // 16. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm().heap(), [this, promise = GC::Root(promise), &global, interfaces = move(interfaces), origin, options = move(options), same_origin_with_ancestors] {
        HTML::TemporaryExecutionContext execution_context { realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let r be the result of executing interfaces[0]'s [[Create]](origin, options, sameOriginWithAncestors)
        //    internal method on origin, options, and sameOriginWithAncestors.
        auto maybe_r = interfaces[0]->create(realm(), origin, options, same_origin_with_ancestors);
        // If that threw an exception:
        if (maybe_r.is_error()) {
            // 1. Let e be the thrown exception.
            auto e = maybe_r.error_value();
            // 2. Queue a task on global’s DOM manipulation task source to run the following substeps:
            queue_global_task(HTML::Task::Source::DOMManipulation, global, GC::create_function(realm().heap(), [&] {
                HTML::TemporaryExecutionContext execution_context { realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. Reject p with e.
                WebIDL::reject_promise(realm(), *promise, e);
            }));
            // 3. Terminate these substeps.
            return;
        }

        auto r = maybe_r.release_value();

        // 2. If r is a Credential or null, resolve p with r, and terminate these substeps.
        if (r.has<Empty>()) {
            WebIDL::resolve_promise(realm(), *promise, JS::js_null());
            return;
        }
        if (r.has<GC::Ref<Credential>>()) {
            auto& credential = r.get<GC::Ref<Credential>>();
            WebIDL::resolve_promise(realm(), *promise, credential);
            return;
        }

        // 3. Assert: r is an algorithm (as defined in §2.2.1.4 [[Create]] internal method).
        VERIFY(r.has<GC::Ref<CreateCredentialAlgorithm>>());

        // 4. Queue a task on global’s DOM manipulation task source to run the following substeps:
        auto& r_algo = r.get<GC::Ref<CreateCredentialAlgorithm>>();
        queue_global_task(HTML::Task::Source::DOMManipulation, global, GC::create_function(realm().heap(), [this, &global, promise = GC::Root(promise), r_algo = GC::Root(r_algo)] {
            HTML::TemporaryExecutionContext execution_context { realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. Resolve p with the result of promise-calling r given global.
            auto maybe_result = r_algo->function()(global);
            if (maybe_result.is_error()) {
                WebIDL::reject_promise(realm(), *promise, maybe_result.error_value());
                return;
            }

            auto& result = maybe_result.value();
            WebIDL::resolve_promise(realm(), *promise, result);
        }));
    }));

    // 17. React to p:
    auto on_completion = GC::create_function(realm().heap(), [&settings, type](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Remove type from settings’ active credential types.
        settings.active_credential_types().remove_first_matching([&](auto& v) { return v == type; });

        return JS::js_undefined();
    });
    WebIDL::react_to_promise(*promise, on_completion, on_completion);

    // 18. Return p.
    return promise;
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-preventsilentaccess
GC::Ref<WebIDL::Promise> CredentialsContainer::prevent_silent_access()
{
    auto* realm = vm().current_realm();
    return WebIDL::create_rejected_promise_from_exception(*realm, vm().throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "prevent silent access"sv));
}

CredentialsContainer::CredentialsContainer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void CredentialsContainer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CredentialsContainer);
    Base::initialize(realm);
}

}
