/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>
#include <LibWeb/SVG/AttributeParsing.h>

namespace Web::CSS {

class LengthPercentage;
class LengthPercentageOrAuto;
class Size;

}

namespace Web::Layout {

// Registers the document-side answers every layout pass needs on the arena, once per document.
WEB_API void register_layout_host(NodeArena&, DOM::Document&);

// Publishes what an SVG element's attributes parse to, under its style node. The publication leaves with the
// identity. An element's attributes are layout input that no pass can change, so the document publishes them as
// they are written rather than answering for them while a pass runs.
void publish_svg_attribute_facts(DOM::Element&);
void publish_svg_style_references(DOM::Element&);

// Publishes whether a row built for the node sits in the user agent shadow tree of the focused text control, which is
// what a caret is painted inside. The overflow pass reserves a pixel for the caret, so it reads the published answer
// rather than asking the document who has focus.
void publish_is_in_focused_text_control(DOM::Node const&);

// Publishes what the element has scrolled to, under its identity. The element's box is replaced whenever its subtree is
// rebuilt, so the offset is held against the identity that outlives it, and every row built for the element reads it
// there.
void publish_element_scroll_offset(DOM::Element const&);

// Publishes the spans a table cell's or table column's attributes give it, under its identity, which table fixup reads
// before the build that stamps the element's row is over. Every other element spans one of each and publishes nothing.
void publish_table_spans(DOM::Element const&);

inline RustFFI::FfiSvgNumberPercentage to_ffi_number_percentage(SVG::NumberPercentage value)
{
    return { .value = value.value(), .is_percentage = value.is_percentage() };
}

}

// The general category facts of a code point, for finding the first letter of a block.
extern "C" WEB_API Web::Layout::RustFFI::FfiCodePointCategoryFacts ladybird_layout_code_point_category_facts(u32);

extern "C" WEB_API void ladybird_layout_node_shell_destroy(void*);
extern "C" WEB_API void ladybird_layout_owned_image_provider_destroy(void*);
extern "C" WEB_API void ladybird_layout_image_observers_destroy(void*);
extern "C" WEB_API void ladybird_layout_owned_image_provider_notify_detach(void*);
