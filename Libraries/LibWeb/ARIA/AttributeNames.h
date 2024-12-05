/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::ARIA::AttributeNames {

// https://www.w3.org/TR/wai-aria-1.2/#accessibilityroleandproperties-correspondence
#define ENUMERATE_ARIA_ATTRIBUTES                                                            \
    __ENUMERATE_ARIA_ATTRIBUTE(role, "role")                                                 \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_active_descendant, "aria-activedescendant")              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_atomic, "aria-atomic")                                   \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_auto_complete, "aria-autocomplete")                      \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_braille_label, "aria-braillelabel")                      \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_braille_role_description, "aria-brailleroledescription") \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_busy, "aria-busy")                                       \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_checked, "aria-checked")                                 \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_col_count, "aria-colcount")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_col_index, "aria-colindex")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_col_index_text, "aria-colindextext")                     \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_col_span, "aria-colspan")                                \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_controls, "aria-controls")                               \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_current, "aria-current")                                 \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_described_by, "aria-describedby")                        \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_description, "aria-description")                         \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_details, "aria-details")                                 \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_drop_effect, "aria-dropeffect")                          \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_error_message, "aria-errormessage")                      \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_disabled, "aria-disabled")                               \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_expanded, "aria-expanded")                               \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_flow_to, "aria-flowto")                                  \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_grabbed, "aria-grabbed")                                 \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_has_popup, "aria-haspopup")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_hidden, "aria-hidden")                                   \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_invalid, "aria-invalid")                                 \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_key_shortcuts, "aria-keyshortcuts")                      \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_label, "aria-label")                                     \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_labelled_by, "aria-labelledby")                          \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_level, "aria-level")                                     \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_live, "aria-live")                                       \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_modal, "aria-modal")                                     \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_multi_line, "aria-multiline")                            \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_multi_selectable, "aria-multiselectable")                \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_orientation, "aria-orientation")                         \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_owns, "aria-owns")                                       \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_placeholder, "aria-placeholder")                         \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_pos_in_set, "aria-posinset")                             \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_pressed, "aria-pressed")                                 \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_read_only, "aria-readonly")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_relevant, "aria-relevant")                               \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_required, "aria-required")                               \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_role_description, "aria-roledescription")                \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_row_count, "aria-rowcount")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_row_index, "aria-rowindex")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_row_index_text, "aria-rowindextext")                     \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_row_span, "aria-rowspan")                                \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_selected, "aria-selected")                               \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_set_size, "aria-setsize")                                \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_sort, "aria-sort")                                       \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_value_max, "aria-valuemax")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_value_min, "aria-valuemin")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_value_now, "aria-valuenow")                              \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_value_text, "aria-valuetext")

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute) \
    extern FlyString name;
ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

}
