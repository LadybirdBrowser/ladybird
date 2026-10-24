/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2023-2026, Callum Law <callumlaw1709@outlook.com>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibCompositing/RustFFI.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/FontCascadeList.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontResolution.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/Canvas/PredefinedColorSpace.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWebCommon/Bindings/PredefinedColorSpace.h>

namespace Web::HTML {

namespace {

// `ContextCreationArguments` of Bindings/Rust/src/canvas_2d/exports.rs.
struct ContextCreationArguments {
    void* canvas { nullptr };
    void* canvas_host { nullptr };
    bool canvas_is_offscreen { false };
    i32 width { 0 };
    i32 height { 0 };
    bool alpha { true };
    bool desynchronized { false };
    Bindings::PredefinedColorSpace color_space { Bindings::PredefinedColorSpace::Srgb };
    Bindings::CanvasColorType color_type { Bindings::CanvasColorType::Unorm8 };
    bool will_read_frequently { false };
};

// `FontMetricsSource` of Bindings/Rust/src/canvas_2d/canvas.rs: the font of the context, if it has one yet.
struct FontMetricsSource {
    Gfx::FontCascadeList const* cascade { nullptr };
    // The size of the font, as the raw value of a CSSPixels.
    i32 font_size { 0 };
};

// `ResolvedFontAbi` of Bindings/Rust/src/canvas_2d/canvas.rs.
struct ResolvedFont {
    // A Gfx::FontCascadeList with a reference that Rust takes over.
    Gfx::FontCascadeList const* cascade { nullptr };
    u64 environment_generation { 0 };
    i32 font_size { 0 };
    // An AK::Utf16String that Rust takes over.
    FlatPtr serialized { 0 };
};

// `FontSelection` of Bindings/Rust/src/canvas_2d/canvas.rs: the text drawing styles of a context that select the fonts
// of its text beside its font, by the indices of their values in their IDL enumerations.
struct CanvasFontSelection {
    u8 font_kerning { 0 };
    u8 font_stretch { 4 };
    u8 font_variant_caps { 0 };
    u8 text_rendering { 0 };
};

// `CanvasImageSourceKind` of Bindings/Rust/src/canvas_2d/canvas.rs: the sources of images that are implemented in C++.
enum class CanvasImageSourceKind : u8 {
    HTMLImageElement,
    SVGImageElement,
    HTMLVideoElement,
    HTMLCanvasElement,
    OffscreenCanvas,
};

// `CapturedVideoFrameAbi` of Bindings/Rust/src/canvas_2d/canvas.rs.
struct CapturedVideoFrame {
    // A Compositing::VideoFrameHold with a reference that Rust takes over.
    Compositing::VideoFrameHold* hold { nullptr };
    u64 sink_handle { 0 };
    u64 generation { 0 };
};

}

// NB: The color space and the color type cross the bridge as their underlying values, which canvas_2d/exports.rs
//     reads in this order.
static_assert(sizeof(Bindings::PredefinedColorSpace) == sizeof(u8));
static_assert(to_underlying(Bindings::PredefinedColorSpace::Srgb) == 0);
static_assert(to_underlying(Bindings::PredefinedColorSpace::DisplayP3Linear) == 3);
static_assert(sizeof(Bindings::CanvasColorType) == sizeof(u8));
static_assert(to_underlying(Bindings::CanvasColorType::Unorm8) == 0);
static_assert(to_underlying(Bindings::CanvasColorType::Float16) == 1);
static_assert(sizeof(Gfx::AffineTransform) == 6 * sizeof(float));
static_assert(sizeof(Gfx::IntRect) == 4 * sizeof(int));

extern "C" {

// Defined in Bindings/Rust/src/canvas_2d/exports.rs. A context is the cell of a context of either kind.
void* web_canvas_2d_context_create(JSVM*, ContextCreationArguments const*);
void* web_canvas_2d_context_canvas_host(void const* context);
bool web_canvas_2d_context_origin_clean(void const* context);
void web_canvas_2d_context_set_size(void* context, i32 width, i32 height);
void web_canvas_2d_context_reset_to_default_state(void* context);
void web_canvas_2d_context_ensure_backing_storage(void* context);
void web_canvas_2d_context_discard_backing_storage(void* context);
bool web_canvas_2d_context_has_backing_storage(void const* context);
void web_canvas_2d_context_drop_cached_readback(void* context);
bool web_canvas_2d_context_is_lost(void const* context);
void web_canvas_2d_context_set_lost(void* context, bool lost);
void web_canvas_2d_context_prepare_for_compositing(void* context);
bool web_canvas_2d_context_canvas_id(void const* context, u64* out_canvas_id);
u8 web_canvas_2d_context_color_space(void const* context);
void* web_canvas_2d_context_read_pixels(void* context, Gfx::IntRect rect, u8 alpha_type, u8 color_space);
bool web_canvas_2d_context_draw_image_bitmap(void* context, void* image_bitmap, float x, float y);

// What the Rust implementation asks of LibWeb, declared in Bindings/Rust/src/canvas_2d/canvas.rs.
void web_canvas_2d_canvas_did_change_content(void* canvas_host);
void web_canvas_2d_canvas_did_create_backing_storage(void* canvas_host);
void* web_canvas_2d_canvas_create_transport(void* canvas_host);
void* web_canvas_2d_canvas_wrapper(void* canvas, bool canvas_is_offscreen, void* realm);
void* web_canvas_2d_canvas_relevant_realm(void* canvas_host);
u64 web_canvas_2d_canvas_font_environment_generation(void* canvas_host);
bool web_canvas_2d_canvas_directionality_is_rtl(void* canvas, bool canvas_is_offscreen);
bool web_canvas_2d_canvas_resolve_font(void* canvas, bool canvas_is_offscreen, FlatPtr font, CanvasFontSelection, ResolvedFont* out_font);
void web_canvas_2d_canvas_resolve_color(void* canvas_host, CSS::StyleValue const* color, FontMetricsSource, u8 color_space, Gfx::Color* out_color, Gfx::Color* out_color_in_color_space);
float web_canvas_2d_canvas_resolve_length(void* canvas_host, CSS::StyleValue const* length, FontMetricsSource);
bool web_canvas_2d_canvas_parse_filter(void* canvas_host, FlatPtr filter, FontMetricsSource, void* functions, void (*append_function)(void*, Compositing::RustFFI::FfiFilterFunction const*));
CSS::StyleValue const* web_canvas_2d_parse_color(FlatPtr color);
CSS::StyleValue const* web_canvas_2d_parse_length(FlatPtr length, FlatPtr* out_serialized);
void web_canvas_2d_style_value_ref(CSS::StyleValue const*);
void web_canvas_2d_style_value_unref(CSS::StyleValue const*);
Gfx::DecodedImageFrame* web_canvas_2d_image_frame_from_pixels(u8 const* first_row, size_t pitch, i32 width, i32 height, u8 color_space);
u8 web_canvas_2d_check_usability_of_image(void* source_wrapper, CanvasImageSourceKind, FlatPtr* out_message);
void web_canvas_2d_image_source_size(void* source_wrapper, CanvasImageSourceKind, i32* out_width, i32* out_height);
bool web_canvas_2d_image_is_not_origin_clean(void* source_wrapper, CanvasImageSourceKind);
bool web_canvas_2d_image_source_canvas_id(void* source_wrapper, CanvasImageSourceKind, u64* out_canvas_id, bool* out_source_is_2d);
Gfx::DecodedImageFrame* web_canvas_2d_image_source_frame(void* source_wrapper, CanvasImageSourceKind);
bool web_canvas_2d_capture_video_frame(void* source_wrapper, CapturedVideoFrame* out_frame);
void* web_canvas_2d_video_transport(void* source_wrapper);
void web_canvas_2d_video_frame_hold_unref(Compositing::VideoFrameHold*);
void web_canvas_2d_set_video_frame_copy(Optional<Gfx::DecodedImageFrame>* out_frame, Gfx::DecodedImageFrame const* frame);

// What Bindings/Rust/src/html/canvas_gradient.rs asks of LibWeb.
bool web_canvas_gradient_parse_color(FlatPtr color, Gfx::Color* out_colors);
}

static void* cell_of(Canvas2DContextBase const& context)
{
    return const_cast<Canvas2DContextBase*>(&context);
}

Canvas2DContextBase& Canvas2DContextBase::create(CanvasHost& canvas_host, GC::Cell& canvas, bool canvas_is_offscreen, CanvasRenderingContext2DSettings const& context_attributes)
{
    auto size = canvas_host.bitmap_size_for_canvas();
    ContextCreationArguments arguments {
        .canvas = &canvas,
        .canvas_host = &canvas_host,
        .canvas_is_offscreen = canvas_is_offscreen,
        .width = size.width(),
        .height = size.height(),
        .alpha = context_attributes.alpha,
        .desynchronized = context_attributes.desynchronized,
        .color_space = context_attributes.color_space,
        .color_type = context_attributes.color_type,
        .will_read_frequently = context_attributes.will_read_frequently,
    };
    return *static_cast<Canvas2DContextBase*>(web_canvas_2d_context_create(JS::HostABI::vm_to_abi(JS::VM::the()), &arguments));
}

CanvasHost& Canvas2DContextBase::canvas_host() const
{
    return *static_cast<CanvasHost*>(web_canvas_2d_context_canvas_host(cell_of(*this)));
}

bool Canvas2DContextBase::origin_clean() const
{
    return web_canvas_2d_context_origin_clean(cell_of(*this));
}

void Canvas2DContextBase::set_size(Gfx::IntSize const& size)
{
    web_canvas_2d_context_set_size(this, size.width(), size.height());
}

void Canvas2DContextBase::reset_to_default_state()
{
    web_canvas_2d_context_reset_to_default_state(this);
}

void Canvas2DContextBase::prepare_for_compositing()
{
    web_canvas_2d_context_prepare_for_compositing(this);
}

void Canvas2DContextBase::ensure_backing_storage()
{
    web_canvas_2d_context_ensure_backing_storage(this);
}

void Canvas2DContextBase::discard_backing_storage()
{
    web_canvas_2d_context_discard_backing_storage(this);
}

Optional<Compositing::CanvasId> Canvas2DContextBase::canvas_id() const
{
    u64 canvas_id = 0;
    if (!web_canvas_2d_context_canvas_id(cell_of(*this), &canvas_id))
        return {};
    return Compositing::CanvasId { canvas_id };
}

Bindings::PredefinedColorSpace Canvas2DContextBase::color_space() const
{
    return predefined_color_space_from_underlying(web_canvas_2d_context_color_space(cell_of(*this)));
}

RefPtr<Gfx::Bitmap> Canvas2DContextBase::read_pixels(Gfx::IntRect const& rect, Gfx::AlphaType alpha_type, Bindings::PredefinedColorSpace color_space)
{
    auto* bitmap = static_cast<Gfx::Bitmap*>(web_canvas_2d_context_read_pixels(this, rect, to_underlying(alpha_type), to_underlying(color_space)));
    if (!bitmap)
        return nullptr;
    return adopt_ref(*bitmap);
}

bool Canvas2DContextBase::draw_image(ImageBitmap& image_bitmap, float x, float y)
{
    return web_canvas_2d_context_draw_image_bitmap(this, &image_bitmap, x, y);
}

// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-context-lost
void Canvas2DContextBase::notify_backing_storage_lost()
{
    if (!web_canvas_2d_context_has_backing_storage(this))
        return;
    web_canvas_2d_context_drop_cached_readback(this);

    // When the user agent detects that the backing storage associated with a canvas context has been lost, then it
    // must queue a global task on the DOM manipulation task source given canvas's relevant global object to run
    // these steps:
    queue_global_task(HTML::Task::Source::DOMManipulation, canvas_host().canvas_relevant_global_object(), GC::create_function(GC::Heap::the(), [context = GC::Ref { *this }] {
        // 1. Let canvas be context's canvas element.
        auto& canvas = context->canvas_host();

        // 2. If context's context lost is true, then abort these steps.
        if (web_canvas_2d_context_is_lost(context.ptr()))
            return;

        // 3. Set context's context lost to true.
        web_canvas_2d_context_set_lost(context.ptr(), true);

        // AD-HOC: Release the remote context backing the lost storage.
        context->discard_backing_storage();

        // 4. Reset the rendering context to its default state given context.
        context->reset_to_default_state();

        // 5. Let shouldRestore be the result of firing an event named contextlost at canvas, with the cancelable
        //    attribute initialized to true.
        DOM::EventInit context_lost_event_init;
        context_lost_event_init.cancelable = true;
        bool should_restore = canvas.canvas_event_target().dispatch_event(DOM::Event::create(canvas.canvas_relevant_global_object(), HTML::EventNames::contextlost, context_lost_event_init));

        // 6. If shouldRestore is false, then abort these steps.
        if (!should_restore)
            return;

        // 7. Attempt to restore context by creating a backing storage using context's attributes and associating
        //    them with context. If this fails, then abort these steps.
        context->ensure_backing_storage();
        if (!web_canvas_2d_context_has_backing_storage(context.ptr()))
            return;

        // 8. Set context's context lost to false.
        web_canvas_2d_context_set_lost(context.ptr(), false);

        // 9. Fire an event named contextrestored at canvas.
        canvas.canvas_event_target().dispatch_event(DOM::Event::create(canvas.canvas_relevant_global_object(), HTML::EventNames::contextrestored));
    }));
}

static CanvasHost& host_from(void* canvas_host)
{
    return *static_cast<CanvasHost*>(canvas_host);
}

void web_canvas_2d_canvas_did_change_content(void* canvas_host)
{
    host_from(canvas_host).did_change_canvas_content();
}

void web_canvas_2d_canvas_did_create_backing_storage(void* canvas_host)
{
    host_from(canvas_host).did_create_canvas_backing_storage();
}

static void* transport_for(Page& page)
{
    if (!page.has_compositor_host())
        return nullptr;
    auto transport = page.compositor_host().create_canvas_2d_transport();
    if (!transport)
        return nullptr;
    return &transport.release_nonnull().leak_ref();
}

void* web_canvas_2d_canvas_create_transport(void* canvas_host)
{
    return transport_for(host_from(canvas_host).canvas_page());
}

void* web_canvas_2d_canvas_wrapper(void* canvas, bool canvas_is_offscreen, void* realm_pointer)
{
    auto& realm = *static_cast<JS::Realm*>(realm_pointer);
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto* cell = static_cast<GC::Cell*>(canvas);
    if (canvas_is_offscreen)
        return Bindings::wrap(wrapper_world, realm, GC::Ref { *static_cast<OffscreenCanvas*>(cell) }).ptr();
    return Bindings::wrap(wrapper_world, realm, GC::Ref { *static_cast<HTMLCanvasElement*>(cell) }).ptr();
}

void* web_canvas_2d_canvas_relevant_realm(void* canvas_host)
{
    return &host_from(canvas_host).canvas_relevant_global_object().shape().realm();
}

u64 web_canvas_2d_canvas_font_environment_generation(void* canvas_host)
{
    return host_from(canvas_host).canvas_font_computer().environment_generation();
}

bool web_canvas_2d_canvas_directionality_is_rtl(void* canvas, bool canvas_is_offscreen)
{
    // FIXME: Return the inherited direction of an OffscreenCanvas, which OffscreenCanvas does not keep yet. It is
    //        'ltr' unless transferControlToOffscreen() or a transfer set it.
    if (canvas_is_offscreen)
        return false;
    auto& canvas_element = *static_cast<HTMLCanvasElement*>(static_cast<GC::Cell*>(canvas));
    return canvas_element.directionality() == DOM::Element::Directionality::Rtl;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-font
template<typename CanvasType>
static bool resolve_font(CanvasType& canvas_element, Utf16View font, CanvasFontSelection font_selection, ResolvedFont& out_font)
{
    // The font IDL attribute, on setting, must be parsed as a CSS <'font'> value (but without supporting property-independent style sheet syntax like 'inherit'),
    // and the resulting font must be assigned to the context, with the 'line-height' component forced to 'normal', with the 'font-size' component converted to CSS pixels,
    // and with system fonts being computed to explicit values.

    auto const parsing_context = [&]() {
        if constexpr (SameAs<CanvasType, HTML::HTMLCanvasElement>)
            return CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::OnScreenCanvasContextFontValue };
        return CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue };
    }();

