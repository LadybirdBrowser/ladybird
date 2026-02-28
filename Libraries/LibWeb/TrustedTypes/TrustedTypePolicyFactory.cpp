/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordTrustedTypes.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/WindowEventHandlers.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicyFactory);

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-getattributetype
Optional<Utf16String> TrustedTypePolicyFactory::get_attribute_type(Utf16String const& tag_name, Utf16String& attribute, Optional<Utf16String> element_ns, Optional<Utf16String> attr_ns)
{
    // 1. Set localName to tagName in ASCII lowercase.
    auto const local_name = tag_name.to_ascii_lowercase();

    // 2. Set attribute to attribute in ASCII lowercase.
    attribute = attribute.to_ascii_lowercase();

    // 3. If elementNs is null or an empty string, set elementNs to HTML namespace.
    if (!element_ns.has_value() || element_ns.value().is_empty())
        element_ns = Utf16String::from_utf8(Namespace::HTML);

    // 4. If attrNs is an empty string, set attrNs to null.
    if (attr_ns.has_value() && attr_ns.value().is_empty())
        attr_ns.clear();

    // 5. Let interface be the element interface for localName and elementNs.
    auto const interface = element_interface(local_name, element_ns.value().to_utf8());

    // 6. Let expectedType be null.
    Optional<Utf16String> expected_type {};

    // 7. Set attributeData to the result of Get Trusted Type data for attribute algorithm,
    // with the following arguments, interface as element, attribute, attrNs
    auto const attribute_data = get_trusted_type_data_for_attribute(interface, attribute, attr_ns);

    // 8. If attributeData is not null, then set expectedType to the interface’s name of the value of the fourth member of attributeData.
    if (attribute_data.has_value()) {
        expected_type = to_string(attribute_data.value().trusted_type);
    }

    // 9. Return expectedType.
    return expected_type;
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-getpropertytype
Optional<Utf16String> TrustedTypePolicyFactory::get_property_type(Utf16String const& tag_name, Utf16String const& property, Optional<Utf16String> element_ns)
{
    // 1. Set localName to tagName in ASCII lowercase.
    auto const local_name = tag_name.to_ascii_lowercase();

    // 2. If elementNs is null or an empty string, set elementNs to HTML namespace.
    if (!element_ns.has_value() || element_ns.value().is_empty())
        element_ns = Utf16String::from_utf8(Namespace::HTML);

    // FIXME: We don't have a method in ElementFactory that can give us the interface name but these are all the cases
    // we care about in the table in get_trusted_type_data_for_attribute function
    // 3. Let interface be the element interface for localName and elementNs.
    Utf16String interface;
    if (local_name == HTML::TagNames::iframe && element_ns == Namespace::HTML) {
        interface = "HTMLIFrameElement"_utf16;
    } else if (local_name == HTML::TagNames::script && element_ns == Namespace::HTML) {
        interface = "HTMLScriptElement"_utf16;
    } else {
        interface = "Element"_utf16;
    }

    // 4. Let expectedType be null.
    Optional<Utf16String> expected_type;

    struct TrustedTypesPropertyTypeData {
        Utf16String interface;
        Utf16String property;
        TrustedTypeName trusted_type;
    };

    static Vector<TrustedTypesPropertyTypeData> const table {
        { "HTMLIFrameElement"_utf16, "srcdoc"_utf16, TrustedTypeName::TrustedHTML },
        { "HTMLScriptElement"_utf16, "innerText"_utf16, TrustedTypeName::TrustedScript },
        { "HTMLScriptElement"_utf16, "src"_utf16, TrustedTypeName::TrustedScriptURL },
        { "HTMLScriptElement"_utf16, "text"_utf16, TrustedTypeName::TrustedScript },
        { "HTMLScriptElement"_utf16, "textContent"_utf16, TrustedTypeName::TrustedScript },
        { "*"_utf16, "innerHTML"_utf16, TrustedTypeName::TrustedHTML },
        { "*"_utf16, "outerHTML"_utf16, TrustedTypeName::TrustedHTML },
    };

    // 5. Find the row in the following table, where the first column is "*" or interface’s name, and property is in the second column.
    // If a matching row is found, set expectedType to the interface’s name of the value of the third column.
    auto const matching_row = table.first_matching([&interface, &property](auto const& row) {
        return (row.interface == interface || row.interface == "*"sv) && row.property == property;
    });

    if (matching_row.has_value()) {
        expected_type = to_string(matching_row.value().trusted_type);
    }

    // 6. Return expectedType.
    return expected_type;
}

TrustedTypePolicyFactory::TrustedTypePolicyFactory(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void TrustedTypePolicyFactory::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedTypePolicyFactory);
    Base::initialize(realm);
}

