/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIDL/ExposedTo.h>

static ByteString s_error_string;

namespace IDL {

ErrorOr<ExposedTo> parse_exposure_set(StringView interface_name, StringView exposed)
{
    // NOTE: This roughly follows the definitions of https://webidl.spec.whatwg.org/#Exposed
    //       It does not remotely interpret all the abstract operations therein though.

    auto exposed_trimmed = exposed.trim_whitespace();
    if (exposed_trimmed == "*"sv)
        return ExposedTo::All;
    if (exposed_trimmed == "Nobody"sv)
        return ExposedTo::Nobody;
    if (exposed_trimmed == "Window"sv)
        return ExposedTo::Window;
    if (exposed_trimmed == "Worker"sv)
        return ExposedTo::AllWorkers;
    if (exposed_trimmed == "DedicatedWorker"sv)
        return ExposedTo::DedicatedWorker;
    if (exposed_trimmed == "SharedWorker"sv)
        return ExposedTo::SharedWorker;
    if (exposed_trimmed == "ServiceWorker"sv)
        return ExposedTo::ServiceWorker;
    if (exposed_trimmed == "AudioWorklet"sv)
        return ExposedTo::AudioWorklet;
    if (exposed_trimmed == "Worklet"sv)
        return ExposedTo::Worklet;
    if (exposed_trimmed == "ShadowRealm"sv)
        return ExposedTo::ShadowRealm;

    if (exposed_trimmed[0] == '(') {
        ExposedTo whom = Nobody;
        for (StringView candidate : exposed_trimmed.substring_view(1, exposed_trimmed.length() - 1).split_view(',')) {
            candidate = candidate.trim_whitespace();
            if (candidate == "Window"sv) {
                whom |= ExposedTo::Window;
            } else if (candidate == "Worker"sv) {
                whom |= ExposedTo::AllWorkers;
            } else if (candidate == "DedicatedWorker"sv) {
                whom |= ExposedTo::DedicatedWorker;
            } else if (candidate == "SharedWorker"sv) {
                whom |= ExposedTo::SharedWorker;
            } else if (candidate == "ServiceWorker"sv) {
                whom |= ExposedTo::ServiceWorker;
            } else if (candidate == "AudioWorklet"sv) {
                whom |= ExposedTo::AudioWorklet;
            } else if (candidate == "Worklet"sv) {
                whom |= ExposedTo::Worklet;
            } else if (candidate == "ShadowRealm"sv) {
                whom |= ExposedTo::ShadowRealm;
            } else {
                s_error_string = ByteString::formatted("Unknown Exposed attribute candidate {} in {} in {}", candidate, exposed_trimmed, interface_name);
                return Error::from_string_view(s_error_string.view());
            }
        }
        if (whom == ExposedTo::Nobody) {
            s_error_string = ByteString::formatted("Unknown Exposed attribute {} in {}", exposed_trimmed, interface_name);
            return Error::from_string_view(s_error_string.view());
        }
        return whom;
    }

    s_error_string = ByteString::formatted("Unknown Exposed attribute {} in {}", exposed_trimmed, interface_name);
    return Error::from_string_view(s_error_string.view());
}

}
