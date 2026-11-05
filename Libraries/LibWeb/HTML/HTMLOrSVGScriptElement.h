/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/Utf16String.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/Scripting/ImportMapParseResult.h>
#include <LibWeb/HTML/Scripting/Script.h>
#include <LibWeb/HTML/Scripting/ScriptFetchOptions.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

struct ScriptElementFetchAttributes {
    CORSSettingAttribute crossorigin { CORSSettingAttribute::NoCORS };
    Utf16String cryptographic_nonce {};
    Optional<Utf16String> integrity {};
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
    Fetch::Infrastructure::Request::Priority fetch_priority { Fetch::Infrastructure::Request::Priority::Auto };
    bool parser_inserted { false };
};

ScriptFetchOptions script_element_fetch_options(DOM::Element const&, ScriptElementFetchAttributes const&);

class HTMLOrSVGScriptElement {
public:
    virtual ~HTMLOrSVGScriptElement() = default;

    bool is_force_async() const { return m_force_async; }
    bool is_ready_to_be_parser_executed() const { return m_ready_to_be_parser_executed; }
    bool is_parser_inserted() const { return !!m_parser_document; }

    void set_parser_document(Badge<XMLDocumentBuilder, HTMLParser>, DOM::Document& document) { m_parser_document = &document; }
    void set_force_async(Badge<XMLDocumentBuilder, HTMLParser>, bool force_async) { m_force_async = force_async; }
    void set_already_started(Badge<XMLDocumentBuilder, HTMLParser>, bool already_started) { m_already_started = already_started; }
    void prepare_script(Badge<XMLDocumentBuilder, HTMLParser>) { prepare_script(); }

    void execute_script();
    void stop_delaying_document_load_event(Badge<DOM::Document>) { m_document_load_event_delayer.clear(); }

    void set_source_line_number(Badge<HTMLParser>, size_t source_line_number) { m_source_line_number = source_line_number; }
    void set_string_text(Utf16View value) { m_script_text = Utf16String::from_utf16(value); }
    void set_script_text_from_parser();

    class ParserChildrenChangeScope {
        AK_MAKE_NONCOPYABLE(ParserChildrenChangeScope);
        AK_MAKE_NONMOVABLE(ParserChildrenChangeScope);

    public:
        explicit ParserChildrenChangeScope(DOM::Node& parent)
        {
            if (auto* element = as_if<DOM::Element>(parent); element && element->local_name() == TagNames::script)
                begin_changing_children(*element);
        }

        ~ParserChildrenChangeScope()
        {
            if (m_script_element)
                m_script_element->m_parser_is_changing_children = m_was_changing_children;
        }

    private:
        void begin_changing_children(DOM::Element&);

        HTMLOrSVGScriptElement* m_script_element { nullptr };
        bool m_was_changing_children { false };
    };

    virtual DOM::Element& as_element() = 0;
    virtual DOM::Element const& as_element() const = 0;

protected:
    enum class ScriptType {
        Null,
        Classic,
        Module,
        ImportMap,
    };

    ScriptType script_type() const { return m_script_type; }
    void set_force_async(bool force_async) { m_force_async = force_async; }

    void script_element_attribute_changed(Utf16FlyString const& name);
    void script_element_children_changed(DOM::Node::ChildrenChangedMetadata const&);
    void script_element_post_connection();
    void script_element_cloned(HTMLOrSVGScriptElement& copy) const;

    void visit_script_element_edges(GC::Cell::Visitor&);

    void retarget_script_load_event_delayer(DOM::Document& new_document)
    {
        if (m_document_load_event_delayer.has_value())
            m_document_load_event_delayer.emplace(new_document);
    }

    virtual bool has_source_attribute() const = 0;
    virtual Utf16String source_attribute_value() = 0;
    virtual Optional<Utf16String> language_attribute_value() const = 0;
    virtual Optional<Utf16String> charset_attribute_value() const = 0;
    virtual Optional<Utf16String> event_attribute_value() const = 0;
    virtual Optional<Utf16String> for_attribute_value() const = 0;
    virtual bool has_defer_attribute() const = 0;
    virtual bool has_nomodule_attribute() const = 0;
    virtual ScriptElementFetchAttributes fetch_attributes() const = 0;

private:
    // https://www.w3.org/TR/trusted-types/#prepare-script-text
    WebIDL::ExceptionOr<void> prepare_script_text();

    // https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element
    void prepare_script();

    void begin_delaying_document_load_event(DOM::Document&);

    struct ResultState {
        struct Uninitialized { };
        struct Null { };
    };

    using Result = Variant<ResultState::Uninitialized, ResultState::Null, GC::Ref<HTML::Script>, GC::Ref<HTML::ImportMapParseResult>>;

    // https://html.spec.whatwg.org/multipage/scripting.html#mark-as-ready
    void mark_as_ready(Result);

    // https://html.spec.whatwg.org/multipage/scripting.html#parser-document
    GC::Ptr<DOM::Document> m_parser_document;

    // https://html.spec.whatwg.org/multipage/scripting.html#preparation-time-document
    GC::Ptr<DOM::Document> m_preparation_time_document;

    // https://html.spec.whatwg.org/multipage/scripting.html#script-force-async
    bool m_force_async { true };

    // https://html.spec.whatwg.org/multipage/scripting.html#already-started
    bool m_already_started { false };

    // https://html.spec.whatwg.org/multipage/scripting.html#concept-script-external
    bool m_from_an_external_file { false };

    // https://html.spec.whatwg.org/multipage/scripting.html#ready-to-be-parser-executed
    bool m_ready_to_be_parser_executed { false };

    // https://html.spec.whatwg.org/multipage/scripting.html#concept-script-type
    ScriptType m_script_type { ScriptType::Null };

    // https://html.spec.whatwg.org/multipage/scripting.html#steps-to-run-when-the-result-is-ready
    Function<void()> m_steps_to_run_when_the_result_is_ready;

    // https://html.spec.whatwg.org/multipage/scripting.html#concept-script-result
    Result m_result { ResultState::Uninitialized {} };

    // https://html.spec.whatwg.org/multipage/scripting.html#concept-script-delay-load
    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer;

    size_t m_source_line_number { 1 };

    // https://www.w3.org/TR/trusted-types/#htmlscriptelement-script-text
    Utf16String m_script_text;

    bool m_parser_is_changing_children { false };
    bool m_children_changed_by_api { false };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLOrSVGScriptElement>() const { return is_html_script_element() || is_svg_script_element(); }

template<>
WEB_API HTML::HTMLOrSVGScriptElement* Node::fast_as<HTML::HTMLOrSVGScriptElement>();
template<>
WEB_API HTML::HTMLOrSVGScriptElement const* Node::fast_as<HTML::HTMLOrSVGScriptElement>() const;

}
