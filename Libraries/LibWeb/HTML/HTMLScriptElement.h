/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLOrSVGScriptElement.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

class HTMLScriptElement final
    : public HTMLElement
    , public HTMLOrSVGScriptElement {
    WEB_WRAPPABLE(HTMLScriptElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLScriptElement);

public:
    virtual ~HTMLScriptElement() override;

    GC::Ref<DOM::DOMTokenList> blocking();

    virtual void children_changed(ChildrenChangedMetadata const&) override;
    virtual void post_connection() override;
    virtual bool has_post_connection_steps() const override { return true; }

    // https://html.spec.whatwg.org/multipage/scripting.html#dom-script-supports
    static bool supports(Utf16String const& type)
    {
        return type.is_one_of("classic"sv, "module"sv, "importmap"sv);
    }

    TrustedTypes::TrustedScriptOrString text() const { return child_text_content(); }
    WebIDL::ExceptionOr<void> set_text(TrustedTypes::TrustedScriptOrString);

    TrustedTypes::TrustedScriptURLOrString src() const;
    WebIDL::ExceptionOr<void> set_src(TrustedTypes::TrustedScriptURLOrString);

    Variant<GC::Ref<TrustedTypes::TrustedScript>, Utf16String, Empty> text_content() const;
    WebIDL::ExceptionOr<void> set_text_content(TrustedTypes::NullableTrustedScriptOrString);

    TrustedTypes::TrustedScriptOrString inner_text();
    WebIDL::ExceptionOr<void> set_inner_text(TrustedTypes::TrustedScriptOrString);

    [[nodiscard]] bool async() const;
    void set_async(bool);

    virtual WebIDL::ExceptionOr<void> cloned(Node&, bool) const override;

protected:
    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#implicitly-potentially-render-blocking
    virtual bool is_implicitly_potentially_render_blocking() const override;

private:
    HTMLScriptElement(DOM::Document&, DOM::QualifiedName);

    GC::Ptr<DOM::DOMTokenList> m_blocking;

    virtual bool is_html_script_element() const override { return true; }
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void adopted_from(DOM::Document&) override;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    // ^HTMLOrSVGScriptElement
    virtual DOM::Element& as_element() override { return *this; }
    virtual DOM::Element const& as_element() const override { return *this; }
    virtual bool has_source_attribute() const override;
    virtual Utf16String source_attribute_value() override;
    virtual Optional<Utf16String> language_attribute_value() const override;
    virtual Optional<Utf16String> charset_attribute_value() const override;
    virtual Optional<Utf16String> event_attribute_value() const override;
    virtual Optional<Utf16String> for_attribute_value() const override;
    virtual bool has_defer_attribute() const override;
    virtual bool has_nomodule_attribute() const override;
    virtual ScriptElementFetchAttributes fetch_attributes() const override;

    // https://html.spec.whatwg.org/multipage/scripting.html#attr-script-crossorigin
    CORSSettingAttribute m_crossorigin { CORSSettingAttribute::NoCORS };

    // https://html.spec.whatwg.org/multipage/scripting.html#attr-script-referrerpolicy
    ReferrerPolicy::ReferrerPolicy m_referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLScriptElement>() const { return is_html_script_element(); }

}