void TrustedTypePolicyFactory::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_default_policy);
    visitor.visit(m_empty_html);
    visitor.visit(m_empty_script);
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-createpolicy
WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> TrustedTypePolicyFactory::create_policy(Utf16String const& policy_name, TrustedTypePolicyOptions const& policy_options)
{
    // 1. Returns the result of executing a Create a Trusted Type Policy algorithm, with the following arguments:
    //      factory: this value
    //      policyName: policyName
    //      options: policyOptions
    //      global: this value’s relevant global object
    return create_a_trusted_type_policy(policy_name, policy_options, HTML::relevant_global_object(*this));
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-ishtml
bool TrustedTypePolicyFactory::is_html(JS::Value value)
{
    // 1. Returns true if value is an instance of TrustedHTML and has an associated data value set, false otherwise.
    return value.is<TrustedHTML>();
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-isscript
bool TrustedTypePolicyFactory::is_script(JS::Value value)
{
    // 1. Returns true if value is an instance of TrustedScript and has an associated data value set, false otherwise.
    return value.is<TrustedScript>();
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-isscripturl
bool TrustedTypePolicyFactory::is_script_url(JS::Value value)
{
    // 1. Returns true if value is an instance of TrustedScriptURL and has an associated data value set, false otherwise.
    return value.is<TrustedScriptURL>();
}

GC::Ref<TrustedHTML const> TrustedTypePolicyFactory::empty_html()
{
    auto& realm = this->realm();

    if (!m_empty_html)
        m_empty_html = realm.create<TrustedHTML>(realm, ""_utf16);

    return GC::Ref { *m_empty_html };
}

GC::Ref<TrustedScript const> TrustedTypePolicyFactory::empty_script()
{
    auto& realm = this->realm();

    if (!m_empty_script)
        m_empty_script = realm.create<TrustedScript>(realm, ""_utf16);

    return GC::Ref { *m_empty_script };
}

// https://w3c.github.io/trusted-types/dist/spec/#create-trusted-type-policy-algorithm
WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> TrustedTypePolicyFactory::create_a_trusted_type_policy(Utf16String const& policy_name, TrustedTypePolicyOptions const& options, JS::Object& global)
{
    auto& realm = this->realm();

    // 1. Let allowedByCSP be the result of executing Should Trusted Type policy creation be blocked by Content Security Policy? algorithm with global, policyName and factory’s created policy names value.
    auto const allowed_by_csp = should_trusted_type_policy_be_blocked_by_content_security_policy(global, policy_name, m_created_policy_names);

    // 2. If allowedByCSP is "Blocked", throw a TypeError and abort further steps.
    if (allowed_by_csp == ContentSecurityPolicy::Directives::Directive::Result::Blocked)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Content Security Policy blocked the creation of the policy {}", policy_name)) };

    // 3. If policyName is default and the factory’s default policy value is not null, throw a TypeError and abort further steps.
    if (policy_name == "default"sv && m_default_policy)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Policy Factory already has a default value defined"_string };

    // 4. Let policy be a new TrustedTypePolicy object.
    // 5. Set policy’s name property value to policyName.
    // 6. Set policy’s options value to «[ "createHTML" -> options["createHTML", "createScript" -> options["createScript", "createScriptURL" -> options["createScriptURL" ]».
    auto const policy = realm.create<TrustedTypePolicy>(realm, policy_name, options);

    // 7. If the policyName is default, set the factory’s default policy value to policy.
    if (policy_name == "default"sv)
        m_default_policy = policy;

    // 8. Append policyName to factory’s created policy names.
    m_created_policy_names.append(policy_name);

    // 9. Return policy.
    return policy;
}

// https://www.w3.org/TR/trusted-types/#should-block-create-policy
ContentSecurityPolicy::Directives::Directive::Result TrustedTypePolicyFactory::should_trusted_type_policy_be_blocked_by_content_security_policy(JS::Object& global, Utf16String const& policy_name, Vector<Utf16String> const& created_policy_names)
{
    auto& realm = this->realm();

    // 1. Let result be "Allowed".
    auto result = ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 2. For each policy in global’s CSP list:
    for (auto const policy : ContentSecurityPolicy::PolicyList::from_object(global)->policies()) {
        // 1. Let createViolation be false.
        bool create_violation = false;

        // 2. If policy’s directive set does not contain a directive which name is "trusted-types", skip to the next policy.
        if (!policy->contains_directive_with_name(ContentSecurityPolicy::Directives::Names::TrustedTypes))
            continue;

        // 3. Let directive be the policy’s directive set’s directive which name is "trusted-types"
        auto const directive = policy->get_directive_by_name(ContentSecurityPolicy::Directives::Names::TrustedTypes);

        // 4. If directive’s value only contains a tt-keyword which is a match for a value 'none', set createViolation to true.
        if (directive->value().size() == 1 && directive->value().first().equals_ignoring_ascii_case(ContentSecurityPolicy::Directives::KeywordTrustedTypes::None))
            create_violation = true;

        // 5. If createdPolicyNames contains policyName and directive’s value does not contain a tt-keyword which is a match for a value 'allow-duplicates', set createViolation to true.
        auto created_policy_names_iterator = created_policy_names.find(policy_name);
        if (!created_policy_names_iterator.is_end()) {
            auto maybe_allow_duplicates = directive->value().find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(ContentSecurityPolicy::Directives::KeywordTrustedTypes::AllowDuplicates);
            });
            if (maybe_allow_duplicates.is_end())
                create_violation = true;
        }

        // 6. If directive’s value does not contain a tt-policy-name, which value is policyName, and directive’s value does not contain a tt-wildcard, set createViolation to true.
        auto directive_value_iterator = directive->value().find(policy_name.to_utf8());
        if (directive_value_iterator.is_end()) {
            auto maybe_wild_card = directive->value().find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case(ContentSecurityPolicy::Directives::KeywordTrustedTypes::WildCard);
            });

            if (maybe_wild_card.is_end())
                create_violation = true;
        }

        // 7. If createViolation is false, skip to the next policy.
        if (!create_violation)
            continue;

        // 8. Let violation be the result of executing Create a violation object for global, policy, and directive on global, policy and "trusted-types"
        auto const violation = ContentSecurityPolicy::Violation::create_a_violation_object_for_global_policy_and_directive(realm, global, policy, ContentSecurityPolicy::Directives::Names::TrustedTypes.to_string());

        // 9. Set violation’s resource to "trusted-types-policy".
        violation->set_resource(ContentSecurityPolicy::Violation::Resource::TrustedTypesPolicy);

        // 10. Set violation’s sample to the substring of policyName, containing its first 40 characters.
        auto sample = policy_name.substring_view(0, min(policy_name.length_in_code_points(), 40));
        violation->set_sample(Utf16String::from_utf16(sample).to_utf8());

        // 11. Execute Report a violation on violation.
        violation->report_a_violation(realm);

        // 12. If policy’s disposition is "enforce", then set result to "Blocked".
        if (policy->disposition() == ContentSecurityPolicy::Policy::Disposition::Enforce)
            result = ContentSecurityPolicy::Directives::Directive::Result::Blocked;
    }

    // 3. Return result.
    return result;
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-data-for-attribute
Optional<TrustedTypeData> get_trusted_type_data_for_attribute(ElementInterface const& element, Utf16String const& attribute, Optional<Utf16String> const& attribute_ns)
{
    // 1. Let data be null.
    Optional<TrustedTypeData const&> data {};

    auto const& [element_name, element_ns] = element;

    // 2. If attributeNs is null, « HTML namespace, SVG namespace, MathML namespace » contains element’s namespace, and attribute is the name of an event handler content attribute:
    if (!attribute_ns.has_value()
        && (Namespace::HTML == element_ns || Namespace::SVG == element_ns || Namespace::MathML == element_ns)) {
#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                                                                                       \
    if (attribute == HTML::AttributeNames::attribute_name) {                                                                                          \
        /* 1. Return (Element, null, attribute, TrustedScript, "Element " + attribute). */                                                            \
        return TrustedTypeData { "Element"_utf16, {}, attribute.to_utf8(), TrustedTypeName::TrustedScript, InjectionSink::Element_##attribute_name }; \
    }
        ENUMERATE_GLOBAL_EVENT_HANDLERS(__ENUMERATE)
        ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
    }

    static Vector<TrustedTypeData> const table {
        { "HTMLIFrameElement"_utf16, {}, HTML::AttributeNames::srcdoc, TrustedTypeName::TrustedHTML, InjectionSink::HTMLIFrameElement_srcdoc },
        { "HTMLScriptElement"_utf16, {}, HTML::AttributeNames::src, TrustedTypeName::TrustedScriptURL, InjectionSink::HTMLScriptElement_src },
        { "SVGScriptElement"_utf16, {}, HTML::AttributeNames::href, TrustedTypeName::TrustedScriptURL, InjectionSink::SVGScriptElement_href },
        { "SVGScriptElement"_utf16, Utf16String::from_utf8(Namespace::XLink), HTML::AttributeNames::href, TrustedTypeName::TrustedScriptURL, InjectionSink::SVGScriptElement_href },
    };

    // 3. Find the row in the following table, where element is in the first column, attributeNs is in the second column,
    // and attribute is in the third column. If a matching row is found, set data to that row.
    data = table.first_matching([&element_name, &attribute, &attribute_ns](auto const& row) {
        return row.element == element_name && row.attribute_ns == attribute_ns && row.attribute_local_name == attribute;
    });

    // 4. Return data
    return data.copy();
}

}
