/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/InstalledStyle.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RepeatStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

// Groups whose layout is defined in Rust must not change size or alignment when the C++
// side layers initial values and accessors on top of the mirrored layout.
static_assert(sizeof(ComputedValues::InheritedBoxValues) == sizeof(ComputedValuesFFI::InheritedBoxValues));
static_assert(alignof(ComputedValues::InheritedBoxValues) == alignof(ComputedValuesFFI::InheritedBoxValues));
static_assert(sizeof(ComputedValues::InheritedTableValues) == sizeof(ComputedValuesFFI::InheritedTableValues));
static_assert(alignof(ComputedValues::InheritedTableValues) == alignof(ComputedValuesFFI::InheritedTableValues));
static_assert(sizeof(ComputedValues::SizingValues) == sizeof(ComputedValuesFFI::SizingValues));
static_assert(alignof(ComputedValues::SizingValues) == alignof(ComputedValuesFFI::SizingValues));
static_assert(sizeof(ComputedValues::AlignmentValues) == sizeof(ComputedValuesFFI::AlignmentValues));
static_assert(alignof(ComputedValues::AlignmentValues) == alignof(ComputedValuesFFI::AlignmentValues));
static_assert(sizeof(ComputedValues::SVGResetValues) == sizeof(ComputedValuesFFI::SVGResetValues));
static_assert(alignof(ComputedValues::SVGResetValues) == alignof(ComputedValuesFFI::SVGResetValues));
static_assert(sizeof(ComputedValues::SurroundValues) == sizeof(ComputedValuesFFI::SurroundValues));
static_assert(alignof(ComputedValues::SurroundValues) == alignof(ComputedValuesFFI::SurroundValues));
static_assert(sizeof(ComputedValues::BoxValues) == sizeof(ComputedValuesFFI::BoxValues));
static_assert(alignof(ComputedValues::BoxValues) == alignof(ComputedValuesFFI::BoxValues));
static_assert(sizeof(ComputedValues::TransformValues) == sizeof(ComputedValuesFFI::TransformValues));
static_assert(alignof(ComputedValues::TransformValues) == alignof(ComputedValuesFFI::TransformValues));
static_assert(sizeof(ComputedValues::EffectsValues) == sizeof(ComputedValuesFFI::EffectsValues));
static_assert(alignof(ComputedValues::EffectsValues) == alignof(ComputedValuesFFI::EffectsValues));
static_assert(sizeof(ComputedValues::AnchorValues) == sizeof(ComputedValuesFFI::AnchorValues));
static_assert(alignof(ComputedValues::AnchorValues) == alignof(ComputedValuesFFI::AnchorValues));
static_assert(sizeof(ComputedValues::InheritedUIValues) == sizeof(ComputedValuesFFI::InheritedUIValues));
static_assert(alignof(ComputedValues::InheritedUIValues) == alignof(ComputedValuesFFI::InheritedUIValues));
static_assert(sizeof(ComputedValues::InheritedSVGValues) == sizeof(ComputedValuesFFI::InheritedSVGValues));
static_assert(alignof(ComputedValues::InheritedSVGValues) == alignof(ComputedValuesFFI::InheritedSVGValues));
static_assert(to_underlying(FillRule::Nonzero) == 0);
static_assert(to_underlying(StrokeLinecap::Butt) == 0);
static_assert(to_underlying(StrokeLinejoin::Miter) == 0);
static_assert(to_underlying(ColorInterpolation::Auto) == 0);
static_assert(to_underlying(ColorInterpolation::Linearrgb) == 1);
static_assert(to_underlying(PaintOrder::Fill) == 0);
static_assert(to_underlying(PaintOrder::Stroke) == 1);
static_assert(to_underlying(PaintOrder::Markers) == 2);
static_assert(to_underlying(TextAnchor::Start) == 0);
static_assert(to_underlying(ShapeRendering::Auto) == 0);
static_assert(sizeof(Size) == sizeof(ComputedValuesFFI::ComputedSize));
static_assert(alignof(Size) == alignof(ComputedValuesFFI::ComputedSize));
static_assert(sizeof(RustStyleValueHandle) == sizeof(StyleValueFFI::StyleValueData const*));
static_assert(alignof(RustStyleValueHandle) == alignof(StyleValueFFI::StyleValueData const*));

