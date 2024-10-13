/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Queue.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibXML/DOM/Document.h>
#include <LibXML/DOM/Node.h>
#include <LibXML/Parser/Parser.h>

static bool g_color = false;
static bool g_only_contents = false;

enum class ColorRole {
    PITag,
    PITarget,
    PIData,
    AttributeName,
    Eq,
    AttributeValue,
    Tag,
    Text,
    Comment,
    Reset,
    Doctype,
    Keyword,
};
static void color(ColorRole role)
{
    if (!g_color)
        return;

    switch (role) {
    case ColorRole::PITag:
    case ColorRole::Doctype:
        out("\x1b[{};{}m", 1, "38;5;223");
        break;
    case ColorRole::PITarget:
        out("\x1b[{};{}m", 1, "38;5;23");
        break;
    case ColorRole::PIData:
        out("\x1b[{};{}m", 1, "38;5;43");
        break;
    case ColorRole::AttributeName:
        out("\x1b[38;5;27m");
        break;
    case ColorRole::Eq:
        break;
    case ColorRole::AttributeValue:
        out("\x1b[38;5;46m");
        break;
    case ColorRole::Tag:
        out("\x1b[{};{}m", 1, "38;5;220");
        break;
    case ColorRole::Text:
        break;
    case ColorRole::Comment:
        out("\x1b[{};{}m", 3, "38;5;250");
        break;
    case ColorRole::Reset:
        out("\x1b[0m");
        break;
    case ColorRole::Keyword:
        out("\x1b[38;5;40m");
        break;
    }
}

static void dump(XML::Node const& node)
{
    node.content.visit(
        [](XML::Node::Text const& text) {
            out("{}", text.builder.string_view());
        },
        [](XML::Node::Comment const& comment) {
            color(ColorRole::Comment);
            out("<!--{}-->", comment.text);
            color(ColorRole::Reset);
        },
        [](XML::Node::Element const& element) {
            color(ColorRole::Tag);
            out("<{}", element.name);
            color(ColorRole::Reset);

            if (!element.attributes.is_empty()) {
                for (auto& attribute : element.attributes) {
                    auto quote = attribute.value.contains('"') ? '\'' : '"';
                    color(ColorRole::AttributeName);
                    out(" {}", attribute.key);
                    color(ColorRole::Eq);
                    out("=");
                    color(ColorRole::AttributeValue);
                    out("{}{}{}", quote, attribute.value, quote);
                    color(ColorRole::Reset);
                }
            }
            if (element.children.is_empty()) {
                color(ColorRole::Tag);
                out("/>");
                color(ColorRole::Reset);
            } else {
                color(ColorRole::Tag);
                out(">");
                color(ColorRole::Reset);

                for (auto& node : element.children)
                    dump(*node);

                color(ColorRole::Tag);
                out("</{}>", element.name);
                color(ColorRole::Reset);
            }
        });
}

