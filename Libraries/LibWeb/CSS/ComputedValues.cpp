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
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
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
static_assert(sizeof(ComputedValues::InheritedListTableValues) == sizeof(ComputedValuesFFI::InheritedListTableValues));
static_assert(alignof(ComputedValues::InheritedListTableValues) == alignof(ComputedValuesFFI::InheritedListTableValues));
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
static_assert(offsetof(ComputedValues::BorderValues, border_left) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_left));
static_assert(offsetof(ComputedValues::BorderValues, border_top) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_top));
static_assert(offsetof(ComputedValues::BorderValues, border_right) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_right));
static_assert(offsetof(ComputedValues::BorderValues, border_bottom) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_bottom));
static_assert(sizeof(ComputedValuesFFI::BorderLayoutFacts) <= offsetof(ComputedValues::BorderValues, border_left_color_style_value));
static_assert(sizeof(ComputedValues::BorderValues) == sizeof(ComputedValuesFFI::BorderValues));
static_assert(alignof(ComputedValues::BorderValues) == alignof(ComputedValuesFFI::BorderValues));
static_assert(sizeof(ComputedValues::ContentValues) == sizeof(ComputedValuesFFI::ContentValues));
static_assert(alignof(ComputedValues::ContentValues) == alignof(ComputedValuesFFI::ContentValues));
static_assert(sizeof(ComputedValues::MiscLayoutValues) == sizeof(ComputedValuesFFI::MiscLayoutValues));
static_assert(alignof(ComputedValues::MiscLayoutValues) == alignof(ComputedValuesFFI::MiscLayoutValues));
static_assert(sizeof(ComputedValues::MiscInteractionValues) == sizeof(ComputedValuesFFI::MiscInteractionValues));
static_assert(alignof(ComputedValues::MiscInteractionValues) == alignof(ComputedValuesFFI::MiscInteractionValues));
static_assert(to_underlying(ListStylePosition::Outside) == 1);

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

void const* ComputedValues::style_group_payload(StyleGroupIndex group) const
{
    switch (group) {
#define LIBWEB_STYLE_GROUP_PAYLOAD_CASE(name, path, sharing_name) \
    case StyleGroupIndex::name:                                   \
        return &*path;
        LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_PAYLOAD_CASE)
#undef LIBWEB_STYLE_GROUP_PAYLOAD_CASE
    case StyleGroupIndex::Count:
        break;
    }
    VERIFY_NOT_REACHED();
}

void ComputedValues::borrow_style_record_groups(void const* groups)
{
    VERIFY(groups);
#define LIBWEB_BORROW_STYLE_GROUP(name, path, sharing_name) \
    path.borrow(style_record_group_payload<to_underlying(StyleGroupIndex::name)>(groups));
    LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_BORROW_STYLE_GROUP)
#undef LIBWEB_BORROW_STYLE_GROUP
    m_inherited.values = style_record_inherited_values(groups);
    m_noninherited.reset_bits = style_record_reset_bits(groups);
}

bool InstalledStyle::display_is_none_ignoring_animations() const
{
    if (!m_record)
        return false;
    auto view = this->view();
    return StyleEngineFFI::style_engine_view_display_is_none_ignoring_animations(&view);
}

bool InstalledStyle::display_is_none() const
{
    if (!m_record)
        return false;
    // The record's base payloads are the ones an animation overlay was layered on top of, matching
    // what ComputedValues::base_values() exposes.
    auto view = this->view();
    auto const* groups = StyleEngineFFI::style_engine_view_base_groups(&view);
    if (!groups)
        return false;
    return display_from_ffi_display(style_record_group<ComputedValues::BoxValues>(groups).display).is_none();
}

// NB: Out of line, as what other libraries link against does not include the engine's calls the destructor makes.
ComputedStyleRecordView::~ComputedStyleRecordView() = default;

ComputedStyleRecordView::ComputedStyleRecordView(InstalledStyle style)
    : m_style(move(style))
{
    VERIFY(m_style);
    auto view = m_style.view();
    VERIFY(view.groups);
    m_values.emplace(ComputedValues::BorrowedStyleRecord::Yes);
    m_values->borrow_style_record_groups(view.groups);
    if (view.animated_overlay) {
        auto const* base_groups = StyleEngineFFI::style_engine_view_base_groups(&view);
        VERIFY(base_groups);
        m_base_values.emplace(ComputedValues::BorrowedStyleRecord::Yes);
        m_base_values->borrow_style_record_groups(base_groups);
        m_values->m_borrowed_base_values = &*m_base_values;
    }

    auto dependency_flags = static_cast<StyleRecordDependencyFlag>(view.dependency_flags);
    m_values->m_depends_on_viewport_metrics = has_flag(dependency_flags, StyleRecordDependencyFlag::DependsOnViewportMetrics);
    m_values->m_font_metrics_depend_on_viewport_metrics = has_flag(dependency_flags, StyleRecordDependencyFlag::FontMetricsDependOnViewportMetrics);
    m_values->m_in_display_none_subtree = has_flag(dependency_flags, StyleRecordDependencyFlag::InDisplayNoneSubtree);
    m_values->m_record = StyleRecordRef { view.longhand_table_record };
    if (view.animated_overlay)
        m_values->m_animated_properties = adopt_ref(*new AnimatedProperties(static_cast<ComputedValuesFFI::AnimatedOverlay const*>(view.animated_overlay)));
    if (view.animated_overlay) {
        m_base_values->m_depends_on_viewport_metrics = m_values->m_depends_on_viewport_metrics;
        m_base_values->m_font_metrics_depend_on_viewport_metrics = m_values->m_font_metrics_depend_on_viewport_metrics;
        m_base_values->m_in_display_none_subtree = m_values->m_in_display_none_subtree;
        m_base_values->m_record = m_values->m_record;
    }
}