// The Rust border group's four leading side facts share BorderData's layout.
static_assert(sizeof(Gfx::Color) == sizeof(u32));
static_assert(sizeof(ShadowData) == sizeof(ComputedValuesFFI::ComputedShadow));
static_assert(alignof(ShadowData) == alignof(ComputedValuesFFI::ComputedShadow));
static_assert(offsetof(ShadowData, offset_x) == offsetof(ComputedValuesFFI::ComputedShadow, offset_x));
static_assert(offsetof(ShadowData, offset_y) == offsetof(ComputedValuesFFI::ComputedShadow, offset_y));
static_assert(offsetof(ShadowData, blur_radius) == offsetof(ComputedValuesFFI::ComputedShadow, blur_radius));
static_assert(offsetof(ShadowData, spread_distance) == offsetof(ComputedValuesFFI::ComputedShadow, spread_distance));
static_assert(offsetof(ShadowData, color) == offsetof(ComputedValuesFFI::ComputedShadow, color));
static_assert(offsetof(ShadowData, color_syntax) == offsetof(ComputedValuesFFI::ComputedShadow, color_syntax));
static_assert(offsetof(ShadowData, placement) == offsetof(ComputedValuesFFI::ComputedShadow, placement));
static_assert(to_underlying(ColorSyntax::Legacy) == 0);
static_assert(to_underlying(ColorSyntax::Modern) == 1);
static_assert(to_underlying(ShadowPlacement::Outer) == 0);
static_assert(to_underlying(ShadowPlacement::Inner) == 1);
static_assert(to_underlying(MixBlendMode::Normal) == 0);
static_assert(to_underlying(Isolation::Auto) == 0);
static_assert(sizeof(LineStyle) == sizeof(u8));
static_assert(sizeof(BorderData) == sizeof(ComputedValuesFFI::ComputedBorderSide));
static_assert(offsetof(BorderData, color) == offsetof(ComputedValuesFFI::ComputedBorderSide, color));
static_assert(offsetof(BorderData, line_style) == offsetof(ComputedValuesFFI::ComputedBorderSide, line_style));
static_assert(offsetof(BorderData, width) == offsetof(ComputedValuesFFI::ComputedBorderSide, width));
static_assert(offsetof(ComputedValues::BorderValues, border_left) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_left));
static_assert(offsetof(ComputedValues::BorderValues, border_top) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_top));
static_assert(offsetof(ComputedValues::BorderValues, border_right) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_right));
static_assert(offsetof(ComputedValues::BorderValues, border_bottom) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_bottom));
static_assert(sizeof(ComputedValuesFFI::BorderLayoutFacts) <= offsetof(ComputedValues::BorderValues, border_left_color_style_value));
static_assert(sizeof(ComputedValues::BorderValues) == sizeof(ComputedValuesFFI::BorderValues));
static_assert(alignof(ComputedValues::BorderValues) == alignof(ComputedValuesFFI::BorderValues));
static_assert(sizeof(ComputedValues::ContentValues) == sizeof(ComputedValuesFFI::ContentValues));
static_assert(alignof(ComputedValues::ContentValues) == alignof(ComputedValuesFFI::ContentValues));
static_assert(sizeof(ComputedValues::InheritedListValues) == sizeof(ComputedValuesFFI::InheritedListValues));
static_assert(alignof(ComputedValues::InheritedListValues) == alignof(ComputedValuesFFI::InheritedListValues));
static_assert(sizeof(ComputedValues::MiscResetValues) == sizeof(ComputedValuesFFI::MiscResetValues));
static_assert(alignof(ComputedValues::MiscResetValues) == alignof(ComputedValuesFFI::MiscResetValues));
static_assert(to_underlying(ListStylePosition::Outside) == 1);

static_assert(sizeof(TextIndentData) == sizeof(ComputedValuesFFI::ComputedTextIndent));
static_assert(offsetof(TextIndentData, length_percentage) == offsetof(ComputedValuesFFI::ComputedTextIndent, length_percentage));
static_assert(offsetof(TextIndentData, each_line) == offsetof(ComputedValuesFFI::ComputedTextIndent, each_line));
static_assert(offsetof(TextIndentData, hanging) == offsetof(ComputedValuesFFI::ComputedTextIndent, hanging));
static_assert(sizeof(ComputedValues::InheritedTextValues) == sizeof(ComputedValuesFFI::InheritedTextValues));
static_assert(alignof(ComputedValues::InheritedTextValues) == alignof(ComputedValuesFFI::InheritedTextValues));
static_assert(sizeof(ComputedValues::AnimationValues) == sizeof(ComputedValuesFFI::AnimationValues));
static_assert(alignof(ComputedValues::AnimationValues) == alignof(ComputedValuesFFI::AnimationValues));