    auto font_style_value_result = parse_css_value(parsing_context, font, CSS::PropertyID::Font);

    // If the new value is syntactically incorrect (including using property-independent style sheet syntax like 'inherit' or 'initial'), then it must be ignored, without assigning a new font value.
    // NOTE: ShorthandStyleValue should be the only valid option here. We implicitly VERIFY this below.
    if (!font_style_value_result || !font_style_value_result->is_shorthand())
        return false;

    // Load font with font style value properties
    auto const& font_style_value = font_style_value_result->as_shorthand();

    auto computed_math_depth = CSS::InitialValues::math_depth();

    // FIXME: We will need to absolutize this once we support ident() functions
    auto font_family = font_style_value.longhand(CSS::PropertyID::FontFamily);

    Optional<DOM::AbstractElement> inheritance_parent;

    if constexpr (SameAs<CanvasType, HTML::HTMLCanvasElement>) {
        // NB: Once pending style work is settled, the canvas's installed style is current, unless it is below
        //     display:none, where style updates leave it stale. Only then is its style computed again.
        auto& document = canvas_element.document();
        Layout::ForcedReadScope read { document };
        document.update_style_for_element(DOM::AbstractElement { canvas_element }, DOM::Document::StyleUpdateMode::OnlyIfNeeded);
        auto style_record = canvas_element.style_record_identity();
        if (!style_record || has_flag(document.style_computer().style_engine().style_record_dependency_flags(read, style_record), CSS::StyleRecordDependencyFlag::InDisplayNoneSubtree))
            document.update_style_for_element(DOM::AbstractElement { canvas_element });

        if (canvas_element.navigable() && canvas_element.is_connected()) {
            // NOTE: Since we can't set a math depth directly here we always use the inherited value for the computed value
            computed_math_depth = canvas_element.template style_group<CSS::ComputedValues::FontValues>()->math_depth;

            // NOTE: The canvas itself is considered the inheritance parent
            inheritance_parent = canvas_element;
        }
    }

