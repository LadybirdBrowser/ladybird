/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::HTML {

static bool token_equals_ignoring_ascii_case(Utf16View token, Utf16View keyword)
{
    return token.equals_ignoring_ascii_case(keyword);
}

static bool contains_token(auto const& tokens, Utf16View keyword)
{
    for (auto const& token : tokens) {
        if (token_equals_ignoring_ascii_case(token, keyword))
            return true;
    }

    return false;
}

static SandboxingFlagSet parse_sandboxing_directive_tokens(auto const& tokens)
{
    // 2. Let output be empty.
    SandboxingFlagSet output {};

    // 3. Add the following flags to output:
    // - The sandboxed navigation browsing context flag.
    output |= SandboxingFlagSet::SandboxedNavigation;

    // - The sandboxed auxiliary navigation browsing context flag, unless tokens contains the allow-popups keyword.
    if (!contains_token(tokens, u"allow-popups"sv))
        output |= SandboxingFlagSet::SandboxedAuxiliaryNavigation;

    // - The sandboxed top-level navigation without user activation browsing context flag, unless tokens contains the
    //   allow-top-navigation keyword.
    if (!contains_token(tokens, u"allow-top-navigation"sv))
        output |= SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation;

    // - The sandboxed top-level navigation with user activation browsing context flag, unless tokens contains either
    //   the allow-top-navigation-by-user-activation keyword or the allow-top-navigation keyword.
    // Spec Note: This means that if the allow-top-navigation is present, the allow-top-navigation-by-user-activation
    //            keyword will have no effect. For this reason, specifying both is a document conformance error.
    if (!contains_token(tokens, u"allow-top-navigation"sv) && !contains_token(tokens, u"allow-top-navigation-by-user-activation"sv))
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
    if (!contains_token(tokens, u"allow-same-origin"sv))
        output |= SandboxingFlagSet::SandboxedOrigin;

    // - The sandboxed forms browsing context flag, unless tokens contains the allow-forms keyword.
    if (!contains_token(tokens, u"allow-forms"sv))
        output |= SandboxingFlagSet::SandboxedForms;

    // - The sandboxed pointer lock browsing context flag, unless tokens contains the allow-pointer-lock keyword.
    if (!contains_token(tokens, u"allow-pointer-lock"sv))
        output |= SandboxingFlagSet::SandboxedPointerLock;

    // - The sandboxed scripts browsing context flag, unless tokens contains the allow-scripts keyword.
    // - The sandboxed automatic features browsing context flag, unless tokens contains the allow-scripts keyword
    //   (defined above).
    // Spec Note: This flag is relaxed by the same keyword as scripts, because when scripts are enabled these features
    //            are trivially possible anyway, and it would be unfortunate to force authors to use script to do them
    //            when sandboxed rather than allowing them to use the declarative features.
    if (!contains_token(tokens, u"allow-scripts"sv)) {
        output |= SandboxingFlagSet::SandboxedScripts;
        output |= SandboxingFlagSet::SandboxedAutomaticFeatures;
    }

    // - The sandboxed document.domain browsing context flag.
    output |= SandboxingFlagSet::SandboxedDocumentDomain;

    // - The sandbox propagates to auxiliary browsing contexts flag, unless tokens contains the
    //   allow-popups-to-escape-sandbox keyword.
    if (!contains_token(tokens, u"allow-popups-to-escape-sandbox"sv))
        output |= SandboxingFlagSet::SandboxPropagatesToAuxiliaryBrowsingContexts;

    // - The sandboxed modals flag, unless tokens contains the allow-modals keyword.
    if (!contains_token(tokens, u"allow-modals"sv))
        output |= SandboxingFlagSet::SandboxedModals;

    // - The sandboxed orientation lock browsing context flag, unless tokens contains the allow-orientation-lock
    //   keyword.
    if (!contains_token(tokens, u"allow-orientation-lock"sv))
        output |= SandboxingFlagSet::SandboxedOrientationLock;

    // - The sandboxed presentation browsing context flag, unless tokens contains the allow-presentation keyword.
    if (!contains_token(tokens, u"allow-presentation"sv))
        output |= SandboxingFlagSet::SandboxedPresentation;

    // - The sandboxed downloads browsing context flag, unless tokens contains the allow-downloads keyword.
    if (!contains_token(tokens, u"allow-downloads"sv))
        output |= SandboxingFlagSet::SandboxedDownloads;

    // - The sandboxed custom protocols navigation browsing context flag, unless tokens contains either the
    //   allow-top-navigation-to-custom-protocols keyword, the allow-popups keyword, or the allow-top-navigation
    //   keyword.
    if (!contains_token(tokens, u"allow-top-navigation-to-custom-protocols"sv) && !contains_token(tokens, u"allow-popups"sv) && !contains_token(tokens, u"allow-top-navigation"sv))
        output |= SandboxingFlagSet::SandboxedCustomProtocols;

    return output;
}

//  https://html.spec.whatwg.org/multipage/browsers.html#parse-a-sandboxing-directive
SandboxingFlagSet parse_a_sandboxing_directive(Utf16View input)
{
    // 1. Split input on ASCII whitespace, to obtain tokens.
    Utf16GenericLexer lexer { input };
    auto is_ascii_whitespace = [](char16_t code_unit) {
        return Infra::is_ascii_whitespace(code_unit);
    };

    Vector<Utf16View> tokens;
    while (!lexer.is_eof()) {
        lexer.ignore_while(is_ascii_whitespace);
        auto token = lexer.consume_until(is_ascii_whitespace);
        if (!token.is_empty())
            tokens.append(token);
    }

    return parse_sandboxing_directive_tokens(tokens);
}

SandboxingFlagSet parse_a_sandboxing_directive(ReadonlySpan<Utf16String const> pre_parsed_tokens)
{
    // FIXME: File spec issue that "parse a sandboxing directive" does not accept a set of tokens.
    return parse_sandboxing_directive_tokens(pre_parsed_tokens);
}

}
