/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/semantics.html#pragma-directives
#define ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTES                                   \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE("content-language", ContentLanguage) \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE("content-type", EncodingDeclaration) \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE("default-style", DefaultStyle)       \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE("refresh", Refresh)                  \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE("set-cookie", SetCookie)             \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE("x-ua-compatible", XUACompatible)    \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE("content-security-policy", ContentSecurityPolicy)

class HTMLMetaElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLMetaElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLMetaElement);

public:
    virtual ~HTMLMetaElement() override;

    enum class HttpEquivAttributeState {
#define __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(_, state) state,
        ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTES
#undef __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE
    };

    Optional<HttpEquivAttributeState> http_equiv_state() const;

private:
    HTMLMetaElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    // ^DOM::Element
    virtual void inserted() override;
};

}
