/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/EncryptedMediaExtensions/Algorithms.h>
#include <LibWeb/EncryptedMediaExtensions/MediaKeySystemAccess.h>
#include <LibWeb/EncryptedMediaExtensions/NavigatorEncryptedMediaExtensionsPartial.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::EncryptedMediaExtensions {

// https://w3c.github.io/encrypted-media/#dom-navigator-requestmediakeysystemaccess
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> NavigatorEncryptedMediaExtensionsPartial::request_media_key_system_access(Utf16String const& key_system, Vector<Bindings::MediaKeySystemConfiguration> const& supported_configurations)
{
    auto& navigator = as<HTML::Navigator>(*this);
    auto& realm = navigator.realm();

    // 1. If this's relevant global object's associated Document is not allowed to use the encrypted-media feature, then throw a "SecurityError" DOMException and abort these steps.
    auto& associated_document = as<HTML::Window>(HTML::relevant_global_object(navigator)).associated_document();
    if (!associated_document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::EncryptedMedia))
        return WebIDL::SecurityError::create(realm, "This document is not allowed to use the encrypted-media feature"_utf16);

    // 2. If keySystem is the empty string, return a promise rejected with a newly created TypeError.
    if (key_system.is_empty())
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "keySystem must not be empty"_utf16));

    // 3. If supportedConfigurations is empty, return a promise rejected with a newly created TypeError.
    if (supported_configurations.is_empty())
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "supportedConfigurations must not be empty"_utf16));

    // 4. Let document be the calling context's Document.
    // FIXME: Is this the same as the associated document?
    auto& document = associated_document;

    // 5. Let origin be the origin of document.
    auto origin = document.origin();

    // 6. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 7. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, key_system, supported_configurations, origin, promise]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 1. If keySystem is not one of the Key Systems supported by the user agent, reject promise with a NotSupportedError. String comparison is case-sensitive.
        if (!is_supported_key_system(key_system))
            return WebIDL::reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "Unsupported key system"_utf16));

        // 2. Let implementation be the implementation of keySystem.
        auto implementation = key_system_from_string(key_system);

        // 3. For each value in supportedConfigurations:
        for (auto const& candidate_configuration : supported_configurations) {
            // 1. Let candidate configuration be the value.

            // 2. Let supported configuration be the result of executing the Get Supported Configuration algorithm on implementation, candidate configuration, and origin.
            auto supported_configuration = get_supported_configuration(*implementation, candidate_configuration, origin);

            // 3. If supported configuration is not NotSupported, run the following steps:
            if (supported_configuration.has_value()) {
                // 1. Let access be a new MediaKeySystemAccess object, and initialize it as follows:
                // 1. Set the keySystem attribute to keySystem.
                // 2. Let the configuration value be supported configuration.
                // 3. Let the cdm implementation value be implementation.
                auto access = MediaKeySystemAccess::create(realm, key_system, *supported_configuration->configuration, move(implementation));

                // 2. Resolve promise with access and abort the parallel steps of this algorithm.
                return WebIDL::resolve_promise(realm, promise, access);
            }
        }

        // 4. Reject promise with a NotSupportedError.
        WebIDL::reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "No supported configurations found for the requested key system"_utf16));
    }));

    // 8. Return promise.
    return promise;
}

}