static void dump(XML::Document& document)
{
    if (!g_only_contents) {
        {
            color(ColorRole::PITag);
            out("<?");
            color(ColorRole::Reset);
            color(ColorRole::PITarget);
            out("xml");
            color(ColorRole::Reset);
            color(ColorRole::PIData);
            out(" version='{}'", document.version() == XML::Version::Version10 ? "1.0" : "1.1");
            color(ColorRole::Reset);
            color(ColorRole::PITag);
            outln("?>");
        }

        for (auto& pi : document.processing_instructions()) {
            color(ColorRole::PITag);
            out("<?");
            color(ColorRole::Reset);
            color(ColorRole::PITarget);
            out("{}", pi.key);
            color(ColorRole::Reset);
            if (!pi.value.is_empty()) {
                color(ColorRole::PIData);
                out(" {}", pi.value);
                color(ColorRole::Reset);
            }
            color(ColorRole::PITag);
            outln("?>");
        }

        if (auto maybe_doctype = document.doctype(); maybe_doctype.has_value()) {
            auto& doctype = *maybe_doctype;
            color(ColorRole::Doctype);
            out("<!DOCTYPE ");
            color(ColorRole::Tag);
            out("{}", doctype.type);
            if (!doctype.markup_declarations.is_empty()) {
                color(ColorRole::Reset);
                out(" [\n");
                for (auto& entry : doctype.markup_declarations) {
                    entry.visit(
                        [&](XML::ElementDeclaration const& element) {
                            color(ColorRole::Doctype);
                            out("    <!ELEMENT ");
                            color(ColorRole::Tag);
                            out("{} ", element.type);
                            element.content_spec.visit(
                                [&](XML::ElementDeclaration::Empty const&) {
                                    color(ColorRole::Keyword);
                                    out("EMPTY");
                                },
                                [&](XML::ElementDeclaration::Any const&) {
                                    color(ColorRole::Keyword);
                                    out("ANY");
                                },
                                [&](XML::ElementDeclaration::Mixed const&) {
                                },
                                [&](XML::ElementDeclaration::Children const&) {
                                });
                            color(ColorRole::Doctype);
                            outln(">");
                        },
                        [&](XML::AttributeListDeclaration const& list) {
                            color(ColorRole::Doctype);
                            out("    <!ATTLIST ");
                            color(ColorRole::Tag);
                            out("{}", list.type);
                            for (auto& attribute : list.attributes) {
                                color(ColorRole::AttributeName);
                                out(" {} ", attribute.name);
                                color(ColorRole::Keyword);
                                attribute.type.visit(
                                    [](XML::AttributeListDeclaration::StringType) {
                                        out("CDATA");
                                    },
                                    [](XML::AttributeListDeclaration::TokenizedType type) {
                                        switch (type) {
                                        case XML::AttributeListDeclaration::TokenizedType::ID:
                                            out("ID");
                                            break;
                                        case XML::AttributeListDeclaration::TokenizedType::IDRef:
                                            out("IDREF");
                                            break;
                                        case XML::AttributeListDeclaration::TokenizedType::IDRefs:
                                            out("IDREFS");
                                            break;
                                        case XML::AttributeListDeclaration::TokenizedType::Entity:
                                            out("ENTITY");
                                            break;
                                        case XML::AttributeListDeclaration::TokenizedType::Entities:
                                            out("ENTITIES");
                                            break;
                                        case XML::AttributeListDeclaration::TokenizedType::NMToken:
                                            out("NMTOKEN");
                                            break;
                                        case XML::AttributeListDeclaration::TokenizedType::NMTokens:
                                            out("NMTOKENS");
                                            break;
                                        }
                                    },
                                    [](XML::AttributeListDeclaration::NotationType const& type) {
                                        out("NOTATION ");
                                        color(ColorRole::Reset);
                                        out("( ");
                                        bool first = true;
                                        for (auto& name : type.names) {
                                            color(ColorRole::Reset);
                                            if (first)
                                                first = false;
                                            else
                                                out(" | ");
                                            color(ColorRole::AttributeValue);
                                            out("{}", name);
                                        }
                                        color(ColorRole::Reset);
                                        out(" )");
                                    },
                                    [](XML::AttributeListDeclaration::Enumeration const& type) {
                                        color(ColorRole::Reset);
                                        out("( ");
                                        bool first = true;
                                        for (auto& name : type.tokens) {
                                            color(ColorRole::Reset);
                                            if (first)
                                                first = false;
                                            else
                                                out(" | ");
                                            color(ColorRole::AttributeValue);
                                            out("{}", name);
                                        }
                                        color(ColorRole::Reset);
                                        out(" )");
                                    });
                                out(" ");
                                attribute.default_.visit(
                                    [](XML::AttributeListDeclaration::Required) {
                                        color(ColorRole::Keyword);
                                        out("#REQUIRED");
                                    },
                                    [](XML::AttributeListDeclaration::Implied) {
                                        color(ColorRole::Keyword);
                                        out("#IMPLIED");
                                    },
                                    [](XML::AttributeListDeclaration::Fixed const& fixed) {
                                        color(ColorRole::Keyword);
                                        out("#FIXED ");
                                        color(ColorRole::AttributeValue);
                                        out("\"{}\"", fixed.value);
                                    },
                                    [](XML::AttributeListDeclaration::DefaultValue const& default_) {
                                        color(ColorRole::AttributeValue);
                                        out("\"{}\"", default_.value);
                                    });
                            }
                            color(ColorRole::Doctype);
                            outln(">");
                        },
                        [&](XML::EntityDeclaration const& entity) {
                            color(ColorRole::Doctype);
                            out("    <!ENTITY ");
                            entity.visit(
                                [](XML::GEDeclaration const& declaration) {
                                    color(ColorRole::Tag);
                                    out("{} ", declaration.name);
                                    declaration.definition.visit(
                                        [](ByteString const& value) {
                                            color(ColorRole::AttributeValue);
                                            out("\"{}\"", value);
                                        },
                                        [](XML::EntityDefinition const& definition) {
                                            if (definition.id.public_id.has_value()) {
                                                color(ColorRole::Keyword);
                                                out("PUBLIC ");
                                                color(ColorRole::PITarget);
                                                out("\"{}\" ", definition.id.public_id->public_literal);
                                            } else {
                                                color(ColorRole::Keyword);
                                                out("SYSTEM ");
                                            }
                                            color(ColorRole::PITarget);
                                            out("\"{}\" ", definition.id.system_id.system_literal);

                                            if (definition.notation.has_value()) {
                                                color(ColorRole::Keyword);
                                                out(" NDATA ");
                                                color(ColorRole::PITarget);
                                                out("{}", *definition.notation);
                                            }
                                        });
                                    color(ColorRole::Tag);
                                    outln(">");
                                },
                                [](XML::PEDeclaration const& declaration) {
                                    color(ColorRole::Tag);
                                    out("{} ", declaration.name);
                                    declaration.definition.visit(
                                        [](ByteString const& value) {
                                            color(ColorRole::AttributeValue);
                                            out("\"{}\"", value);
                                        },
                                        [](XML::ExternalID const& id) {
                                            if (id.public_id.has_value()) {
                                                color(ColorRole::Keyword);
                                                out("PUBLIC ");
                                                color(ColorRole::PITarget);
                                                out("\"{}\" ", id.public_id->public_literal);
                                            } else {
                                                color(ColorRole::Keyword);
                                                out("SYSTEM ");
                                            }
                                            color(ColorRole::PITarget);
                                            out("\"{}\"", id.system_id.system_literal);
                                        });
                                    color(ColorRole::Tag);
                                    outln(">");
                                });
                        },
                        [&](XML::NotationDeclaration const&) {

                        });
                }
                color(ColorRole::Reset);
                out("]");
            }
            color(ColorRole::Doctype);
            outln(">");
        }
    }
    dump(document.root());
}