static_assert(sizeof(ComputedValues::FontValues) == sizeof(ComputedValuesFFI::FontValues));
static_assert(alignof(ComputedValues::FontValues) == alignof(ComputedValuesFFI::FontValues));
static_assert(to_underlying(FontVariantEmoji::Normal) == 0);
static_assert(to_underlying(FontVariantEmoji::Text) == 1);
static_assert(to_underlying(FontVariantEmoji::Emoji) == 2);
static_assert(to_underlying(FontVariantEmoji::Unicode) == 3);
static_assert(to_underlying(MathShift::Normal) == 0);
static_assert(to_underlying(MathShift::Compact) == 1);
static_assert(to_underlying(MathStyle::Normal) == 0);
static_assert(to_underlying(MathStyle::Compact) == 1);

bool ComputedValues::layout_affecting_group_payloads_differ(void const* const* a, void const* const* b)
{
    auto differs = [&]<typename T>() {
        auto const* mine = static_cast<T const*>(a[T::style_group_index]);
        auto const* theirs = static_cast<T const*>(b[T::style_group_index]);
        return mine != theirs && !(*mine == *theirs);
    };
#define LIBWEB_COMPARE_STYLE_GROUP_PAYLOAD(name, path, sharing_name, affects_layout) \
    if constexpr (affects_layout) {                                                  \
        if (differs.template operator()<name>())                                     \
            return true;                                                             \
    }
    LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_COMPARE_STYLE_GROUP_PAYLOAD)
#undef LIBWEB_COMPARE_STYLE_GROUP_PAYLOAD
    return false;
}

void const* ComputedValues::style_group_payload(StyleGroupIndex group) const
{
    switch (group) {
#define LIBWEB_STYLE_GROUP_PAYLOAD_CASE(name, path, sharing_name, affects_layout) \
    case StyleGroupIndex::name:                                                   \
        return &*path;
        LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_PAYLOAD_CASE)
#undef LIBWEB_STYLE_GROUP_PAYLOAD_CASE
    case StyleGroupIndex::Count:
        break;
    }
    VERIFY_NOT_REACHED();
}

void ComputedValues::borrow_style_record_payloads(ReadonlySpan<void const*> payloads)
{
    VERIFY(payloads.size() == to_underlying(StyleGroupIndex::Count));
    size_t index = 0;
#define LIBWEB_BORROW_STYLE_GROUP(path) path.borrow(payloads[index++]);
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.table)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.list)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.ui)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.svg)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.text)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.box)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.font)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.animation)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.svg_reset)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.grid)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.anchor)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.effects)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.mask_data)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.text_reset)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.content_data)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.transform)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.background)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.border)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.alignment)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.misc)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.sizing)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.surround)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.box)
#undef LIBWEB_BORROW_STYLE_GROUP
    VERIFY(index == payloads.size());
}

bool InstalledStyle::display_is_none() const
{
    if (!m_view.present)
        return false;
    // The record's base payloads are the ones an animation overlay was layered on top of, matching
    // what ComputedValues::base_values() exposes.
    auto const* payloads = m_view.base_payloads ? m_view.base_payloads : m_view.payloads;
    if (!payloads)
        return false;
    auto const* box = static_cast<ComputedValuesFFI::BoxValues const*>(payloads[to_underlying(StyleGroupIndex::BoxValues)]);
    if (!box)
        return false;
    return display_from_ffi_display(box->display).is_none();
}

