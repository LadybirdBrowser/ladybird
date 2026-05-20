/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/StringBuilder.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibGfx/Color.h>
#include <LibGfx/Palette.h>
#include <LibMain/Main.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/ParserScriptingMode.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>

class DumpHTMLTreePageClient final : public Web::PageClient {
    GC_CELL(DumpHTMLTreePageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(DumpHTMLTreePageClient);

public:
    static GC::Ref<DumpHTMLTreePageClient> create(JS::VM& vm)
    {
        return vm.heap().allocate<DumpHTMLTreePageClient>();
    }

    virtual ~DumpHTMLTreePageClient() override = default;

    void set_page(GC::Ref<Web::Page> page) { m_page = page; }

    virtual u64 id() const override { return 0; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { return Gfx::Palette(*m_palette_impl); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1.0; }
    virtual double device_pixel_ratio() const override { return 1.0; }
    virtual double device_pixels_per_css_pixel() const override { return 1.0; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::Auto; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { return m_input_event_queue; }
    virtual void report_finished_handling_input_event([[maybe_unused]] u64 page_id, [[maybe_unused]] Web::EventResult event_was_handled) override { }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual Web::DisplayListPlayerType display_list_player_type() const override { return Web::DisplayListPlayerType::SkiaCPU; }
    virtual bool is_headless() const override { return true; }

private:
    DumpHTMLTreePageClient()
    {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)));
        auto* theme = buffer.data<Gfx::SystemTheme>();
        theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::White).value();
        theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::Black).value();
        m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Queue<Web::QueuedInputEvent> m_input_event_queue;
};

GC_DEFINE_ALLOCATOR(DumpHTMLTreePageClient);

static String escape_for_dump(String const& string)
{
    StringBuilder builder;
    for (auto code_point : string.code_points()) {
        switch (code_point) {
        case '\n':
            builder.append("\\n"sv);
            break;
        case '\r':
            builder.append("\\r"sv);
            break;
        case '\t':
            builder.append("\\t"sv);
            break;
        case '"':
            builder.append("\\\""sv);
            break;
        case '\\':
            builder.append("\\\\"sv);
            break;
        default:
            if (code_point < 0x20 || code_point == 0x7f) {
                builder.append("\\u{"sv);
                builder.appendff("{:04X}", code_point);
                builder.append('}');
            } else {
                builder.append_code_point(code_point);
            }
            break;
        }
    }
    return MUST(builder.to_string());
}

static String escape_for_dump(Utf16String const& string)
{
    return escape_for_dump(string.to_utf8());
}

static StringView namespace_label(Optional<FlyString> const& namespace_uri)
{
    if (!namespace_uri.has_value())
        return "none"sv;
    if (namespace_uri == Web::Namespace::HTML)
        return "html"sv;
    if (namespace_uri == Web::Namespace::MathML)
        return "mathml"sv;
    if (namespace_uri == Web::Namespace::SVG)
        return "svg"sv;
    if (namespace_uri == Web::Namespace::XLink)
        return "xlink"sv;
    if (namespace_uri == Web::Namespace::XML)
        return "xml"sv;
    if (namespace_uri == Web::Namespace::XMLNS)
        return "xmlns"sv;
    return namespace_uri->bytes_as_string_view();
}

static StringView quirks_mode_label(Web::DOM::Document const& document)
{
    if (document.in_quirks_mode())
        return "quirks"sv;
    if (document.in_limited_quirks_mode())
        return "limited-quirks"sv;
    return "no-quirks"sv;
}

static void dump_line_prefix(size_t indent)
{
    out("|");
    for (size_t i = 0; i < indent; ++i)
        out("  ");
    out(" ");
}

static void dump_tree(Web::DOM::Node const&, size_t indent = 0);

static void dump_children(Web::DOM::Node const& node, size_t indent)
{
    for (auto const* child = node.first_child(); child; child = child->next_sibling())
        dump_tree(*child, indent);
}