    auto computation_context = canvas_element.canvas_font_computation_context();

    auto const& computed_font_size = CSS::StyleComputer::compute_font_size(font_style_value.longhand(CSS::PropertyID::FontSize)->absolutized(computation_context), computed_math_depth, inheritance_parent, CSSPixels { 10 });
    auto const& computed_font_style = CSS::StyleComputer::compute_font_style(font_style_value.longhand(CSS::PropertyID::FontStyle)->absolutized(computation_context));
    auto const& computed_font_weight = CSS::StyleComputer::compute_font_weight(font_style_value.longhand(CSS::PropertyID::FontWeight)->absolutized(computation_context), inheritance_parent);
    auto const& computed_font_width = CSS::StyleComputer::compute_font_width(font_style_value.longhand(CSS::PropertyID::FontWidth)->absolutized(computation_context));
    // NB: This doesn't require absolutization since only the font-variant-caps longhand can be set and that can only be
    //     a keyword value
    auto const& computed_font_variant = font_style_value.longhand(CSS::PropertyID::FontVariant).release_nonnull();

    auto resolved_font_style_value = CSS::ShorthandStyleValue::create(
        CSS::PropertyID::Font,
        {
            // Set explicitly https://drafts.csswg.org/css-fonts/#font-prop
            CSS::PropertyID::FontFamily,
            CSS::PropertyID::FontSize,
            CSS::PropertyID::FontWidth,
            CSS::PropertyID::FontStyle,
            CSS::PropertyID::FontVariant,
            CSS::PropertyID::FontWeight,
            CSS::PropertyID::LineHeight,

            // Reset implicitly https://drafts.csswg.org/css-fonts/#font-prop
            CSS::PropertyID::FontFeatureSettings,
            CSS::PropertyID::FontKerning,
            CSS::PropertyID::FontLanguageOverride,
            CSS::PropertyID::FontOpticalSizing,
            CSS::PropertyID::FontVariationSettings,
            CSS::PropertyID::FontSizeAdjust,
        },
        {
            // Set explicitly
            *font_family,
            computed_font_size,
            computed_font_width,
            computed_font_style,
            computed_font_variant,
            computed_font_weight,
            property_initial_value(CSS::PropertyID::LineHeight), // NB: line-height is forced to normal (i.e. the initial value)

            // Reset implicitly
            property_initial_value(CSS::PropertyID::FontFeatureSettings),   // font-feature-settings
            property_initial_value(CSS::PropertyID::FontKerning),           // font-kerning,
            property_initial_value(CSS::PropertyID::FontLanguageOverride),  // font-language-override
            property_initial_value(CSS::PropertyID::FontOpticalSizing),     // font-optical-sizing,
            property_initial_value(CSS::PropertyID::FontVariationSettings), // font-variation-settings
            property_initial_value(CSS::PropertyID::FontSizeAdjust),        // font-size-adjust
        });