ComputedStyleRecordView::ComputedStyleRecordView(StyleEngineFFI::FfiStyleRecordView const& view, StyleComputer const& style_computer, StyleRecordID style_record_identity, bool owns_style_record_pin)
    : m_style_computer(&style_computer)
    , m_style_record_identity(style_record_identity)
    , m_owns_style_record_pin(owns_style_record_pin)
{
    VERIFY(view.present);
    VERIFY(style_record_identity);
    VERIFY(view.payload_count == to_underlying(StyleGroupIndex::Count));
    VERIFY(view.payloads);
    VERIFY(view.base_payloads);
    auto payloads = ReadonlySpan<void const*> { view.payloads, view.payload_count };
    auto base_payloads = ReadonlySpan<void const*> { view.base_payloads, view.payload_count };
    m_values.borrow_style_record_payloads(payloads);
    if (view.animation_overlay_identity != 0) {
        m_base_values.emplace(ComputedValues::BorrowedStyleRecord::Yes);
        m_base_values->borrow_style_record_payloads(base_payloads);
        m_values.m_borrowed_base_values = &*m_base_values;
    }

    m_values.m_pseudo_element_styles = view.pseudo_element_styles;
    auto dependency_flags = static_cast<StyleRecordDependencyFlag>(view.dependency_flags);
    m_values.m_depends_on_viewport_metrics = has_flag(dependency_flags, StyleRecordDependencyFlag::DependsOnViewportMetrics);
    m_values.m_font_metrics_depend_on_viewport_metrics = has_flag(dependency_flags, StyleRecordDependencyFlag::FontMetricsDependOnViewportMetrics);
    m_values.m_in_display_none_subtree = has_flag(dependency_flags, StyleRecordDependencyFlag::InDisplayNoneSubtree);
    m_values.m_highlight_colors_authored = has_flag(dependency_flags, StyleRecordDependencyFlag::HighlightColorsAuthored);
    m_values.m_highlight_color_is_current_color = has_flag(dependency_flags, StyleRecordDependencyFlag::HighlightColorIsCurrentColor);
    m_values.m_computed_longhand_table = view.longhand_table;
    if (m_values.m_computed_longhand_table)
        m_values.refresh_computed_longhand_table_views();
    if (view.animated_overlay)
        m_values.m_animated_properties = adopt_ref(*new AnimatedProperties(static_cast<ComputedValuesFFI::AnimatedOverlay const*>(view.animated_overlay)));
    if (view.animation_overlay_identity != 0) {
        m_base_values->m_property_important = m_values.m_property_important;
        m_base_values->m_property_inherited = m_values.m_property_inherited;
        m_base_values->m_pseudo_element_styles = m_values.m_pseudo_element_styles;
        m_base_values->m_depends_on_viewport_metrics = m_values.m_depends_on_viewport_metrics;
        m_base_values->m_font_metrics_depend_on_viewport_metrics = m_values.m_font_metrics_depend_on_viewport_metrics;
        m_base_values->m_in_display_none_subtree = m_values.m_in_display_none_subtree;
        m_base_values->m_highlight_colors_authored = m_values.m_highlight_colors_authored;
        m_base_values->m_highlight_color_is_current_color = m_values.m_highlight_color_is_current_color;
        m_base_values->m_inheritance_dependent_specified_values = m_values.m_inheritance_dependent_specified_values;
        m_base_values->m_computed_longhand_table = m_values.m_computed_longhand_table;
        if (m_base_values->m_computed_longhand_table)
            m_base_values->refresh_computed_longhand_table_views();
    }
    m_present = true;
}

ComputedStyleRecordView::~ComputedStyleRecordView()
{
    if (m_style_computer && m_owns_style_record_pin)
        m_style_computer->unpin_style_record(m_style_record_identity);
}

// The table-driven build and the marshalled build must stay on one numbering with the Rust
// mirror in table_group_builder.rs.
static_assert(to_underlying(StyleGroupIndex::InheritedTableValues) == 0);
static_assert(to_underlying(StyleGroupIndex::InheritedListValues) == 1);
static_assert(to_underlying(StyleGroupIndex::InheritedUIValues) == 2);
static_assert(to_underlying(StyleGroupIndex::InheritedSVGValues) == 3);
static_assert(to_underlying(StyleGroupIndex::InheritedTextValues) == 4);
static_assert(to_underlying(StyleGroupIndex::InheritedBoxValues) == 5);
static_assert(to_underlying(StyleGroupIndex::FontValues) == 6);
static_assert(to_underlying(StyleGroupIndex::SVGResetValues) == 8);
static_assert(to_underlying(StyleGroupIndex::GridValues) == 9);
static_assert(to_underlying(StyleGroupIndex::EffectsValues) == 11);
static_assert(to_underlying(StyleGroupIndex::MaskValues) == 12);
static_assert(to_underlying(StyleGroupIndex::ContentValues) == 14);
static_assert(to_underlying(StyleGroupIndex::TransformValues) == 15);
static_assert(to_underlying(StyleGroupIndex::MiscResetValues) == 19);
static_assert(to_underlying(StyleGroupIndex::BackgroundValues) == 16);
static_assert(to_underlying(StyleGroupIndex::BorderValues) == 17);
static_assert(to_underlying(StyleGroupIndex::AlignmentValues) == 18);
static_assert(to_underlying(StyleGroupIndex::SizingValues) == 20);
static_assert(to_underlying(StyleGroupIndex::SurroundValues) == 21);
static_assert(to_underlying(StyleGroupIndex::BoxValues) == 22);
static_assert(to_underlying(StyleGroupIndex::Count) == 23);