// The table-driven build and the marshalled build must stay on one numbering with the Rust
// mirror in table_group_builder.rs.
static_assert(to_underlying(StyleGroupIndex::InheritedListTableValues) == 0);
static_assert(to_underlying(StyleGroupIndex::InheritedUIValues) == 1);
static_assert(to_underlying(StyleGroupIndex::InheritedSVGValues) == 2);
static_assert(to_underlying(StyleGroupIndex::InheritedTextValues) == 3);
static_assert(to_underlying(StyleGroupIndex::FontValues) == 4);
static_assert(to_underlying(StyleGroupIndex::SVGResetValues) == 6);
static_assert(to_underlying(StyleGroupIndex::GridValues) == 7);
static_assert(to_underlying(StyleGroupIndex::EffectsValues) == 13);
static_assert(to_underlying(StyleGroupIndex::MaskValues) == 14);
static_assert(to_underlying(StyleGroupIndex::ContentValues) == 9);
static_assert(to_underlying(StyleGroupIndex::TransformValues) == 16);
static_assert(to_underlying(StyleGroupIndex::MiscLayoutValues) == 11);
static_assert(to_underlying(StyleGroupIndex::MiscInteractionValues) == 12);
static_assert(to_underlying(StyleGroupIndex::BackgroundValues) == 17);
static_assert(to_underlying(StyleGroupIndex::BorderValues) == 18);
static_assert(to_underlying(StyleGroupIndex::AlignmentValues) == 10);
static_assert(to_underlying(StyleGroupIndex::SizingValues) == 19);
static_assert(to_underlying(StyleGroupIndex::SurroundValues) == 20);
static_assert(to_underlying(StyleGroupIndex::BoxValues) == 21);
static_assert(to_underlying(StyleGroupIndex::Count) == 22);

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

Optional<Utf16FlyString> ComputedValues::MiscInteractionValues::view_transition_name_value() const
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
        VERIFY(value->value_list.values.length() > 0);
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
    images.ensure_capacity(value->value_list.values.length());
    for (size_t i = 0; i < value->value_list.values.length(); ++i)
        images.unchecked_append(abstract_image_value(static_cast<StyleValueFFI::StyleValueData const*>(value->value_list.values.pointer[i].pointer)));
    return images;
}

RefPtr<AbstractImageStyleValue const> ComputedValues::InheritedListTableValues::list_style_image_value() const
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
        return url_from_rust_data(image->url._0->url, image->url._0->url_type, image->url._0->modifiers);
    return {};
}

Optional<URL> ComputedValues::MaskValues::clip_path_value() const
{
    auto const* value_data = static_cast<StyleValueFFI::StyleValueData const*>(clip_path.pointer);
    VERIFY(value_data);
    if (value_data->tag == StyleValueFFI::StyleValueData::Tag::Url)
        return url_from_rust_data(value_data->url._0->url, value_data->url._0->url_type, value_data->url._0->modifiers);
    return {};
}

Vector<RefPtr<AbstractImageStyleValue const>> ComputedValues::MaskValues::mask_images_value() const
{
    return abstract_image_items(mask_image);
}

ComputedValues::Statistics ComputedValues::s_statistics;

ComputedValues::ComputedValues()
{
    ++s_statistics.live_instance_count;
    ++s_statistics.total_instances_created;
}

StyleRecordRef::StyleRecordRef(void const* record)
    : m_record(record)
{
    if (m_record)
        StyleEngineFFI::style_engine_record_retain(m_record);
}

StyleRecordRef::~StyleRecordRef()
{
    if (m_record)
        StyleEngineFFI::style_engine_record_release(m_record);
}

ComputedValues::ComputedValues(BorrowedStyleRecord)
    : m_is_style_record_view(true)
{
    m_ref_count = 0;
}

ComputedValues::~ComputedValues()
{
    if (!m_is_style_record_view)
        --s_statistics.live_instance_count;
}

bool ComputedValues::is_property_important(PropertyID property_id) const
{
    if (!m_record.get())
        return false;
    auto index = property_bitmap_index(property_id);
    return ComputedValuesFFI::rust_style_record_importance_bits(m_record.get())[index / 8] & (1 << (index % 8));
}

NonnullRefPtr<ComputedValues const> ComputedValues::clone() const
{
    auto copy = adopt_ref(*new ComputedValues);
    copy->m_inherited = m_inherited;
    copy->m_noninherited = m_noninherited;
    copy->m_record = m_record;
    if (m_borrowed_base_values)
        copy->m_base_values = m_borrowed_base_values->clone();
    else
        copy->m_base_values = m_base_values;
    copy->m_animated_properties = m_animated_properties;
    copy->m_depends_on_viewport_metrics = m_depends_on_viewport_metrics;
    copy->m_font_metrics_depend_on_viewport_metrics = m_font_metrics_depend_on_viewport_metrics;
    copy->m_in_display_none_subtree = m_in_display_none_subtree;
    return copy;
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

    if (!m_record.get())
        return {};
    auto const* stored = ComputedValuesFFI::rust_style_record_longhand(m_record.get(), to_underlying(property_id));
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
