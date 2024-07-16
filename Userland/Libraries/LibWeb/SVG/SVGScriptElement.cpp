/*
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGScriptElementPrototype.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGScriptElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGScriptElement);

SVGScriptElement::SVGScriptElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGScriptElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGScriptElement);
}

void SVGScriptElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_script);
}

// https://www.w3.org/TR/SVGMobile12/script.html#ScriptContentProcessing
void SVGScriptElement::process_the_script_element()
{
    // 1. If the 'script' element's "already processed" flag is true or if the element is not in the
    //    document tree, then no action is performed and these steps are ended.
    if (m_already_processed || !in_a_document_tree())
        return;

    auto inline_script = child_text_content();

    // FIXME: 2. If the 'script' element references external script content, then the external script content
    //           using the current value of the 'xlink:href' attribute is fetched. Further processing of the
    //           'script' element is dependent on the external script content, and will block here until the
    //           resource has been fetched or is determined to be an invalid IRI reference.
    if (has_attribute(SVG::AttributeNames::href) || has_attribute_ns(Namespace::XLink.to_string(), SVG::AttributeNames::href)) {
        dbgln("FIXME: Unsupported external fetch of SVGScriptElement!");
        return;
    }

    // 3. The 'script' element's "already processed" flag is set to true.
    m_already_processed = true;

    // 4. If the script content is inline, or if it is external and was fetched successfully, then the
    //    script is executed. Note that at this point, these steps may be re-entrant if the execution
    //    of the script results in further 'script' elements being inserted into the document.

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // Before any script execution occurs, the user agent must wait for scripts may run for the newly-created document to be true for document.
    if (!m_document->ready_to_run_scripts())
        HTML::main_thread_event_loop().spin_until([&] { return m_document->ready_to_run_scripts(); });

    // FIXME: Support non-inline scripts.
    auto& settings_object = document().relevant_settings_object();
    auto base_url = document().base_url();

    m_script = HTML::ClassicScript::create(m_document->url().to_byte_string(), inline_script, settings_object, base_url, m_source_line_number);
    (void)m_script->run();
}

}