// The enum codes the core's transform and effects lowering mirrors.
static_assert(to_underlying(TransformBox::ViewBox) == 4);
static_assert(to_underlying(TransformStyle::Flat) == 0);
static_assert(to_underlying(BackfaceVisibility::Visible) == 0);
static_assert(to_underlying(FilterStyleValue::Kind::Blur) == 0);
static_assert(to_underlying(FilterStyleValue::Kind::DropShadow) == 1);
static_assert(to_underlying(FilterStyleValue::Kind::HueRotate) == 2);
static_assert(to_underlying(FilterStyleValue::Kind::Color) == 3);
static_assert(to_underlying(ColorStyleValue::ColorType::RGB) == 0);
static_assert(to_underlying(ColorStyleValue::ColorType::HSL) == 4);
static_assert(to_underlying(ColorStyleValue::ColorType::HWB) == 5);
static_assert(to_underlying(ColorSyntax::Legacy) == 0);
static_assert(to_underlying(ColorSyntax::Modern) == 1);
static_assert(to_underlying(StyleValueList::Separator::Space) == 0);
static_assert(to_underlying(StyleValueList::Separator::Comma) == 1);
static_assert(to_underlying(TimeUnit::Ms) == 0);
static_assert(to_underlying(TimeUnit::S) == 1);
static_assert(to_underlying(PaintOrder::Fill) == 0);
static_assert(to_underlying(PaintOrder::Stroke) == 1);
static_assert(to_underlying(PaintOrder::Markers) == 2);

static NonnullRefPtr<StyleValue const> animation_style_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    VERIFY(handle.pointer);
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
        static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer)));
}

Gfx::FontCascadeList const& ComputedValues::FontValues::font_list_value() const
{
    VERIFY(font_cascade_list.pointer);
    return *static_cast<Gfx::FontCascadeList const*>(font_cascade_list.pointer);
}

FontStyleKeyword ComputedValues::FontValues::font_style_keyword() const
{
    if (!font_style.pointer)
        return FontStyleKeyword::Normal;
    return animation_style_value(font_style)->as_font_style().font_style();
}

Optional<Utf16FlyString> ComputedValues::MiscResetValues::view_transition_name_value() const
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(view_transition_name.pointer);
    VERIFY(value);
    if (value->tag != StyleValueFFI::StyleValueData::Tag::CustomIdent)
        return {};
    return css_string_from_rust(&value->custom_ident.custom_ident);
}

static StyleValueFFI::StyleValueData const* first_animation_item_data(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer);
    VERIFY(value);
    if (value->tag == StyleValueFFI::StyleValueData::Tag::ValueList
        && value->value_list.separator == to_underlying(StyleValueList::Separator::Comma)) {
        VERIFY(value->value_list.values.length > 0);
        return static_cast<StyleValueFFI::StyleValueData const*>(value->value_list.values.pointer[0].pointer);
    }
    return value;
}

static RefPtr<AbstractImageStyleValue const> abstract_image_value(StyleValueFFI::StyleValueData const* image_data)
{
    VERIFY(image_data);
    if (!AK::first_is_one_of(image_data->tag,
            StyleValueFFI::StyleValueData::Tag::Image,
            StyleValueFFI::StyleValueData::Tag::ImageSet,
            StyleValueFFI::StyleValueData::Tag::LinearGradient,
            StyleValueFFI::StyleValueData::Tag::ConicGradient,
            StyleValueFFI::StyleValueData::Tag::RadialGradient))
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(image_data))->as_abstract_image();
}

// One entry per layer of a comma-separated image list, null where the layer has no image.
static Vector<RefPtr<AbstractImageStyleValue const>> abstract_image_items(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer);
    VERIFY(value);
    if (value->tag != StyleValueFFI::StyleValueData::Tag::ValueList
        || value->value_list.separator != to_underlying(StyleValueList::Separator::Comma))
        return { abstract_image_value(value) };
    Vector<RefPtr<AbstractImageStyleValue const>> images;
    images.ensure_capacity(value->value_list.values.length);
    for (size_t i = 0; i < value->value_list.values.length; ++i)
        images.unchecked_append(abstract_image_value(static_cast<StyleValueFFI::StyleValueData const*>(value->value_list.values.pointer[i].pointer)));
    return images;
}

RefPtr<AbstractImageStyleValue const> ComputedValues::InheritedListValues::list_style_image_value() const
{
    auto value = animation_style_value(list_style_image);
    if (!value->is_abstract_image())
        return nullptr;
    return value->as_abstract_image();
}

NonnullRefPtr<StyleValue const> ComputedValues::ContentValues::computed_content_value() const
{
    return animation_style_value(content);
}

bool ComputedValues::ContentValues::content_is_normal() const
{
    auto value = animation_style_value(content);
    return value->is_keyword() && value->to_keyword() == Keyword::Normal;
}

RefPtr<AbstractImageStyleValue const> ComputedValues::BorderValues::border_image_source_value() const
{
    return abstract_image_value(static_cast<StyleValueFFI::StyleValueData const*>(border_image_source.pointer));
}

Vector<RefPtr<AbstractImageStyleValue const>> ComputedValues::BackgroundValues::background_images_value() const
{
    return abstract_image_items(background_image);
}

