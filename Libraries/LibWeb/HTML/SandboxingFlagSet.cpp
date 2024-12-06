/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::HTML {

//  https://html.spec.whatwg.org/multipage/browsers.html#parse-a-sandboxing-directive
SandboxingFlagSet parse_a_sandboxing_directive(Variant<String, Vector<String>> input)
{
    // 1. Split input on ASCII whitespace, to obtain tokens.
    Vector<String> tokens;
    if (input.has<String>()) {
        auto lowercase_input = input.get<String>().to_ascii_lowercase();
        auto token_views = lowercase_input.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        tokens.ensure_capacity(token_views.size());
        for (auto token : token_views) {
            tokens.unchecked_append(MUST(String::from_utf8(token)));
        }
    } else {
        auto const& pre_parsed_tokens = input.get<Vector<String>>();
        tokens.ensure_capacity(pre_parsed_tokens.size());
        for (auto const& token : pre_parsed_tokens) {
            tokens.unchecked_append(token.to_ascii_lowercase());
        }
    }

    // 2. Let output be empty.
    SandboxingFlagSet output {};

    // 3. Add the following flags to output:
    // - The sandboxed navigation browsing context flag.
    output |= SandboxingFlagSet::SandboxedNavigation;

    // - The sandboxed auxiliary navigation browsing context flag, unless tokens contains the allow-popups keyword.
    if (!tokens.contains_slow("allow-popups"sv))
        output |= SandboxingFlagSet::SandboxedAuxiliaryNavigation;

    // - The sandboxed top-level navigation without user activation browsing context flag, unless tokens contains the
    //   allow-top-navigation keyword.
    if (!tokens.contains_slow("allow-top-navigation"sv))
        output |= SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation;

    // - The sandboxed top-level navigation with user activation browsing context flag, unless tokens contains either
    //   the allow-top-navigation-by-user-activation keyword or the allow-top-navigation keyword.
    // Spec Note: This means that if the allow-top-navigation is present, the allow-top-navigation-by-user-activation
    //            keyword will have no effect. For this reason, specifying both is a document conformance error.
    if (!tokens.contains_slow("allow-top-navigation"sv) && !tokens.contains_slow("allow-top-navigation-by-user-activation"sv))
        output |= SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation;

    // - The sandboxed origin browsing context flag, unless the tokens contains the allow-same-origin keyword.
    // Spec Note: The allow-same-origin keyword is intended for two cases.
    //
    //            First, it can be used to allow content from the same site to be sandboxed to disable scripting,
    //            while still allowing access to the DOM of the sandboxed content.
    //
    //            Second, it can be used to embed content from a third-party site, sandboxed to prevent that site from
    //            opening popups, etc, without preventing the embedded page from communicating back to its originating
    //            site, using the database APIs to store data, etc.
    if (!tokens.contains_slow("allow-same-origin"sv))
        output |= SandboxingFlagSet::SandboxedOrigin;

    // - The sandboxed forms browsing context flag, unless tokens contains the allow-forms keyword.
    if (!tokens.contains_slow("allow-forms"sv))
        output |= SandboxingFlagSet::SandboxedForms;

    // - The sandboxed pointer lock browsing context flag, unless tokens contains the allow-pointer-lock keyword.
    if (!tokens.contains_slow("allow-pointer-lock"sv))
        output |= SandboxingFlagSet::SandboxedPointerLock;

    // - The sandboxed scripts browsing context flag, unless tokens contains the allow-scripts keyword.
    // - The sandboxed automatic features browsing context flag, unless tokens contains the allow-scripts keyword
    //   (defined above).
    // Spec Note: This flag is relaxed by the same keyword as scripts, because when scripts are enabled these features
    //            are trivially possible anyway, and it would be unfortunate to force authors to use script to do them
    //            when sandboxed rather than allowing them to use the declarative features.
    if (!tokens.contains_slow("allow-scripts"sv)) {
        output |= SandboxingFlagSet::SandboxedScripts;
        output |= SandboxingFlagSet::SandboxedAutomaticFeatures;
    }

    // - The sandboxed document.domain browsing context flag.
    output |= SandboxingFlagSet::SandboxedDocumentDomain;

    // - The sandbox propagates to auxiliary browsing contexts flag, unless tokens contains the
    //   allow-popups-to-escape-sandbox keyword.
    if (!tokens.contains_slow("allow-popups-to-escape-sandbox"sv))
        output |= SandboxingFlagSet::SandboxPropagatesToAuxiliaryBrowsingContexts;

    // - The sandboxed modals flag, unless tokens contains the allow-modals keyword.
    if (!tokens.contains_slow("allow-modals"sv))
        output |= SandboxingFlagSet::SandboxedModals;

    // - The sandboxed orientation lock browsing context flag, unless tokens contains the allow-orientation-lock
    //   keyword.
    if (!tokens.contains_slow("allow-orientation-lock"sv))
        output |= SandboxingFlagSet::SandboxedOrientationLock;

    // - The sandboxed presentation browsing context flag, unless tokens contains the allow-presentation keyword.
    if (!tokens.contains_slow("allow-presentation"sv))
        output |= SandboxingFlagSet::SandboxedPresentation;

    // - The sandboxed downloads browsing context flag, unless tokens contains the allow-downloads keyword.
    if (!tokens.contains_slow("allow-downloads"sv))
        output |= SandboxingFlagSet::SandboxedDownloads;

    // - The sandboxed custom protocols navigation browsing context flag, unless tokens contains either the
    //   allow-top-navigation-to-custom-protocols keyword, the allow-popups keyword, or the allow-top-navigation
    //   keyword.
    if (!tokens.contains_slow("allow-top-navigation-to-custom-protocols"sv) && !tokens.contains_slow("allow-popups"sv) && !tokens.contains_slow("allow-top-navigation"sv))
        output |= SandboxingFlagSet::SandboxedCustomProtocols;

    return output;
}

}