    CSS::FontResolutionFeatureValues feature_values;
    if (auto caps = computed_font_variant->as_shorthand().longhand(CSS::PropertyID::FontVariantCaps); caps->to_keyword() == CSS::Keyword::SmallCaps)
        feature_values[to_underlying(CSS::FontResolutionFeatureInput::FontVariantCaps)] = caps;

    // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
    // NB: The text preparation algorithm sets 'font-kerning', 'font-stretch', 'font-variant-caps' and SVG
    //     'text-rendering' from the fontKerning, fontStretch, fontVariantCaps and textRendering attributes of the
    //     context. They select the fonts of the text, so they are applied when the font is resolved.
    // AD-HOC: fontStretch and fontVariantCaps only apply when they are not "normal", so that the font-stretch and
    //         font-variant-caps components of the font still apply when they are.
    auto computed_font_width_percentage = computed_font_width->as_percentage().percentage();
    static constexpr Array<CSS::Keyword, 3> font_kerning_keywords { CSS::Keyword::Auto, CSS::Keyword::Normal, CSS::Keyword::None };
    if (font_selection.font_kerning != 0 && font_selection.font_kerning < font_kerning_keywords.size())
        feature_values[to_underlying(CSS::FontResolutionFeatureInput::FontKerning)] = CSS::KeywordStyleValue::create(font_kerning_keywords[font_selection.font_kerning]);
    static constexpr Array<double, 9> font_stretch_percentages { 50, 62.5, 75, 87.5, 100, 112.5, 125, 150, 200 };
    if (font_selection.font_stretch != 4 && font_selection.font_stretch < font_stretch_percentages.size())
        computed_font_width_percentage = CSS::Percentage { font_stretch_percentages[font_selection.font_stretch] };
    static constexpr Array<CSS::Keyword, 7> font_variant_caps_keywords { CSS::Keyword::Normal, CSS::Keyword::SmallCaps, CSS::Keyword::AllSmallCaps, CSS::Keyword::PetiteCaps, CSS::Keyword::AllPetiteCaps, CSS::Keyword::Unicase, CSS::Keyword::TitlingCaps };
    if (font_selection.font_variant_caps != 0 && font_selection.font_variant_caps < font_variant_caps_keywords.size())
        feature_values[to_underlying(CSS::FontResolutionFeatureInput::FontVariantCaps)] = CSS::KeywordStyleValue::create(font_variant_caps_keywords[font_selection.font_variant_caps]);
    static constexpr Array<CSS::Keyword, 4> text_rendering_keywords { CSS::Keyword::Auto, CSS::Keyword::Optimizespeed, CSS::Keyword::Optimizelegibility, CSS::Keyword::Geometricprecision };
    if (font_selection.text_rendering != 0 && font_selection.text_rendering < text_rendering_keywords.size())
        feature_values[to_underlying(CSS::FontResolutionFeatureInput::TextRendering)] = CSS::KeywordStyleValue::create(text_rendering_keywords[font_selection.text_rendering]);