static void dump_attribute(Web::DOM::Attr const& attribute, size_t indent)
{
    dump_line_prefix(indent);
    out("@{}", attribute.name());
    if (attribute.namespace_uri().has_value())
        out(" namespace=\"{}\"", namespace_label(attribute.namespace_uri()));
    outln(" value=\"{}\"", escape_for_dump(attribute.value()));
}

static void dump_element(Web::DOM::Element const& element, size_t indent)
{
    dump_line_prefix(indent);
    out("<{}", element.local_name());
    if (element.prefix().has_value())
        out(" prefix=\"{}\"", element.prefix().value());
    outln(" namespace=\"{}\">", namespace_label(element.namespace_uri()));

    element.for_each_attribute([&](auto const& attribute) {
        dump_attribute(attribute, indent + 1);
    });

    if (auto shadow_root = element.shadow_root(); shadow_root && !shadow_root->is_user_agent_internal()) {
        dump_line_prefix(indent + 1);
        out("#shadow-root mode=\"{}\"", shadow_root->mode() == Web::Bindings::ShadowRootMode::Open ? "open"sv : "closed"sv);
        if (shadow_root->declarative())
            out(" declarative");
        if (shadow_root->clonable())
            out(" clonable");
        if (shadow_root->serializable())
            out(" serializable");
        if (shadow_root->delegates_focus())
            out(" delegates-focus");
        outln();
        dump_children(*shadow_root, indent + 2);
    }

    if (is<Web::HTML::HTMLTemplateElement>(element)) {
        auto const& template_element = as<Web::HTML::HTMLTemplateElement>(element);
        dump_line_prefix(indent + 1);
        outln("#template-content");
        dump_children(template_element.content(), indent + 2);
    }

    dump_children(element, indent + 1);
}

static void dump_tree(Web::DOM::Node const& node, size_t indent)
{
    switch (node.type()) {
    case Web::DOM::NodeType::DOCUMENT_NODE: {
        auto const& document = as<Web::DOM::Document>(node);
        outln("#document mode=\"{}\" url=\"{}\"", quirks_mode_label(document), document.url().serialize());
        dump_children(document, indent);
        break;
    }
    case Web::DOM::NodeType::DOCUMENT_TYPE_NODE: {
        auto const& doctype = as<Web::DOM::DocumentType>(node);
        dump_line_prefix(indent);
        out("<!DOCTYPE");
        if (!doctype.name().is_empty())
            out(" {}", doctype.name());
        if (!doctype.public_id().is_empty())
            out(" public_id=\"{}\"", escape_for_dump(doctype.public_id()));
        if (!doctype.system_id().is_empty())
            out(" system_id=\"{}\"", escape_for_dump(doctype.system_id()));
        outln(">");
        break;
    }
    case Web::DOM::NodeType::DOCUMENT_FRAGMENT_NODE:
        dump_line_prefix(indent);
        outln(node.is_shadow_root() ? "#shadow-root"sv : "#document-fragment"sv);
        dump_children(node, indent + 1);
        break;
    case Web::DOM::NodeType::ELEMENT_NODE:
        dump_element(as<Web::DOM::Element>(node), indent);
        break;
    case Web::DOM::NodeType::TEXT_NODE:
        dump_line_prefix(indent);
        outln("#text \"{}\"", escape_for_dump(as<Web::DOM::CharacterData>(node).data()));
        break;
    case Web::DOM::NodeType::COMMENT_NODE:
        dump_line_prefix(indent);
        outln("#comment \"{}\"", escape_for_dump(as<Web::DOM::CharacterData>(node).data()));
        break;
    case Web::DOM::NodeType::CDATA_SECTION_NODE:
        dump_line_prefix(indent);
        outln("#cdata-section \"{}\"", escape_for_dump(as<Web::DOM::CharacterData>(node).data()));
        break;
    case Web::DOM::NodeType::PROCESSING_INSTRUCTION_NODE:
        dump_line_prefix(indent);
        outln("#processing-instruction \"{}\"", escape_for_dump(as<Web::DOM::CharacterData>(node).data()));
        break;
    case Web::DOM::NodeType::ATTRIBUTE_NODE:
        dump_attribute(as<Web::DOM::Attr>(node), indent);
        break;
    case Web::DOM::NodeType::INVALID:
    case Web::DOM::NodeType::ENTITY_REFERENCE_NODE:
    case Web::DOM::NodeType::ENTITY_NODE:
    case Web::DOM::NodeType::NOTATION_NODE:
        dump_line_prefix(indent);
        outln("#unsupported-node type={}", to_underlying(node.type()));
        break;
    }
}

