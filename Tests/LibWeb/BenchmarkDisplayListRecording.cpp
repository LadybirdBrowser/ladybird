/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/Utilities.h>

// Compare cold, quiet and sparse recordings of flat and nested pages. Mutations and layout are
// timed separately, and outline changes exercise fragments whose command counts grow or shrink.

namespace {

constexpr size_t cards_per_row = 40;
constexpr int card_width = 20;
constexpr int card_height = 12;
constexpr size_t timed_iterations = 200;
constexpr Web::DevicePixelSize viewport_size { Web::DevicePixels(800), Web::DevicePixels(600) };
constexpr Web::CSSPixelSize css_viewport_size { Web::CSSPixels(800), Web::CSSPixels(600) };

enum class DocumentShape {
    FlatCards,
    FlatCardsFew,
    NestedPanels,
    SharedContextCards,
    TokenizedCards,
};

size_t card_count_of(DocumentShape shape)
{
    // A long diff has many paint occurrences sharing one stacking context. Keep
    // enough owners here to expose the cost of reconstructing the packed program.
    if (shape == DocumentShape::SharedContextCards)
        return 20000;
    return shape == DocumentShape::FlatCardsFew ? 200 : 2000;
}

// What one timed iteration changes before recording. Each variant dirties a different set of stacking
// producers; the outline mutations also change the amount of recorded content.
enum class Mutation {
    // One card's background: the dirty capture keeps its size, so nothing after it moves.
    OneCardBackground,
    // One card gains or loses an outline without changing layout geometry.
    OneCardOutlineToggle,
    // Two cards in different halves of the page: two dirty contexts under one ancestor.
    TwoCardsBackground,
    // The first card, with a negative z-index, gains or loses commands ahead of its siblings.
    NegativeZIndexCardOutlineToggle,
    // Every twentieth card: a hundred dirty contexts under one ancestor among 2000.
    OneInTwentyCardsBackground,
    // Every fifth card: four hundred dirty contexts under one ancestor among 2000.
    OneInFiveCardsBackground,
};

// Whether the paint caches are dropped before each timed recording, so the recording starts from scratch and
// records every capture along with the bookkeeping that later warm recordings rely on.
enum class CacheState {
    Warm,
    Cold,
};

class BenchmarkPageClient final : public Web::PageClient {
    GC_CELL(BenchmarkPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(BenchmarkPageClient);

public:
    virtual ~BenchmarkPageClient() override = default;

    void set_page(GC::Ref<Web::Page> page) { m_page = page; }

    virtual Web::PageId id() const override { return Web::PageId { 1 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { return Gfx::Palette(*m_palette_impl); }
    virtual Web::DevicePixelRect screen_rect() const override { return { { Web::DevicePixels(0), Web::DevicePixels(0) }, viewport_size }; }
    virtual double zoom_level() const override { return 1.0; }
    virtual double device_pixel_ratio() const override { return 1.0; }
    virtual double device_pixels_per_css_pixel() const override { return 1.0; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Light; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::NoPreference; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { return m_input_event_queue; }
    virtual void report_finished_handling_input_event(Web::PageId, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 1, m_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }

private:
    BenchmarkPageClient()
    {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)));
        auto* theme = buffer.data<Gfx::SystemTheme>();
        theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::White).value();
        theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::Black).value();
        theme->color[to_underlying(Gfx::ColorRole::Base)] = Color(Color::White).value();
        theme->color[to_underlying(Gfx::ColorRole::BaseText)] = Color(Color::Black).value();
        m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(move(buffer));
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Queue<Web::QueuedInputEvent> m_input_event_queue;
    u64 m_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(BenchmarkPageClient);

struct LoadedPage {
    GC::Root<BenchmarkPageClient> client;
    GC::Root<Web::Page> page;
    Web::Painting::DisplayListResourceStorage display_list_resource_storage {};
    DocumentShape shape { DocumentShape::FlatCards };
    size_t style_change_count { 0 };