    // https://drafts.csswg.org/css-font-loading/#font-source
    auto& font_computer = canvas_element.canvas_font_computer();

    auto font_size = computed_font_size->as_length().length().absolute_length_to_px();
    auto cascade = CSS::resolve_font_for_style_values(font_computer,
        {
            .font_families = CSS::computed_font_families_from_style_value(*font_family),
            .font_optical_sizing = CSS::FontOpticalSizing::Auto,
            .font_size = font_size,
            .font_slope = computed_font_style->as_font_style().to_font_slope(),
            .font_weight = computed_font_weight->as_number().number(),
            .font_width = computed_font_width_percentage,
            .feature_values = move(feature_values),
            .font_feature_values_scope = {},
        });

    out_font = {
        .cascade = &cascade.leak_ref(),
        .environment_generation = font_computer.environment_generation(),
        .font_size = font_size.raw_value(),
        // On getting, the font attribute must return the serialized form of the current font of the context (with no 'line-height' component).
        .serialized = resolved_font_style_value->to_utf16_string(CSS::SerializationMode::ResolvedValue).into_raw(),
    };
    return true;
}

bool web_canvas_2d_canvas_resolve_font(void* canvas, bool canvas_is_offscreen, FlatPtr font, CanvasFontSelection font_selection, ResolvedFont* out_font)
{
    auto font_string = Utf16String::from_raw(font);
    auto* cell = static_cast<GC::Cell*>(canvas);
    if (canvas_is_offscreen)
        return resolve_font(*static_cast<OffscreenCanvas*>(cell), font_string.utf16_view(), font_selection, *out_font);
    return resolve_font(*static_cast<HTMLCanvasElement*>(cell), font_string.utf16_view(), font_selection, *out_font);
}

// What the CSS values of a context are resolved with: the viewport of the canvas, and the font of the context.
static CSS::ComputationContext computation_context_for(CanvasHost& canvas_host, FontMetricsSource font)
{
    auto font_metrics = [&]() {
        // https://html.spec.whatwg.org/multipage/canvas.html#text-styles
        // NB: We lazily initialize the font, so if it hasn't been set yet we generate the font metrics based on the default font
        if (!font.cascade) {
            // When the object implementing the CanvasTextDrawingStyles interface is created, the font of the context must be set to 10px sans-serif.
            return CSS::Length::FontMetrics { 10, Platform::FontPlugin::the().default_font(8)->pixel_metrics(), CSS::InitialValues::line_height() };
        }

        auto const& first_font = font.cascade->first_available_font();
        return CSS::Length::FontMetrics { CSSPixels::from_raw(font.font_size), first_font.pixel_metrics(), CSS::InitialValues::line_height() };
    }();

    return {
        .length_resolution_context = {
            .viewport_rect = canvas_host.canvas_viewport_rect(),
            .font_metrics = font_metrics,
            .root_font_metrics = font_metrics },

        // NB: We don't require an abstract element because tree counting and random() functions aren't allowed in
        //     non-font canvas context values
        .abstract_element = {},

        // FIXME: The spec doesn't specify what color scheme should be used here but other browsers always use light so
        //        we do too for compatibility. See https://github.com/whatwg/html/issues/12505
        .color_scheme = CSS::PreferredColorScheme::Light
    };
}