Optional<URL> ComputedValues::MaskValues::mask_url_value() const
{
    auto const* image = first_animation_item_data(mask_image);
    if (image->tag == StyleValueFFI::StyleValueData::Tag::Url)
        return url_from_rust_data(image->url.url, image->url.url_type, image->url.modifiers);
    return {};
}

MaskType ComputedValues::MaskValues::mask_type_value() const
{
    return keyword_to_mask_type(animation_style_value(mask_type)->to_keyword()).release_value();
}

Optional<URL> ComputedValues::MaskValues::clip_path_value() const
{
    auto const* value_data = static_cast<StyleValueFFI::StyleValueData const*>(clip_path.pointer);
    VERIFY(value_data);
    if (value_data->tag == StyleValueFFI::StyleValueData::Tag::Url)
        return url_from_rust_data(value_data->url.url, value_data->url.url_type, value_data->url.modifiers);
    return {};
}

Vector<RefPtr<AbstractImageStyleValue const>> ComputedValues::MaskValues::mask_images_value() const
{
    return abstract_image_items(mask_image);
}

Vector<Utf16FlyString> ComputedValues::AnimationValues::animation_names_value() const
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(animation_name.pointer);
    VERIFY(value && value->tag == StyleValueFFI::StyleValueData::Tag::ValueList);
    auto const& items = value->value_list.values;
    Vector<Utf16FlyString> names;
    for (size_t i = 0; i < items.length; ++i) {
        auto const* item = static_cast<StyleValueFFI::StyleValueData const*>(items.pointer[i].pointer);
        switch (item->tag) {
        case StyleValueFFI::StyleValueData::Tag::Keyword:
            VERIFY(static_cast<Keyword>(item->keyword.keyword) == Keyword::None);
            break;
        case StyleValueFFI::StyleValueData::Tag::String:
            names.append(css_string_from_rust(&item->string.string));
            break;
        case StyleValueFFI::StyleValueData::Tag::CustomIdent:
            names.append(css_string_from_rust(&item->custom_ident.custom_ident));
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    return names;
}

NonnullRefPtr<ComputedValues const> ComputedValues::create(ComputedStyleWorkingSet const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context, ComputedValues const* inherit_parent)
{
    Builder builder;
    auto& computed_values = *builder.operator->();

    // NOTE: color-scheme must resolve first to ensure system colors can be resolved correctly,
    //       and the element's own color right after it, so currentColor can resolve in every
    //       other property (e.g. background-color). Both resolve against the caller's context,
    //       so resolving them up front is order-equivalent to the setters below.
    auto color_scheme = computed_style.color_scheme(document.page().preferred_color_scheme(), document.supported_color_schemes());
    color_resolution_context.color_scheme = color_scheme;
    // FIXME: We should resolve colors to their absolute forms at compute time (i.e. by implementing the relevant absolutized methods)
    auto color = computed_style.color(CSS::PropertyID::Color, color_resolution_context);
    color_resolution_context.current_color = color;

    // Build every group payload the core can map straight from the drive's longhand table.
    auto const* longhand_table = computed_style.computed_longhand_table();
    auto animated_properties = computed_style.animated_properties_snapshot();
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_context_storage;
    auto ffi_color_input = make_rust_color_resolution_input(color_resolution_context, length_context_storage);
    auto font_group_inputs = computed_style.font_group_build_inputs(document, style_scope.style_engine_tree_scope());
    ComputedValuesFFI::FfiTableGroupBuildInputs table_build_inputs {
        .color_input = &ffi_color_input,
        .used_color_scheme = static_cast<u8>(to_underlying(color_scheme)),
        .animated_overlay = animated_properties ? animated_properties->overlay() : nullptr,
        .box_display_before_transformation_raw = bit_cast<u32>(computed_style.display_before_box_type_transformation()),
        .font = &font_group_inputs,
    };
    Array<void const*, to_underlying(StyleGroupIndex::Count)> parent_group_payloads {};
    if (inherit_parent) {
        for (size_t group = 0; group < parent_group_payloads.size(); ++group)
            parent_group_payloads[group] = inherit_parent->style_group_payload(static_cast<StyleGroupIndex>(group));
    }
    Array<void const*, to_underlying(StyleGroupIndex::Count)> table_group_payloads {};
    ComputedValuesFFI::rust_build_group_payloads_from_table(longhand_table, all_style_groups, parent_group_payloads.data(), &table_build_inputs, table_group_payloads.data(), table_group_payloads.size());
    computed_values.adopt_style_group_payloads(table_group_payloads);
    computed_values.set_property_flag_bitmaps(computed_style.property_importance_bitmap(), computed_style.property_inheritance_bitmap());
    computed_values.set_depends_on_viewport_metrics(computed_style.depends_on_viewport_metrics());
    computed_values.set_font_metrics_depend_on_viewport_metrics(computed_style.font_metrics_depend_on_viewport_metrics());
    computed_values.set_in_display_none_subtree(computed_style.in_display_none_subtree());
    computed_values.set_highlight_colors_authored(computed_style.highlight_colors_authored());
    computed_values.set_highlight_color_is_current_color(computed_style.highlight_color_is_current_color());
    u64 pseudo_element_styles = 0;
    for (auto i = 0; i < to_underlying(PseudoElement::KnownPseudoElementCount); ++i) {
        auto pseudo_element = static_cast<PseudoElement>(i);
        if (computed_style.has_pseudo_element_style(pseudo_element))
            pseudo_element_styles |= 1ull << i;
    }
    computed_values.set_pseudo_element_styles(pseudo_element_styles);
    computed_values.set_computed_longhand_table(computed_style.computed_longhand_table());

    return move(builder).build();
}

ComputedValues::Statistics ComputedValues::s_statistics;

ComputedValues::ComputedValues()
{
    ++s_statistics.live_instance_count;
    ++s_statistics.total_instances_created;
}

ComputedValues::ComputedValues(BorrowedStyleRecord)
    : m_is_style_record_view(true)
{
    m_ref_count = 0;
}

ComputedValues::~ComputedValues()
{
    if (m_is_style_record_view)
        m_computed_longhand_table = nullptr;
    else {
        clear_computed_longhand_table();
        --s_statistics.live_instance_count;
    }
}

void ComputedValues::adopt_computed_longhand_table(void const* table)
{
    if (!table) {
        clear_computed_longhand_table();
        return;
    }
    // NB: Retain before releasing, so adopting the table this style already holds stays safe.
    auto const* typed_table = ComputedValuesFFI::rust_computed_longhand_table_retain(static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(table));
    clear_computed_longhand_table();
    m_computed_longhand_table = typed_table;
    refresh_computed_longhand_table_views();
}

void ComputedValues::refresh_computed_longhand_table_views()
{
    VERIFY(m_computed_longhand_table);
    auto const* table = static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(m_computed_longhand_table);
    m_longhand_values = { ComputedValuesFFI::rust_computed_longhand_table_values(table), number_of_longhand_properties };
    m_property_important.copy_from({ ComputedValuesFFI::rust_computed_longhand_table_importance_bits(table), m_property_important.size_in_bytes() });
    m_property_inherited.copy_from({ ComputedValuesFFI::rust_computed_longhand_table_inheritance_bits(table), m_property_inherited.size_in_bytes() });
    size_t inheritance_dependent_value_count = 0;
    auto const* inheritance_dependent_values = ComputedValuesFFI::rust_computed_longhand_table_inheritance_dependent_values(table, &inheritance_dependent_value_count);
    m_inheritance_dependent_specified_values = { inheritance_dependent_values, inheritance_dependent_value_count };
}

void ComputedValues::clear_computed_longhand_table()
{
    if (m_computed_longhand_table)
        ComputedValuesFFI::rust_computed_longhand_table_release(const_cast<ComputedValuesFFI::ComputedLonghandTable*>(static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(m_computed_longhand_table)));
    m_computed_longhand_table = nullptr;
    m_longhand_values = {};
    m_inheritance_dependent_specified_values = {};
}

void ComputedValues::copy_computed_longhand_table_from(ComputedValues const& other)
{
    if (other.m_computed_longhand_table) {
        adopt_computed_longhand_table(other.m_computed_longhand_table);
        return;
    }
    clear_computed_longhand_table();
    if (other.m_longhand_values.is_empty())
        return;
    auto* table = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_copy_from_values(table, other.m_longhand_values.data(), other.m_longhand_values.size());
    for (auto const& entry : other.m_inheritance_dependent_specified_values)
        ComputedValuesFFI::rust_computed_longhand_table_add_inheritance_dependent_value(table, entry.property, entry.value);
    ComputedValuesFFI::rust_computed_longhand_table_freeze(table);
    // The freshly created table already carries the one reference this style owns.
    m_computed_longhand_table = table;
    refresh_computed_longhand_table_views();
}

void ComputedValues::Mutator::set_animated_properties(AnimatedProperties const* value)
{
    m_values.m_animated_properties = value;
}

RefPtr<StyleValue const> ComputedValues::style_value_from_handle(PropertyID property_id, RustStyleValueHandle const& handle) const
{
    if (!handle) {
        if (m_style_value_cache)
            m_style_value_cache->remove(property_id);
        return nullptr;
    }
    if (m_style_value_cache) {
        if (auto it = m_style_value_cache->find(property_id); it != m_style_value_cache->end() && it->value->rust_style_value_data() == handle.data())
            return it->value;
    }
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(handle.data()));
    count_longhand_wrapper_mint();
    if (!m_style_value_cache)
        m_style_value_cache = make<HashMap<PropertyID, NonnullRefPtr<StyleValue const>>>();
    m_style_value_cache->set(property_id, value);
    return value;
}