    Web::DOM::Document& document()
    {
        auto document = page->local_root_navigable()->active_document();
        VERIFY(document);
        return *document;
    }
};

OwnPtr<Core::EventLoop> s_event_loop;
OwnPtr<LoadedPage> s_flat_cards_page;
OwnPtr<LoadedPage> s_flat_cards_few_page;
OwnPtr<LoadedPage> s_nested_panels_page;
OwnPtr<LoadedPage> s_shared_context_cards_page;
OwnPtr<LoadedPage> s_tokenized_cards_page;

// The markup goes into the initial about:blank document's root element: navigations are driven by the browser
// process's session history, which an in-process page has no access to.
Utf16String build_document_markup(DocumentShape shape)
{
    StringBuilder builder { 400'000 };
    builder.append("<head><style>"sv);
    builder.append("html,body{margin:0;padding:0}body{background:white;font:8px/12px monospace}"sv);
    builder.append(".card{position:absolute;z-index:1;box-sizing:border-box;border:1px solid #446;background:#dde;color:#113;overflow:hidden}"sv);
    if (shape == DocumentShape::SharedContextCards)
        builder.append(".card{z-index:auto}"sv);
    builder.append(".token:nth-child(2n){color:#c40}.token:nth-child(2n+1){color:#048}"sv);
    builder.append(".panel{position:relative;z-index:0;width:800px;height:600px}"sv);
    builder.append("#negative{position:absolute;z-index:-1;left:0;top:0;width:800px;height:600px;background:#eef}"sv);
    builder.append("</style></head><body>"sv);
    size_t open_panels = 0;
    if (shape == DocumentShape::NestedPanels) {
        for (; open_panels < 3; ++open_panels)
            builder.append("<div class=panel>"sv);
    }
    builder.append("<div id=negative></div>"sv);
    for (size_t i = 0; i < card_count_of(shape); ++i) {
        auto columns = shape == DocumentShape::TokenizedCards ? 4 : cards_per_row;
        auto width = shape == DocumentShape::TokenizedCards ? 200 : card_width;
        auto left = static_cast<int>(i % columns) * width;
        auto top = static_cast<int>(i / columns) * card_height;
        builder.appendff("<div class=card id=card-{} style=\"left:{}px;top:{}px;width:{}px;height:{}px\">", i, left, top, width, card_height);
        if (shape == DocumentShape::TokenizedCards) {
            for (auto token : { "auto"sv, " value"sv, " ="sv, " foo"sv, "("sv, "42"sv, ")"sv, ";"sv })
                builder.appendff("<span class=token>{}</span>", token);
        } else {
            builder.appendff("{}", i % 100);
        }
        builder.append("</div>"sv);
    }
    for (; open_panels > 0; --open_panels)
        builder.append("</div>"sv);
    builder.append("</body>"sv);
    return Utf16String::from_utf8_without_validation(builder.string_view());
}

void initialize_web_platform()
{
    s_event_loop = make<Core::EventLoop>();

    WebView::platform_init();
    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);

    // System fonts, like the other in-process page tests: the test binaries have no resource directory to load
    // the bundled fonts from.
    Web::Platform::FontPlugin::install(*new Web::Platform::FontPlugin(false));

    Web::Bindings::initialize_main_thread_vm(Web::HTML::AgentType::SimilarOriginWindow);
}

Web::HTML::PaintConfig benchmark_paint_config()
{
    return Web::HTML::PaintConfig {
        .paint_overlay = false,
        .canvas_fill_rect = Gfx::IntRect { 0, 0, viewport_size.width().value(), viewport_size.height().value() },
    };
}

RefPtr<Web::Painting::DisplayList> record_display_list(LoadedPage& loaded_page)
{
    auto& document = loaded_page.document();
    document.update_layout(Web::DOM::UpdateLayoutReason::Debugging);
    return document.record_display_list(benchmark_paint_config(), loaded_page.display_list_resource_storage, Web::Painting::PaintCommandCacheMode::ReadWrite);
}

OwnPtr<LoadedPage> load_page(DocumentShape shape)
{
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<BenchmarkPageClient>();
    auto page = Web::Page::create(client);
    client->set_page(page);

    page->set_is_scripting_enabled(false);
    page->set_window_size(viewport_size);
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_local_root_navigable(traversable);
    traversable->set_viewport_size(css_viewport_size, Web::InvalidateDisplayList::PaintCommandsAndHitTestList);

    auto document = traversable->active_document();
    VERIFY(document);
    document->set_quirks_mode(Web::DOM::QuirksMode::No);
    auto markup = build_document_markup(shape);
    MUST(document->document_element()->set_inner_html(markup));

    auto loaded_page = make<LoadedPage>(GC::make_root(client), GC::make_root(page));
    loaded_page->shape = shape;
    // Populate the per-paintable paint command caches before timing the warm path.
    VERIFY(record_display_list(*loaded_page));
    VERIFY(record_display_list(*loaded_page));
    return loaded_page;
}

LoadedPage& page_for(DocumentShape shape)
{
    auto& slot = [&]() -> OwnPtr<LoadedPage>& {
        switch (shape) {
        case DocumentShape::FlatCards:
            return s_flat_cards_page;
        case DocumentShape::FlatCardsFew:
            return s_flat_cards_few_page;
        case DocumentShape::NestedPanels:
            return s_nested_panels_page;
        case DocumentShape::SharedContextCards:
            return s_shared_context_cards_page;
        case DocumentShape::TokenizedCards:
            return s_tokenized_cards_page;
        }
        VERIFY_NOT_REACHED();
    }();
    if (!slot)
        slot = load_page(shape);
    return *slot;
}

void set_card_style(LoadedPage& loaded_page, size_t card_index, StringView background, bool framed)
{
    auto card = loaded_page.document().get_element_by_id(Utf16String::formatted("card-{}", card_index));
    VERIFY(card);
    auto left = static_cast<int>(card_index % cards_per_row) * card_width;
    auto top = static_cast<int>(card_index / cards_per_row) * card_height;
    card->set_attribute_value(Web::HTML::AttributeNames::style, Utf16String::formatted("left:{}px;top:{}px;width:{}px;height:{}px;background:{}{}", left, top, card_width, card_height, background, framed ? ";outline:2px solid #800"sv : ""sv));
}

void apply_mutation(LoadedPage& loaded_page, Mutation mutation)
{
    auto count = card_count_of(loaded_page.shape);
    auto alternate = loaded_page.style_change_count % 2 == 0;
    ++loaded_page.style_change_count;
    switch (mutation) {
    case Mutation::OneCardBackground:
        set_card_style(loaded_page, count / 2, alternate ? "#fdd"sv : "#dfd"sv, false);
        break;
    case Mutation::OneCardOutlineToggle:
        set_card_style(loaded_page, count / 2, "#dde"sv, alternate);
        break;
    case Mutation::TwoCardsBackground:
        set_card_style(loaded_page, count / 4, alternate ? "#fdd"sv : "#dfd"sv, false);
        set_card_style(loaded_page, count * 3 / 4, alternate ? "#dfd"sv : "#fdd"sv, false);
        break;
    case Mutation::NegativeZIndexCardOutlineToggle: {
        auto negative = loaded_page.document().get_element_by_id("negative"_utf16);
        VERIFY(negative);
        negative->set_attribute_value(Web::HTML::AttributeNames::style, alternate ? "outline:2px solid #800"_utf16 : "outline:none"_utf16);
        break;
    }
    case Mutation::OneInTwentyCardsBackground:
        for (size_t index = 10; index < count; index += 20)
            set_card_style(loaded_page, index, alternate ? "#fdd"sv : "#dfd"sv, false);
        break;
    case Mutation::OneInFiveCardsBackground:
        for (size_t index = 2; index < count; index += 5)
            set_card_style(loaded_page, index, alternate ? "#fdd"sv : "#dfd"sv, false);
        break;
    }
}

struct Samples {
    Vector<u64> microseconds;