// NB: A color is converted to the color space of a context, and clipped to its gamut, as the 'relative-colorimetric'
//     rendering intent does, which the conversions of the pixels of a context also use.
static Gfx::Color used_color_in_predefined_color_space(CSS::StyleValue const& color, CSS::ColorResolutionStyle const& style, Bindings::PredefinedColorSpace color_space, Gfx::Color srgb_color)
{
    if (color_space == Bindings::PredefinedColorSpace::Srgb)
        return srgb_color;

    auto ffi_style = style.to_ffi();
    auto resolved = CSS::StyleValueFFI::rust_style_value_to_color_components(color.rust_style_value_data(), &ffi_style, to_underlying(css_color_type_for(color_space)));
    if (!resolved.resolved)
        return srgb_color;
    auto quantize = [](float component) -> u8 {
        if (isnan(component))
            return 0;
        return static_cast<u8>(clamp(roundf(component * 255.0f), 0.0f, 255.0f));
    };
    return Gfx::Color(quantize(resolved.components[0]), quantize(resolved.components[1]), quantize(resolved.components[2]), quantize(resolved.components[3]));
}

void web_canvas_2d_canvas_resolve_color(void* canvas_host, CSS::StyleValue const* color, FontMetricsSource font, u8 color_space, Gfx::Color* out_color, Gfx::Color* out_color_in_color_space)
{
    auto& host = host_from(canvas_host);
    auto computation_context = computation_context_for(host, font);
    auto absolutized_color = color->absolutized(computation_context);
    auto style = host.canvas_color_resolution_style();
    *out_color = absolutized_color->to_color(style).value();
    *out_color_in_color_space = used_color_in_predefined_color_space(*absolutized_color, style, predefined_color_space_from_underlying(color_space), *out_color);
}

float web_canvas_2d_canvas_resolve_length(void* canvas_host, CSS::StyleValue const* length, FontMetricsSource font)
{
    auto absolutized_length = CSS::Length::from_style_value(length->absolutized(computation_context_for(host_from(canvas_host), font)), {});
    return static_cast<float>(absolutized_length.absolute_length_to_px().to_double());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filter
bool web_canvas_2d_canvas_parse_filter(void* canvas_host, FlatPtr filter, FontMetricsSource font, void* functions, void (*append_function)(void*, Compositing::RustFFI::FfiFilterFunction const*))
{
    auto& host = host_from(canvas_host);
    auto filter_string = Utf16String::from_raw(filter);
    CSS::Parser::Parser parser { CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue } };

    // 2. Let parsedValue be the result of parsing the given values as a <filter-value-list>.
    //    If any property-independent style sheet syntax like 'inherit' or 'initial' is present,
    //    then this parsing must return failure.
    auto style_value = parser.parse_as_css_value(filter_string.utf16_view(), CSS::PropertyID::Filter);
    if (!style_value || !style_value->is_value_list())
        return false;

    auto absolutized_style_value = style_value->absolutized(computation_context_for(host, font));
    for (auto& item : absolutized_style_value->as_value_list().values()) {
        if (item->is_url()) {
            // FIXME: Resolve the SVG filter
            dbgln("FIXME: SVG filters are not implemented for Canvas2D");
            continue;
        }

        auto const& filter_value = item->as_filter();
        Compositing::RustFFI::FfiFilterFunction function {};
        switch (filter_value.kind()) {
        case CSS::FilterStyleValue::Kind::Blur: {
            auto const& blur_filter = static_cast<CSS::BlurFilterStyleValue const&>(filter_value);
            function.kind = Compositing::RustFFI::FfiFilterFunctionKind::Blur;
            function.amount = blur_filter.resolved_radius();
            break;
        }
        case CSS::FilterStyleValue::Kind::Color: {
            auto const& color = static_cast<CSS::ColorFilterStyleValue const&>(filter_value);
            function.kind = Compositing::RustFFI::FfiFilterFunctionKind::Color;
            function.color_operation = color.operation();
            function.amount = color.resolved_amount();
            break;
        }
        case CSS::FilterStyleValue::Kind::HueRotate: {
            auto const& hue_rotate = static_cast<CSS::HueRotateFilterStyleValue const&>(filter_value);
            function.kind = Compositing::RustFFI::FfiFilterFunctionKind::HueRotate;
            function.amount = hue_rotate.angle_degrees();
            break;
        }
        case CSS::FilterStyleValue::Kind::DropShadow: {
            auto const& drop_shadow = static_cast<CSS::DropShadowFilterStyleValue const&>(filter_value);
            function.kind = Compositing::RustFFI::FfiFilterFunctionKind::DropShadow;
            function.offset_x = static_cast<float>(CSS::Length::from_style_value(drop_shadow.offset_x(), {}).absolute_length_to_px());
            function.offset_y = static_cast<float>(CSS::Length::from_style_value(drop_shadow.offset_y(), {}).absolute_length_to_px());
            if (drop_shadow.radius())
                function.amount = static_cast<float>(CSS::Length::from_style_value(*drop_shadow.radius(), {}).absolute_length_to_px());
            function.color = drop_shadow.color() ? drop_shadow.color()->to_color(host.canvas_color_resolution_style()).value_or(Gfx::Color::Black) : Gfx::Color::Black;
            break;
        }
        }
        append_function(functions, &function);
    }
    return true;
}

