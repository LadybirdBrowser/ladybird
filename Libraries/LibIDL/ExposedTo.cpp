/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Vector.h>
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

    auto exposed_from_string = [](auto& string) -> Optional<ExposedTo> {
        if (string == "Window"sv)
            return ExposedTo::Window;
        if (string == "Worker"sv)
            return ExposedTo::AllWorkers;
        if (string == "DedicatedWorker"sv)
            return ExposedTo::DedicatedWorker;
        if (string == "SharedWorker"sv)
            return ExposedTo::SharedWorker;
        if (string == "ServiceWorker"sv)
            return ExposedTo::ServiceWorker;
        if (string == "AudioWorklet"sv)
            return ExposedTo::AudioWorklet;
        if (string == "LayoutWorklet"sv)
            return ExposedTo::LayoutWorklet;
        if (string == "PaintWorklet"sv)
            return ExposedTo::PaintWorklet;
        if (string == "Worklet"sv)
            return ExposedTo::Worklet;
        if (string == "ShadowRealm"sv)
            return ExposedTo::ShadowRealm;
        return {};
    };
    if (auto parsed_exposed = exposed_from_string(exposed_trimmed); parsed_exposed.has_value())
        return parsed_exposed.value();

    if (exposed_trimmed[0] == '(') {
        ExposedTo whom = ExposedTo::Nobody;
        for (StringView candidate : exposed_trimmed.substring_view(1, exposed_trimmed.length() - 1).split_view(',')) {
            candidate = candidate.trim_whitespace();
            if (auto parsed_exposed = exposed_from_string(candidate); parsed_exposed.has_value()) {
                whom |= parsed_exposed.value();
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