    void report(StringView label)
    {
        quick_sort(microseconds);
        u64 total = 0;
        for (auto sample : microseconds)
            total += sample;
        outln("{}: median {} µs, min {} µs, mean {} µs over {} recordings", label, microseconds[microseconds.size() / 2], microseconds.first(), total / microseconds.size(), microseconds.size());
    }
};

// The style change and its layout update are timed separately from the recording, so the invalidation and
// relayout cost of each mutation is visible next to the recording it triggers.
void time_document_recordings(DocumentShape shape, StringView label, Optional<Mutation> mutation, CacheState cache_state = CacheState::Warm)
{
    auto& loaded_page = page_for(shape);
    auto& document = loaded_page.document();
    Samples layout_samples;
    Samples samples;
    size_t recorded_byte_count = 0;
    for (size_t i = 0; i < timed_iterations; ++i) {
        auto layout_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        if (mutation.has_value())
            apply_mutation(loaded_page, *mutation);
        document.update_layout(Web::DOM::UpdateLayoutReason::Debugging);
        layout_samples.microseconds.append(layout_timer.elapsed_time().to_microseconds());
        if (cache_state == CacheState::Cold)
            document.paint_state().invalidate_all_cached_paint(document);
        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        auto display_list = document.record_display_list(benchmark_paint_config(), loaded_page.display_list_resource_storage, Web::Painting::PaintCommandCacheMode::ReadWrite);
        samples.microseconds.append(timer.elapsed_time().to_microseconds());
        VERIFY(display_list);
        recorded_byte_count = display_list->command_bytes().size();
    }
    outln("{} bytes of display list commands", recorded_byte_count);
    layout_samples.report("  mutation and layout"sv);
    samples.report(label);
}

// The Rust recording and publication alone, without the document-level bookkeeping that follows a recording
// (collecting referenced resources and rebuilding the hit-test display list).
void time_rust_recordings(DocumentShape shape, StringView label)
{
    auto& loaded_page = page_for(shape);
    auto& document = loaded_page.document();
    Samples samples;
    for (size_t i = 0; i < timed_iterations; ++i) {
        apply_mutation(loaded_page, Mutation::OneCardBackground);
        document.update_layout(Web::DOM::UpdateLayoutReason::Debugging);
        document.update_paint_and_hit_testing_properties_if_needed();
        auto visual_context_tree = document.paint_state().visual_context_tree(document);
        auto placeholder_display_list = Web::Painting::DisplayList::create_from_command_bytes(visual_context_tree, {}, {});
        Web::Painting::InspectorOverlayInputs overlay_inputs;
        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        auto display_list = Web::Painting::record_rust_display_list(document, *placeholder_display_list, loaded_page.display_list_resource_storage, Web::Painting::PaintCommandCacheMode::ReadWrite, benchmark_paint_config(), overlay_inputs);
        samples.microseconds.append(timer.elapsed_time().to_microseconds());
        VERIFY(display_list);
    }
    samples.report(label);
}

void time_removal_recordings(DocumentShape shape, StringView label)
{
    auto& loaded_page = page_for(shape);
    auto& document = loaded_page.document();
    auto card = GC::make_root(*document.get_element_by_id("card-1000"_utf16));
    auto parent = GC::make_root(*card->parent());
    Samples layout_samples;
    Samples recording_samples;
    for (size_t iteration = 0; iteration < timed_iterations; ++iteration) {
        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        if (card->parent())
            card->remove();
        else
            MUST(parent->append_child(*card));
        document.update_layout(Web::DOM::UpdateLayoutReason::Debugging);
        layout_samples.microseconds.append(timer.elapsed_time().to_microseconds());
        timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        VERIFY(document.record_display_list(benchmark_paint_config(), loaded_page.display_list_resource_storage, Web::Painting::PaintCommandCacheMode::ReadWrite));
        recording_samples.microseconds.append(timer.elapsed_time().to_microseconds());
    }
    layout_samples.report("  removal/attachment and layout"sv);
    recording_samples.report(label);
}

}

TEST_SETUP
{
    initialize_web_platform();
}

BENCHMARK_CASE(document_record_after_one_card_style_change_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, one dirty"sv, Mutation::OneCardBackground);
}

BENCHMARK_CASE(document_record_after_one_card_style_change_among_2000_nested)
{
    time_document_recordings(DocumentShape::NestedPanels, "document recording, nested panels, one dirty"sv, Mutation::OneCardBackground);
}

BENCHMARK_CASE(document_record_after_one_card_style_change_among_200_flat)
{
    time_document_recordings(DocumentShape::FlatCardsFew, "document recording, 200 flat cards, one dirty"sv, Mutation::OneCardBackground);
}

BENCHMARK_CASE(document_record_after_one_card_grows_or_shrinks_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, one dirty changing size"sv, Mutation::OneCardOutlineToggle);
}

BENCHMARK_CASE(document_record_after_two_cards_change_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, two dirty"sv, Mutation::TwoCardsBackground);
}

BENCHMARK_CASE(document_record_after_negative_z_index_card_changes_size_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, negative z-index dirty changing size"sv, Mutation::NegativeZIndexCardOutlineToggle);
}

BENCHMARK_CASE(document_record_after_one_in_twenty_cards_change_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, 100 dirty"sv, Mutation::OneInTwentyCardsBackground);
}

BENCHMARK_CASE(document_record_after_one_in_five_cards_change_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, 400 dirty"sv, Mutation::OneInFiveCardsBackground);
}

BENCHMARK_CASE(document_record_cold_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, cold"sv, Mutation::OneCardBackground, CacheState::Cold);
}

BENCHMARK_CASE(document_record_cold_among_2000_nested)
{
    time_document_recordings(DocumentShape::NestedPanels, "document recording, nested panels, cold"sv, Mutation::OneCardBackground, CacheState::Cold);
}

BENCHMARK_CASE(rust_record_after_one_card_style_change_among_2000_flat)
{
    time_rust_recordings(DocumentShape::FlatCards, "rust recording, flat cards, one dirty"sv);
}

BENCHMARK_CASE(rust_record_after_one_card_style_change_among_2000_nested)
{
    time_rust_recordings(DocumentShape::NestedPanels, "rust recording, nested panels, one dirty"sv);
}

BENCHMARK_CASE(document_record_quiet_frame_among_2000_flat)
{
    time_document_recordings(DocumentShape::FlatCards, "document recording, flat cards, quiet"sv, {});
}

BENCHMARK_CASE(document_record_after_removal_in_shared_stacking_context)
{
    time_removal_recordings(DocumentShape::SharedContextCards, "document recording, shared context, one card removed/attached"sv);
}

BENCHMARK_CASE(document_record_after_removal_among_tokenized_cards)
{
    time_removal_recordings(DocumentShape::TokenizedCards, "document recording, tokenized cards, one card removed/attached"sv);
}

BENCHMARK_CASE(document_record_after_viewport_scroll_and_one_card_change)
{
    auto loaded_page = load_page(DocumentShape::FlatCards);
    auto& document = loaded_page->document();
    document.document_element()->set_attribute("style"_fly_string, "height:2400px"_utf16);
    VERIFY(record_display_list(*loaded_page));
    Samples layout_samples;
    Samples recording_samples;
    for (size_t iteration = 0; iteration < timed_iterations; ++iteration) {
        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        loaded_page->page->local_root_navigable()->perform_scroll_of_viewport_scrolling_box({ 0, iteration % 2 ? 200 : 300 });
        apply_mutation(*loaded_page, Mutation::OneCardBackground);
        document.update_layout(Web::DOM::UpdateLayoutReason::Debugging);
        layout_samples.microseconds.append(timer.elapsed_time().to_microseconds());
        timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        VERIFY(document.record_display_list(benchmark_paint_config(), loaded_page->display_list_resource_storage, Web::Painting::PaintCommandCacheMode::ReadWrite));
        recording_samples.microseconds.append(timer.elapsed_time().to_microseconds());
    }
    layout_samples.report("  scrolling, mutation and layout"sv);
    recording_samples.report("document recording, viewport scroll and one changed card"sv);
}
