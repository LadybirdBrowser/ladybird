/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibWebView/BookmarkHTMLImporter.h>

namespace WebView {

// The NETSCAPE-Bookmark-file-1 format is a simple HTML-like format used by all major browsers
// for bookmark import/export. It uses <DT><A> for bookmarks
// and <DT><H3> for folders, with <DL><p> for nesting.

static String decode_html_entities(StringView text)
{
    StringBuilder builder;

    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '&') {
            auto remaining = text.substring_view(i);
            if (remaining.starts_with("&amp;"sv)) {
                builder.append('&');
                i += 4;
            } else if (remaining.starts_with("&lt;"sv)) {
                builder.append('<');
                i += 3;
            } else if (remaining.starts_with("&gt;"sv)) {
                builder.append('>');
                i += 3;
            } else if (remaining.starts_with("&quot;"sv)) {
                builder.append('"');
                i += 5;
            } else if (remaining.starts_with("&apos;"sv)) {
                builder.append('\'');
                i += 5;
            } else if (remaining.starts_with("&#"sv)) {
                // Numeric character reference: &#123; or &#x1F;
                auto semicolon = remaining.find(';');
                if (semicolon.has_value()) {
                    auto number_part = remaining.substring_view(2, *semicolon - 2);
                    u32 code_point = 0;
                    bool valid = false;

                    if (number_part.starts_with('x') || number_part.starts_with('X')) {
                        // Hex
                        auto hex_part = number_part.substring_view(1);
                        auto parsed = hex_part.to_number<u32>(TrimWhitespace::No, 16);
                        if (parsed.has_value()) {
                            code_point = *parsed;
                            valid = true;
                        }
                    } else {
                        auto parsed = number_part.to_number<u32>(TrimWhitespace::No);
                        if (parsed.has_value()) {
                            code_point = *parsed;
                            valid = true;
                        }
                    }

                    if (valid && code_point > 0) {
                        builder.append_code_point(code_point);
                        i += *semicolon;
                    } else {
                        builder.append('&');
                    }
                } else {
                    builder.append('&');
                }
            } else {
                builder.append('&');
            }
        } else {
            builder.append(text[i]);
        }
    }

    return MUST(builder.to_string());
}

static Optional<String> extract_attribute(StringView tag, StringView attribute_name)
{
    auto tag_upper = MUST(String::from_utf8(tag)).to_ascii_uppercase();
    auto attr_upper = MUST(String::from_utf8(attribute_name)).to_ascii_uppercase();

    auto pos = tag_upper.bytes_as_string_view().find(attr_upper);
    if (!pos.has_value())
        return {};

    auto after_name = tag.substring_view(*pos + attribute_name.length());

    size_t i = 0;
    while (i < after_name.length() && (after_name[i] == ' ' || after_name[i] == '='))
        ++i;

    if (i >= after_name.length())
        return {};

    char quote = after_name[i];
    if (quote != '"' && quote != '\'')
        return {};

    ++i;
    auto start = i;
    while (i < after_name.length() && after_name[i] != quote)
        ++i;

    if (i >= after_name.length())
        return {};

    return MUST(String::from_utf8(after_name.substring_view(start, i - start)));
}

static StringView trim_whitespace(StringView sv)
{
    return sv.trim_whitespace();
}

static Optional<StringView> extract_text_content(StringView line, StringView close_tag)
{
    // Extract text between >text</TAG>
    // open_tag_end is what marks the end of the opening tag (e.g., ">" after attributes)
    // We find the last '>' before close_tag, which marks end of opening tag
    auto close_pos = line.find(close_tag);
    if (!close_pos.has_value())
        return {};

    auto text = line.substring_view(0, *close_pos);

    auto last_gt = text.find_last('>');
    if (!last_gt.has_value())
        return {};

    return text.substring_view(*last_gt + 1);
}

ErrorOr<Vector<BookmarkItem>> import_bookmarks_from_html(StringView html)
{
    Vector<BookmarkItem> root_items;

    Vector<Vector<BookmarkItem>*> folder_stack;
    folder_stack.append(&root_items);

    // Pending folder: when we see <DT><H3...>Title</H3>, we create a folder item
    // but wait for the next <DL> to start populating its children
    struct PendingFolder {
        Optional<String> title;
    };
    Optional<PendingFolder> pending_folder;

    auto lines = html.split_view('\n');

    for (auto const& raw_line : lines) {
        auto line = trim_whitespace(raw_line);

        if (line.is_empty())
            continue;

        if (line.contains("<DL>"sv) || line.contains("<DL><p>"sv)) {
            if (pending_folder.has_value()) {
                // Create the folder and push its children list
                auto* current_list = folder_stack.last();
                current_list->append({
                    .id = generate_random_uuid(),
                    .data = BookmarkItem::Folder {
                        .title = move(pending_folder->title),
                        .children = {},
                    },
                });
                pending_folder.clear();

                auto& new_folder = current_list->last();
                folder_stack.append(&new_folder.folder().children);
            }
            continue;
        }

        if (line.contains("</DL>"sv)) {
            if (folder_stack.size() > 1)
                folder_stack.take_last();
            continue;
        }

        if (line.contains("<H3"sv)) {
            auto title_text = extract_text_content(line, "</H3>"sv);
            Optional<String> title;
            if (title_text.has_value() && !title_text->is_empty())
                title = decode_html_entities(*title_text);

            pending_folder = PendingFolder {
                .title = move(title),
            };
            continue;
        }

        if (line.contains("<A "sv) || line.contains("<a "sv)) {
            auto href = extract_attribute(line, "HREF"sv);
            if (!href.has_value())
                continue;

            auto url = URL::Parser::basic_parse(*href);
            if (!url.has_value())
                continue;

            auto title_text = extract_text_content(line, "</A>"sv);
            if (!title_text.has_value())
                title_text = extract_text_content(line, "</a>"sv);

            Optional<String> title;
            if (title_text.has_value() && !title_text->is_empty())
                title = decode_html_entities(*title_text);

            Optional<String> favicon_base64;
            auto icon = extract_attribute(line, "ICON"sv);
            if (icon.has_value()) {
                auto icon_str = icon->bytes_as_string_view();
                auto base64_prefix = "data:image/png;base64,"sv;
                if (icon_str.starts_with(base64_prefix)) {
                    auto base64_data = icon_str.substring_view(base64_prefix.length());
                    if (!base64_data.is_empty())
                        favicon_base64 = MUST(String::from_utf8(base64_data));
                }
                if (!favicon_base64.has_value()) {
                    auto generic_prefix = "data:image/"sv;
                    if (icon_str.starts_with(generic_prefix)) {
                        auto after_type = icon_str.find(";base64,"sv);
                        if (after_type.has_value()) {
                            auto base64_data = icon_str.substring_view(*after_type + 8);
                            if (!base64_data.is_empty())
                                favicon_base64 = MUST(String::from_utf8(base64_data));
                        }
                    }
                }
            }

            auto* current_list = folder_stack.last();
            current_list->append({
                .id = generate_random_uuid(),
                .data = BookmarkItem::Bookmark {
                    .url = url.release_value(),
                    .title = move(title),
                    .favicon_base64_png = move(favicon_base64),
                },
            });
            continue;
        }
    }

    return root_items;
}

}
