/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/StyleGroupPayloadPins.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// What one element's computation was allowed to read, kept so a later one can ask whether any of it
// moved before deriving a style that would be identical.
//
// The words are the style sharing key minus the style being replaced - that one is the element's own
// and decides nothing here - and with the traversal-local match signature expanded into the
// declarations it names, since an identity minted per traversal cannot answer across two of them.
// The pins keep every value a word names by its address alive, so an address cannot come to mean
// something else while the record holds it.
struct StyleInputRecord {
    Vector<u64> words;
    Vector<NonnullRefPtr<StyleValue const>> pinned_values;
    StyleGroupPayloadPins pinned_parent_groups;
    RefPtr<CustomPropertyData const> pinned_parent_custom_property_data;
    // The style this input produced. A partial recomputation can use the element's current group
    // payloads only while they still name that answer.
    StyleRecordID computed_style_record;
    // A computation that produced its own property result binds the publication that follows. An
    // answer shared from another element keeps the old identity and is recomputed in full next time.
    bool bind_next_published_style { false };
    bool font_environment_changed { false };
    // Whether the computation this record describes read something the record does not name - its
    // container, its attributes, its place among its siblings, the environment an `if()` asks about.
    // Such a computation cannot be answered from the record, because what it read can move without
    // any word of it moving.
    bool read_beyond_the_record { true };
    // A snapshot of the element's authoritative dependency marks. The element clears its marks
    // before deriving a replacement, so a computation skipped through this record must restore the
    // previous marks that nothing else will leave.
    bool style_uses_attr_css_function { false };
    bool style_uses_var_css_function { false };
    bool style_uses_if_css_function { false };
    bool style_uses_custom_function { false };
    bool style_uses_inherit_css_function { false };
    bool style_uses_tree_counting_function { false };
    bool style_depends_on_viewport_metrics { false };
    bool style_depends_on_size_container_query { false };
    bool style_depends_on_style_container_query { false };
    u32 explicitly_inherited_non_inherited_style_groups { 0 };
    bool cascade_reads_custom_properties { false };

    // Which half of the record differs first, which is what says why a recomputation could not be
    // answered from what its last one read.
    enum class Difference : u8 {
        None,
        ParentStyle,
        ParentCustomProperties,
        Element,
        Declarations,
    };
};

}