RustStyleValueHandle const* ComputedValues::stored_style_value_handle(PropertyID property_id) const
{
    auto from_ffi_handle = [](ComputedValuesFFI::ComputedStyleValueHandle const& handle) -> RustStyleValueHandle const* {
        static_assert(sizeof(RustStyleValueHandle) == sizeof(handle));
        return reinterpret_cast<RustStyleValueHandle const*>(&handle);
    };
    auto non_empty = [](RustStyleValueHandle const* handle) -> RustStyleValueHandle const* {
        return (handle && *handle) ? handle : nullptr;
    };
    switch (property_id) {
    case PropertyID::Cx:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->cx));
    case PropertyID::Cy:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->cy));
    case PropertyID::D:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->d));
    case PropertyID::GridAutoColumns:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_auto_columns_style_value));
    case PropertyID::GridAutoRows:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_auto_rows_style_value));
    case PropertyID::GridColumnEnd:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_column_end_style_value));
    case PropertyID::GridColumnStart:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_column_start_style_value));
    case PropertyID::GridRowEnd:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_row_end_style_value));
    case PropertyID::GridRowStart:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_row_start_style_value));
    case PropertyID::GridTemplateAreas:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_template_areas_style_value));
    case PropertyID::GridTemplateColumns:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_template_columns_style_value));
    case PropertyID::GridTemplateRows:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_template_rows_style_value));
    case PropertyID::LetterSpacing:
        return non_empty(from_ffi_handle(m_inherited.text->letter_spacing_style_value));
    case PropertyID::R:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->r));
    case PropertyID::Rx:
        if (m_noninherited.svg_reset->rx.is_auto)
            return nullptr;
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->rx.value));
    case PropertyID::Ry:
        if (m_noninherited.svg_reset->ry.is_auto)
            return nullptr;
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->ry.value));
    case PropertyID::WordSpacing:
        return non_empty(from_ffi_handle(m_inherited.text->word_spacing_style_value));
    case PropertyID::X:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->x));
    case PropertyID::Y:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->y));
    default:
        return nullptr;
    }
}