static GC::Ref<Web::DOM::Document> parse_html(JS::Realm& realm, Web::DOM::Document const& origin_document, StringView input)
{
    auto document = Web::DOM::Document::create(realm, URL::about_blank());
    document->set_document_type(Web::DOM::Document::Type::HTML);
    document->set_content_type("text/html"_string);
    document->set_origin(origin_document.origin());
    document->set_ready_to_run_scripts();
    document->set_allow_declarative_shadow_roots(true);
    document->set_custom_element_registry(realm.create<Web::HTML::CustomElementRegistry>(realm));

    auto parser = Web::HTML::HTMLParser::create(document, input, Web::HTML::ParserScriptingMode::Disabled, "UTF-8"sv);
    parser->run(URL::about_blank());
    return document;
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView file_path;
    bool silent = false;
    int iterations = 1;

    Core::ArgsParser args_parser;
    args_parser.set_general_help(
        "Parse HTML and dump the resulting DOM tree in a canonical format. "
        "Use --silent for perf work on the parser itself.");
    args_parser.add_positional_argument(file_path, "Path to HTML file (or - for stdin)", "file", Core::ArgsParser::Required::No);
    args_parser.add_option(silent, "Don't print anything (for benchmarking)", "silent", 'q');
    args_parser.add_option(iterations, "Run the parser N times, reporting timing unless --silent is used", "iterations", 'n', "count");
    args_parser.parse(arguments);

    if (iterations < 1) {
        warnln("--iterations must be at least 1");
        return 1;
    }

    if (silent)
        AK::set_debug_enabled(false);

    ByteBuffer input_data;
    if (file_path.is_empty() || file_path == "-"sv) {
        auto stdin_file = TRY(Core::File::standard_input());
        input_data = TRY(stdin_file->read_until_eof());
    } else {
        auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
        input_data = TRY(file->read_until_eof());
    }

    StringView input { input_data };

    [[maybe_unused]] Core::EventLoop event_loop;
    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);
    Web::Platform::FontPlugin::install(*new Web::Platform::FontPlugin(false));
    Web::Bindings::initialize_main_thread_vm(Web::Bindings::AgentType::SimilarOriginWindow);

    auto& vm = Web::Bindings::main_thread_vm();
    auto page_client = DumpHTMLTreePageClient::create(vm);
    auto page = Web::Page::create(vm, page_client);
    page->set_is_scripting_enabled(false);
    page_client->set_page(page);
    page->set_top_level_traversable(Web::HTML::TraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {}));
    auto& origin_document = *page->top_level_traversable()->active_document();
    auto& realm = origin_document.realm();

    auto timer = Core::ElapsedTimer::start_new();

    for (int i = 0; i < iterations; i++) {
        auto document = parse_html(realm, origin_document, input);
        if (!silent && i == 0)
            dump_tree(document);
    }

    if (iterations > 1 && !silent) {
        auto elapsed_ms = timer.elapsed_milliseconds();
        warnln("input={}B iterations={} total={}ms ({:.3f}ms/iter)",
            input_data.size(),
            iterations,
            elapsed_ms,
            static_cast<double>(elapsed_ms) / iterations);
    }

    return 0;
}