CSS::StyleValue const* web_canvas_2d_parse_color(FlatPtr color)
{
    auto color_string = Utf16String::from_raw(color);
    auto parsed_color = parse_css_type(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, color_string.utf16_view(), CSS::ValueType::Color);
    if (!parsed_color)
        return nullptr;
    return &parsed_color.release_nonnull().leak_ref();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-letterspacing
CSS::StyleValue const* web_canvas_2d_parse_length(FlatPtr length, FlatPtr* out_serialized)
{
    auto length_string = Utf16String::from_raw(length);
    auto parsed = parse_css_type(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, length_string.utf16_view(), CSS::ValueType::Length);
    if (!parsed)
        return nullptr;
    Utf16StringBuilder builder;
    parsed->serialize(builder, CSS::SerializationMode::Normal);
    *out_serialized = builder.to_string().into_raw();
    return &parsed.release_nonnull().leak_ref();
}

void web_canvas_2d_style_value_ref(CSS::StyleValue const* style_value)
{
    style_value->ref();
}

void web_canvas_2d_style_value_unref(CSS::StyleValue const* style_value)
{
    style_value->unref();
}

Gfx::DecodedImageFrame* web_canvas_2d_image_frame_from_pixels(u8 const* first_row, size_t pitch, i32 width, i32 height, u8 color_space)
{
    // NB: The bitmap is shareable, so that flushing to the Compositor passes the pixels by file descriptor instead of
    //     copying them again.
    auto bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, { width, height });
    if (bitmap.is_error())
        return nullptr;
    auto row_size = static_cast<size_t>(width) * sizeof(Gfx::RawPixel);
    for (int y = 0; y < height; ++y)
        __builtin_memcpy(bitmap.value()->scanline(y), first_row + static_cast<size_t>(y) * pitch, row_size);
    return new Gfx::DecodedImageFrame { *bitmap.release_value(), Gfx::AlphaType::Unpremultiplied, gfx_color_space_for(predefined_color_space_from_underlying(color_space)) };
}

static CanvasImageSource image_source_from(void* source_wrapper, CanvasImageSourceKind kind)
{
    auto& wrapper = *static_cast<JS::Object*>(source_wrapper);
    switch (kind) {
    case CanvasImageSourceKind::HTMLImageElement:
        return GC::Ref { Bindings::impl_from<HTMLImageElement>(wrapper) };
    case CanvasImageSourceKind::SVGImageElement:
        return GC::Ref { Bindings::impl_from<SVG::SVGImageElement>(wrapper) };
    case CanvasImageSourceKind::HTMLVideoElement:
        return GC::Ref { Bindings::impl_from<HTMLVideoElement>(wrapper) };
    case CanvasImageSourceKind::HTMLCanvasElement:
        return GC::Ref { Bindings::impl_from<HTMLCanvasElement>(wrapper) };
    case CanvasImageSourceKind::OffscreenCanvas:
        return GC::Ref { Bindings::impl_from<OffscreenCanvas>(wrapper) };
    }
    VERIFY_NOT_REACHED();
}

// The values of `Usability` of Bindings/Rust/src/canvas_2d/canvas.rs.
static constexpr u8 usability_good = 0;
static constexpr u8 usability_bad = 1;
static constexpr u8 usability_invalid_state = 2;

u8 web_canvas_2d_check_usability_of_image(void* source_wrapper, CanvasImageSourceKind kind, FlatPtr* out_message)
{
    auto usability = check_usability_of_image(image_source_from(source_wrapper, kind));
    if (usability.is_exception()) {
        auto exception = usability.release_error();
        auto& dom_exception = exception.get<GC::Ref<WebIDL::DOMException>>();
        VERIFY(dom_exception->name() == "InvalidStateError"_utf16_fly_string);
        *out_message = Utf16String { dom_exception->message() }.into_raw();
        return usability_invalid_state;
    }
    return usability.value() == CanvasImageSourceUsability::Good ? usability_good : usability_bad;
}