RefPtr<StyleValue const> ComputedValues::color_style_value() const
{
    if (m_inherited.text->color_style_value.pointer) {
        auto handle = RustStyleValueHandle::retained(
            static_cast<StyleValueFFI::StyleValueData const*>(m_inherited.text->color_style_value.pointer));
        return style_value_from_handle(PropertyID::Color, handle);
    }
    return computed_style_value(PropertyID::Color);
}

RefPtr<StyleValue const> ComputedValues::background_color_style_value() const
{
    auto const& handle = m_noninherited.background->background_color_style_value;
    static_assert(sizeof(RustStyleValueHandle) == sizeof(handle));
    return style_value_from_handle(PropertyID::BackgroundColor, reinterpret_cast<RustStyleValueHandle const&>(handle));
}

RefPtr<StyleValue const> ComputedValues::computed_style_value(PropertyID property_id, WithAnimationsApplied with_animations_applied) const
{
    if (with_animations_applied == WithAnimationsApplied::No && has_animated_values())
        return base_values().computed_style_value(property_id);

    if (property_is_logical_alias(property_id))
        property_id = map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { writing_mode(), direction() });

    if (property_id < first_longhand_property_id || property_id > last_longhand_property_id)
        return {};

    if (auto inset = anchor_inset(property_id))
        return inset;

    // The animated overlay first, under the overlay read rule the Rust side implements once:
    // important base values override animated but not transitioned properties.
    if (with_animations_applied == WithAnimationsApplied::Yes && m_animated_properties
        && ComputedValuesFFI::rust_animated_overlay_effective_value(m_animated_properties->overlay(), to_underlying(property_id), is_property_important(property_id)))
        return m_animated_properties->property(property_id);

    if (m_longhand_values.is_empty())
        return {};
    auto const* stored = m_longhand_values[to_underlying(property_id) - to_underlying(first_longhand_property_id)];
    if (!stored)
        return {};
    if (m_style_value_cache) {
        if (auto it = m_style_value_cache->find(property_id); it != m_style_value_cache->end() && it->value->rust_style_value_data() == stored)
            return it->value;
    }
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(stored)));
    count_longhand_wrapper_mint();
    if (!m_style_value_cache)
        m_style_value_cache = make<HashMap<PropertyID, NonnullRefPtr<StyleValue const>>>();
    m_style_value_cache->set(property_id, value);
    return value;
}

}
