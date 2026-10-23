/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleProperties.h>

namespace Web::CSS {

#define ENUMERATE_CSS_POSITION_TRY_DESCRIPTORS(X)                    \
    X(margin, "margin", Margin)                                      \
    X(margin_top, "margin-top", MarginTop)                           \
    X(margin_right, "margin-right", MarginRight)                     \
    X(margin_bottom, "margin-bottom", MarginBottom)                  \
    X(margin_left, "margin-left", MarginLeft)                        \
    X(margin_block, "margin-block", MarginBlock)                     \
    X(margin_block_start, "margin-block-start", MarginBlockStart)    \
    X(margin_block_end, "margin-block-end", MarginBlockEnd)          \
    X(margin_inline, "margin-inline", MarginInline)                  \
    X(margin_inline_start, "margin-inline-start", MarginInlineStart) \
    X(margin_inline_end, "margin-inline-end", MarginInlineEnd)       \
    X(inset, "inset", Inset)                                         \
    X(inset_block, "inset-block", InsetBlock)                        \
    X(inset_block_start, "inset-block-start", InsetBlockStart)       \
    X(inset_block_end, "inset-block-end", InsetBlockEnd)             \
    X(inset_inline, "inset-inline", InsetInline)                     \
    X(inset_inline_start, "inset-inline-start", InsetInlineStart)    \
    X(inset_inline_end, "inset-inline-end", InsetInlineEnd)          \
    X(top, "top", Top)                                               \
    X(left, "left", Left)                                            \
    X(right, "right", Right)                                         \
    X(bottom, "bottom", Bottom)                                      \
    X(width, "width", Width)                                         \
    X(min_width, "min-width", MinWidth)                              \
    X(max_width, "max-width", MaxWidth)                              \
    X(height, "height", Height)                                      \
    X(min_height, "min-height", MinHeight)                           \
    X(max_height, "max-height", MaxHeight)                           \
    X(block_size, "block-size", BlockSize)                           \
    X(min_block_size, "min-block-size", MinBlockSize)                \
    X(max_block_size, "max-block-size", MaxBlockSize)                \
    X(inline_size, "inline-size", InlineSize)                        \
    X(min_inline_size, "min-inline-size", MinInlineSize)             \
    X(max_inline_size, "max-inline-size", MaxInlineSize)             \
    X(place_self, "place-self", PlaceSelf)                           \
    X(align_self, "align-self", AlignSelf)                           \
    X(justify_self, "justify-self", JustifySelf)                     \
    X(position_anchor, "position-anchor", PositionAnchor)            \
    X(position_area, "position-area", PositionArea)
// https://drafts.csswg.org/css-anchor-position-1/#om-position-try
class CSSPositionTryDescriptors final : public CSSStyleProperties {
    WEB_WRAPPABLE(CSSPositionTryDescriptors, CSSStyleProperties);
    GC_DECLARE_ALLOCATOR(CSSPositionTryDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSPositionTryDescriptors> create(RustDeclarationBlock);

    virtual ~CSSPositionTryDescriptors() override = default;

    virtual WebIDL::ExceptionOr<void> set_property(Utf16FlyString const& property_name, Utf16View css_text, Utf16View priority) override;

#define __ENUMERATE(name, property_name, property_id)         \
    WebIDL::ExceptionOr<void> set_##name(Utf16View css_text); \
    Utf16String name() const;
    ENUMERATE_CSS_POSITION_TRY_DESCRIPTORS(__ENUMERATE)
#undef __ENUMERATE

private:
    explicit CSSPositionTryDescriptors(RustDeclarationBlock);
};

}
