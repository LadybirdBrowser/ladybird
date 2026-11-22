/*
 * Copyright (c) 2024-2025, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebVTT/VTTCue.h>

namespace Web::WebVTT {

// The cue text parser supplies the nodes in tree order. Index zero names the document fragment.
enum class CueNodeKind : u8 { Text,
    Timestamp,
    Class,
    Italic,
    Bold,
    Underline,
    Ruby,
    RubyText,
    Voice,
    Language };

struct CueTextNode {
    size_t parent;
    CueNodeKind kind;
    FlatPtr value;
    FlatPtr classes;
};

extern "C" {

// Defined in Bindings/Rust/src/web_vtt/exports.rs.
void* web_vtt_cue_create(JSVM*, double start_time, double end_time, FlatPtr text);
GC::Cell* web_vtt_create_fragment(DOM::Document&, CueTextNode const*, size_t);
}

GC::Ref<VTTCue> VTTCue::create(double start_time, double end_time, Utf16String const& text)
{
    auto* cue = web_vtt_cue_create(JS::HostABI::vm_to_abi(JS::VM::the()), start_time, end_time, text.raw_identity());
    return GC::Ref { *static_cast<VTTCue*>(cue) };
}

// https://w3c.github.io/webvtt/#dom-construction-rules
GC::Cell* web_vtt_create_fragment(DOM::Document& document, CueTextNode const* nodes, size_t count)
{
    auto fragment = DOM::DocumentFragment::create(document);
    GC::RootVector<GC::Ref<DOM::Node>> tree;
    tree.append(fragment);
    for (auto const& node : ReadonlySpan<CueTextNode> { nodes, count }) {
        auto value = Utf16String::from_raw(node.value);
        auto create_node = [&]() -> GC::Ref<DOM::Node> {
            if (node.kind == CueNodeKind::Text)
                return DOM::Text::create(document, move(value));
            if (node.kind == CueNodeKind::Timestamp)
                return DOM::ProcessingInstruction::create(document, move(value), "timestamp"_utf16_fly_string);

            auto const& name = [&]() -> Utf16FlyString const& {
                switch (node.kind) {
                case CueNodeKind::Class:
                case CueNodeKind::Voice:
                case CueNodeKind::Language:
                    return HTML::TagNames::span;
                case CueNodeKind::Italic:
                    return HTML::TagNames::i;
                case CueNodeKind::Bold:
                    return HTML::TagNames::b;
                case CueNodeKind::Underline:
                    return HTML::TagNames::u;
                case CueNodeKind::Ruby:
                    return HTML::TagNames::ruby;
                case CueNodeKind::RubyText:
                    return HTML::TagNames::rt;
                default:
                    VERIFY_NOT_REACHED();
                }
            }();
            auto element = MUST(DOM::create_element(document, name, Namespace::HTML));
            if (node.kind == CueNodeKind::Voice)
                element->set_attribute_value(HTML::AttributeNames::title, value);
            if (node.kind == CueNodeKind::Language)
                element->set_attribute_value(HTML::AttributeNames::lang, value);
            auto classes = Utf16String::from_raw(node.classes);
            if (!classes.is_empty())
                element->set_attribute_value(HTML::AttributeNames::class_, classes);
            return element;
        };
        auto child = create_node();
        tree[node.parent]->parser_append_child(child);
        tree.append(child);
    }
    return fragment.ptr();
}

}
