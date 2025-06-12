/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/ARIA/RoleType.h>

namespace Web::ARIA {

enum class StateAndProperties {
    AriaActiveDescendant,
    AriaAtomic,
    AriaAutoComplete,
    AriaBrailleLabel,
    AriaBrailleRoleDescription,
    AriaBusy,
    AriaChecked,
    AriaColCount,
    AriaColIndex,
    AriaColIndexText,
    AriaColSpan,
    AriaControls,
    AriaCurrent,
    AriaDescribedBy,
    AriaDescription,
    AriaDetails,
    AriaDisabled,
    AriaDropEffect,
    AriaErrorMessage,
    AriaExpanded,
    AriaFlowTo,
    AriaGrabbed,
    AriaHasPopup,
    AriaHidden,
    AriaInvalid,
    AriaKeyShortcuts,
    AriaLabel,
    AriaLabelledBy,
    AriaLevel,
    AriaLive,
    AriaModal,
    AriaMultiLine,
    AriaMultiSelectable,
    AriaOrientation,
    AriaOwns,
    AriaPlaceholder,
    AriaPosInSet,
    AriaPressed,
    AriaReadOnly,
    AriaRelevant,
    AriaRequired,
    AriaRoleDescription,
    AriaRowCount,
    AriaRowIndex,
    AriaRowIndexText,
    AriaRowSpan,
    AriaSelected,
    AriaSetSize,
    AriaSort,
    AriaValueMax,
    AriaValueMin,
    AriaValueNow,
    AriaValueText
};

using DefaultValueType = Variant<Empty, f64, AriaOrientation, AriaLive, bool, AriaHasPopup>;
ErrorOr<String> state_or_property_to_string_value(StateAndProperties, AriaData const&, DefaultValueType = {});
ErrorOr<String> tristate_to_string(Tristate);
ErrorOr<String> optional_integer_to_string(Optional<i32>);
ErrorOr<String> optional_bool_to_string(Optional<bool>);
ErrorOr<String> optional_number_to_string(Optional<f64>);
ErrorOr<String> id_reference_list_to_string(Vector<String> const&);
StringView state_or_property_to_string(StateAndProperties);

}