static ByteString s_path;
static auto parse(StringView contents)
{
    return XML::Parser {
        contents,
        {
            .preserve_comments = true,
            .resolve_external_resource = [&](XML::SystemID const& system_id, Optional<XML::PublicID> const&) -> ErrorOr<Variant<ByteString, Vector<XML::MarkupDeclaration>>> {
                auto base = URL::create_with_file_scheme(s_path);
                auto url = URL::Parser::basic_parse(system_id.system_literal, base);
                if (!url.is_valid())
                    return Error::from_string_literal("Invalid URL");

                if (url.scheme() != "file")
                    return Error::from_string_literal("NYI: Nonlocal entity");

                auto file = TRY(Core::File::open(URL::percent_decode(url.serialize_path()), Core::File::OpenMode::Read));
                return ByteString::copy(TRY(file->read_until_eof()));
            },
        },
    };
}

enum class TestResult {
    Passed,
    Failed,
    RunnerFailed,
};
static HashMap<ByteString, TestResult> s_test_results {};
static void do_run_tests(XML::Document& document)
{
    auto& root = document.root().content.get<XML::Node::Element>();
    VERIFY(root.name == "TESTSUITE");
    Queue<XML::Node*> suites;
    auto dump_cases = [&](auto& root) {
        for (auto& node : root.children) {
            auto element = node->content.template get_pointer<XML::Node::Element>();
            if (!element)
                continue;
            if (element->name != "TESTCASES" && element->name != "TEST")
                continue;
            suites.enqueue(node);
        }
    };

    dump_cases(root);

    auto base_path = LexicalPath::dirname(s_path);

    while (!suites.is_empty()) {
        auto& node = *suites.dequeue();
        auto& suite = node.content.get<XML::Node::Element>();
        if (suite.name == "TESTCASES") {
            dump_cases(suite);
            continue;
        }
        if (suite.name == "TEST") {
            Vector<StringView> bases;
            for (auto* parent = node.parent; parent; parent = parent->parent) {
                auto& attributes = parent->content.get<XML::Node::Element>().attributes;
                auto it = attributes.find("xml:base");
                if (it == attributes.end())
                    continue;
                bases.append(it->value);
            }

            auto type = suite.attributes.find("TYPE")->value;

            StringBuilder path_builder;
            path_builder.append(base_path);
            path_builder.append('/');
            for (auto& entry : bases.in_reverse()) {
                path_builder.append(entry);
                path_builder.append('/');
            }
            auto test_base_path = path_builder.to_byte_string();

            path_builder.append(suite.attributes.find("URI")->value);
            auto url = URL::create_with_file_scheme(path_builder.string_view());
            if (!url.is_valid()) {
                warnln("Invalid URL {}", path_builder.string_view());
                s_test_results.set(path_builder.string_view(), TestResult::RunnerFailed);
                continue;
            }

            auto file_path = URL::percent_decode(url.serialize_path());
            auto file_result = Core::File::open(file_path, Core::File::OpenMode::Read);
            if (file_result.is_error()) {
                warnln("Read error for {}: {}", file_path, file_result.error());
                s_test_results.set(file_path, TestResult::RunnerFailed);
                continue;
            }

            warnln("Running test {}", file_path);

            auto contents = file_result.value()->read_until_eof();
            if (contents.is_error()) {
                warnln("Read error for {}: {}", file_path, contents.error());
                s_test_results.set(file_path, TestResult::RunnerFailed);
                continue;
            }
            auto parser = parse(contents.value());
            auto doc_or_error = parser.parse();
            if (doc_or_error.is_error()) {
                if (type == "invalid" || type == "error" || type == "not-wf")
                    s_test_results.set(file_path, TestResult::Passed);
                else
                    s_test_results.set(file_path, TestResult::Failed);
                continue;
            }

            auto out = suite.attributes.find("OUTPUT");
            if (out != suite.attributes.end()) {
                auto out_path = LexicalPath::join(test_base_path, out->value).string();
                auto file_result = Core::File::open(out_path, Core::File::OpenMode::Read);
                if (file_result.is_error()) {
                    warnln("Read error for {}: {}", out_path, file_result.error());
                    s_test_results.set(file_path, TestResult::RunnerFailed);
                    continue;
                }
                auto contents = file_result.value()->read_until_eof();
                if (contents.is_error()) {
                    warnln("Read error for {}: {}", out_path, contents.error());
                    s_test_results.set(file_path, TestResult::RunnerFailed);
                    continue;
                }
                auto parser = parse(contents.value());
                auto out_doc_or_error = parser.parse();
                if (out_doc_or_error.is_error()) {
                    warnln("Parse error for {}: {}", out_path, out_doc_or_error.error());
                    s_test_results.set(file_path, TestResult::RunnerFailed);
                    continue;
                }
                auto out_doc = out_doc_or_error.release_value();
                if (out_doc.root() != doc_or_error.value().root()) {
                    s_test_results.set(file_path, TestResult::Failed);
                    continue;
                }
            }

            if (type == "invalid" || type == "error" || type == "not-wf")
                s_test_results.set(file_path, TestResult::Failed);
            else
                s_test_results.set(file_path, TestResult::Passed);
        }
    }
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView filename;
    bool run_tests { false };

    Core::ArgsParser parser;
    parser.set_general_help("Parse and dump XML files");
    parser.add_option(g_color, "Syntax highlight the output", "color", 'c');
    parser.add_option(g_only_contents, "Only display markup and text", "only-contents", 'o');
    parser.add_option(run_tests, "Run tests", "run-tests", 't');
    parser.add_positional_argument(filename, "File to read from", "file");
    parser.parse(arguments);

    s_path = TRY(FileSystem::real_path(filename));
    auto file = TRY(Core::File::open(s_path, Core::File::OpenMode::Read));
    auto contents = TRY(file->read_until_eof());

    auto xml_parser = parse(contents);
    auto result = xml_parser.parse();
    if (result.is_error()) {
        if (xml_parser.parse_error_causes().is_empty()) {
            warnln("{}", result.error());
        } else {
            warnln("{}; caused by:", result.error());
            for (auto const& cause : xml_parser.parse_error_causes())
                warnln("    {}", cause);
        }
        return 1;
    }

    auto doc = result.release_value();
    if (run_tests) {
        do_run_tests(doc);
        size_t passed = 0;
        size_t failed = 0;
        size_t runner_error = 0;
        size_t total = 0;
        for (auto& entry : s_test_results) {
            total++;
            switch (entry.value) {
            case TestResult::Passed:
                passed++;
                break;
            case TestResult::Failed:
                failed++;
                break;
            case TestResult::RunnerFailed:
                runner_error++;
                break;
            }
        }
        outln("{} passed, {} failed, {} runner failed of {} tests run.", passed, failed, runner_error, total);
        return 0;
    }

    dump(doc);
    if (!g_only_contents)
        outln();

    return 0;
}
