/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

enum class CorsMode {
    NoCors,
    Cors,
};

using StyleResourceURL = Variant<::URL::URL, CSS::URL>;

// FIXME: This awkward API represents a CSS rule or a css declaration block. This could be a variant of
//        those two classes, but ImageStyleValue does not have ready access to those types but still
//        wants to go through CSS fetch. There is surely a less jank way of representing this.
struct RuleOrDeclaration {
    struct StyleDeclaration {
        GC::Ptr<CSSRule> parent_rule;
    };

    struct Rule {
        GC::Ptr<CSSStyleSheet> parent_style_sheet;
    };

    GC::Ref<HTML::EnvironmentSettingsObject> environment_settings_object;
    Variant<StyleDeclaration, Rule> value;
};

// https://drafts.csswg.org/css-values-4/#fetch-a-style-resource
GC::Ptr<Fetch::Infrastructure::FetchController> fetch_a_style_resource(StyleResourceURL const& url, RuleOrDeclaration, Fetch::Infrastructure::Request::Destination, CorsMode, Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_response);

// https://drafts.csswg.org/css-images-4/#fetch-an-external-image-for-a-stylesheet
GC::Ptr<HTML::SharedResourceRequest> fetch_an_external_image_for_a_stylesheet(StyleResourceURL const&, RuleOrDeclaration, DOM::Document&);

// https://drafts.csswg.org/css-values-5/#apply-request-modifiers-from-url-value
void apply_request_modifiers_from_url_value(URL const&, GC::Ref<Fetch::Infrastructure::Request>);

}
