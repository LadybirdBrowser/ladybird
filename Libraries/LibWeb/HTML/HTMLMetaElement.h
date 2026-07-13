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
#define ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTES                                    \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(u"content-language", ContentLanguage) \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(u"content-type", EncodingDeclaration) \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(u"default-style", DefaultStyle)       \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(u"refresh", Refresh)                  \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(u"set-cookie", SetCookie)             \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(u"x-ua-compatible", XUACompatible)    \
    __ENUMERATE_HTML_META_HTTP_EQUIV_ATTRIBUTE(u"content-security-policy", ContentSecurityPolicy)

class HTMLMetaElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLMetaElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLMetaElement);

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

    void update_metadata(Optional<Utf16String> const& old_name = {});
    void update_referrer_policy();

    // ^DOM::Element
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
};

}
