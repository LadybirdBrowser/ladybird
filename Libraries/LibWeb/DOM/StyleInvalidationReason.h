/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

#define ENUMERATE_STYLE_INVALIDATION_REASONS(X)     \
    X(AdoptedStyleSheetsList)                       \
    X(BaseURLChanged)                               \
    X(CSSFontLoaded)                                \
    X(CSSImportRule)                                \
    X(CSSStylePropertiesRemoveProperty)             \
    X(CSSStylePropertiesSetProperty)                \
    X(CSSStylePropertiesSetPropertyStyleValue)      \
    X(CSSStylePropertiesTextChange)                 \
    X(CustomElementStateChange)                     \
    X(CustomStateSetChange)                         \
    X(DidLoseFocus)                                 \
    X(DidReceiveFocus)                              \
    X(EditingInsertion)                             \
    X(EditingDeletion)                              \
    X(ElementAttributeChange)                       \
    X(ElementSetShadowRoot)                         \
    X(Fullscreen)                                   \
    X(HTMLDialogElementSetIsModal)                  \
    X(HTMLDetailsOrDialogOpenAttributeChange)       \
    X(HTMLHyperlinkElementHrefChange)               \
    X(HTMLIFrameElementGeometryChange)              \
    X(HTMLInputElementSetChecked)                   \
    X(HTMLInputElementSetIsOpen)                    \
    X(HTMLInputElementSetType)                      \
    X(HTMLObjectElementUpdateLayoutAndChildObjects) \
    X(HTMLOptionElementSelectedChange)              \
    X(HTMLSelectElementSetIsOpen)                   \
    X(MediaListSetMediaText)                        \
    X(MediaListAppendMedium)                        \
    X(MediaListDeleteMedium)                        \
    X(MediaQueryChangedMatchState)                  \
    X(NavigableSetViewportSize)                     \
    X(NodeInsertBefore)                             \
    X(NodeRemove)                                   \
    X(NodeSetTextContent)                           \
    X(Other)                                        \
    X(SetSelectorText)                              \
    X(SettingsChange)                               \
    X(StyleSheetDeleteRule)                         \
    X(StyleSheetInsertRule)                         \
    X(StyleSheetListAddSheet)                       \
    X(StyleSheetListRemoveSheet)

enum class StyleInvalidationReason {
#define __ENUMERATE_STYLE_INVALIDATION_REASON(reason) reason,
    ENUMERATE_STYLE_INVALIDATION_REASONS(__ENUMERATE_STYLE_INVALIDATION_REASON)
#undef __ENUMERATE_STYLE_INVALIDATION_REASON
};

struct StyleInvalidationOptions {
    bool invalidate_self { false };
};

}