void web_canvas_2d_image_source_size(void* source_wrapper, CanvasImageSourceKind kind, i32* out_width, i32* out_height)
{
    auto image_provider_intrinsic_size = [](Layout::ImageProvider const& image_provider) -> Optional<Gfx::IntSize> {
        auto intrinsic_width = image_provider.intrinsic_width();
        auto intrinsic_height = image_provider.intrinsic_height();
        if (!intrinsic_width.has_value() || !intrinsic_height.has_value())
            return {};
        return Gfx::IntSize { intrinsic_width->to_int(), intrinsic_height->to_int() };
    };

    auto size = image_source_from(source_wrapper, kind).visit([&](GC::Ref<HTMLImageElement> source) -> Gfx::IntSize {
            if (auto intrinsic_size = image_provider_intrinsic_size(*source); intrinsic_size.has_value())
                return *intrinsic_size;

            // FIXME: This is very janky and not correct.
            return { source->width(), source->height() }; }, [&](GC::Ref<SVG::SVGImageElement> source) -> Gfx::IntSize {
            if (auto intrinsic_size = image_provider_intrinsic_size(*source); intrinsic_size.has_value())
                return *intrinsic_size;

            // FIXME: This is very janky and not correct.
            auto maybe_width = source->width()->anim_val()->value();
            auto maybe_height = source->height()->anim_val()->value();

            auto width = maybe_width.is_exception() ? 0 : maybe_width.release_value();
            auto height = maybe_height.is_exception() ? 0 : maybe_height.release_value();

            return { width, height }; }, [](GC::Ref<HTMLCanvasElement> source) -> Gfx::IntSize { return { source->width(), source->height() }; }, [](GC::Ref<OffscreenCanvas> source) -> Gfx::IntSize { return source->bitmap_size_for_canvas(); }, [](GC::Ref<HTMLVideoElement> source) -> Gfx::IntSize { return { source->video_width(), source->video_height() }; }, [](GC::Ref<ImageBitmap>) -> Gfx::IntSize { VERIFY_NOT_REACHED(); });
    *out_width = size.width();
    *out_height = size.height();
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
bool web_canvas_2d_image_is_not_origin_clean(void* source_wrapper, CanvasImageSourceKind kind)
{
    return image_is_not_origin_clean(image_source_from(source_wrapper, kind));
}

// The id of the canvas that a canvas or an OffscreenCanvas draws into, readied to be drawn, if it is drawn by its id.
bool web_canvas_2d_image_source_canvas_id(void* source_wrapper, CanvasImageSourceKind kind, u64* out_canvas_id, bool* out_source_is_2d)
{
    auto& wrapper = *static_cast<JS::Object*>(source_wrapper);
    Optional<Compositing::CanvasId> source_canvas_id;
    if (kind == CanvasImageSourceKind::HTMLCanvasElement) {
        auto& source_canvas = Bindings::impl_from<HTMLCanvasElement>(wrapper);
        if (source_canvas.is_placeholder())
            return false;
        bool const source_is_2d = source_canvas.canvas_rendering_context_2d() != nullptr;
        source_canvas.ensure_backing_storage();
        if (!source_is_2d)
            source_canvas.prepare_for_compositing();
        source_canvas_id = source_canvas.canvas_id();
        *out_source_is_2d = source_is_2d;
    } else if (kind == CanvasImageSourceKind::OffscreenCanvas) {
        auto& source_canvas = Bindings::impl_from<OffscreenCanvas>(wrapper);
        auto const* source_context = source_canvas.context().get_pointer<GC::Ref<OffscreenCanvasRenderingContext2D>>();
        if (!source_context)
            return false;
        (*source_context)->ensure_backing_storage();
        source_canvas_id = (*source_context)->canvas_id();
        *out_source_is_2d = true;
    }
    if (!source_canvas_id.has_value())
        return false;
    *out_canvas_id = source_canvas_id->value();
    return true;
}

Gfx::DecodedImageFrame* web_canvas_2d_image_source_frame(void* source_wrapper, CanvasImageSourceKind kind)
{
    auto frame = image_source_from(source_wrapper, kind).visit([](OneOf<GC::Ref<HTMLImageElement>, GC::Ref<SVG::SVGImageElement>> auto const& element) -> Optional<Gfx::DecodedImageFrame> {
            auto image_data = element->decoded_image_data();
            if (!image_data)
                return {};

            Gfx::IntSize size;
            auto intrinsic_width = element->intrinsic_width();
            auto intrinsic_height = element->intrinsic_height();
            if (intrinsic_width.has_value() && intrinsic_height.has_value())
                size = { intrinsic_width->to_int(), intrinsic_height->to_int() };

            return image_data->default_frame(size); }, [](GC::Ref<HTMLCanvasElement> const& canvas) -> Optional<Gfx::DecodedImageFrame> {
            canvas->prepare_for_compositing();
            return canvas->get_frame_from_surface(); }, [](GC::Ref<OffscreenCanvas> const& source) -> Optional<Gfx::DecodedImageFrame> { return source->get_frame_from_surface(); }, [](OneOf<GC::Ref<HTMLVideoElement>, GC::Ref<ImageBitmap>> auto const&) -> Optional<Gfx::DecodedImageFrame> { VERIFY_NOT_REACHED(); });
    if (!frame.has_value())
        return nullptr;
    return new Gfx::DecodedImageFrame { frame.release_value() };
}

bool web_canvas_2d_capture_video_frame(void* source_wrapper, CapturedVideoFrame* out_frame)
{
    auto& video = Bindings::impl_from<HTMLVideoElement>(*static_cast<JS::Object*>(source_wrapper));
    auto captured_frame = video.capture_current_video_frame();
    if (!captured_frame.has_value())
        return false;
    VERIFY(captured_frame->hold);
    *out_frame = {
        .hold = &captured_frame->hold.release_nonnull().leak_ref(),
        .sink_handle = captured_frame->sink_handle.value(),
        .generation = captured_frame->generation,
    };
    return true;
}

void* web_canvas_2d_video_transport(void* source_wrapper)
{
    auto& video = Bindings::impl_from<HTMLVideoElement>(*static_cast<JS::Object*>(source_wrapper));
    return transport_for(video.document().page());
}

void web_canvas_2d_video_frame_hold_unref(Compositing::VideoFrameHold* hold)
{
    hold->unref();
}

void web_canvas_2d_set_video_frame_copy(Optional<Gfx::DecodedImageFrame>* out_frame, Gfx::DecodedImageFrame const* frame)
{
    *out_frame = *frame;
}

// https://drafts.csswg.org/css-color/#parse-a-css-color-value
// NB: The color is written in each predefined color space, in the order of Bindings::PredefinedColorSpace.
bool web_canvas_gradient_parse_color(FlatPtr color, Gfx::Color* out_colors)
{
    auto color_string = Utf16String::from_raw(color);
    auto const maybe_color = parse_css_value(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, color_string.utf16_view(), CSS::PropertyID::Color);
    if (maybe_color.is_null() || !maybe_color->has_color())
        return false;
    CSS::ColorResolutionStyle style;
    auto srgb_color = maybe_color->to_color(style).value();
    for (u8 color_space = 0; color_space <= to_underlying(Bindings::PredefinedColorSpace::DisplayP3Linear); ++color_space)
        out_colors[color_space] = used_color_in_predefined_color_space(*maybe_color, style, predefined_color_space_from_underlying(color_space), srgb_color);
    return true;
}

}
