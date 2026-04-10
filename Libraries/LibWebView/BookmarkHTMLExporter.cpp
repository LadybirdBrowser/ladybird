/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibWebView/BookmarkHTMLExporter.h>

namespace WebView {

static void escape_and_append(StringBuilder& builder, StringView text)
{
    for (auto ch : text) {
        switch (ch) {
        case '&':
            builder.append("&amp;"sv);
            break;
        case '<':
            builder.append("&lt;"sv);
            break;
        case '>':
            builder.append("&gt;"sv);
            break;
        case '"':
            builder.append("&quot;"sv);
            break;
        default:
            builder.append(ch);
            break;
        }
    }
}

static void append_indent(StringBuilder& builder, size_t depth)
{
    for (size_t i = 0; i < depth; ++i)
        builder.append("    "sv);
}

static void export_items(StringBuilder& builder, ReadonlySpan<BookmarkItem> items, size_t depth)
{
    append_indent(builder, depth);
    builder.append("<DL><p>\n"sv);

    for (auto const& item : items) {
        item.data.visit(
            [&](BookmarkItem::Bookmark const& bookmark) {
                append_indent(builder, depth + 1);
                builder.append("<DT><A HREF=\""sv);
                escape_and_append(builder, bookmark.url.serialize());
                builder.append('"');

                // FIXME: ADD_DATE we don't track creation dates yet
                builder.append(" ADD_DATE=\"0\""sv);

                if (bookmark.favicon_base64_png.has_value() && !bookmark.favicon_base64_png->is_empty()) {
                    builder.append(" ICON=\"data:image/png;base64,"sv);
                    builder.append(*bookmark.favicon_base64_png);
                    builder.append('"');
                }

                builder.append('>');
                if (bookmark.title.has_value())
                    escape_and_append(builder, *bookmark.title);
                else
                    escape_and_append(builder, bookmark.url.serialize());
                builder.append("</A>\n"sv);
            },
            [&](BookmarkItem::Folder const& folder) {
                append_indent(builder, depth + 1);
                builder.append("<DT><H3 ADD_DATE=\"0\" LAST_MODIFIED=\"0\">"sv);
                if (folder.title.has_value())
                    escape_and_append(builder, *folder.title);
                else
                    builder.append("Untitled Folder"sv);
                builder.append("</H3>\n"sv);

                export_items(builder, folder.children, depth + 1);
            });
    }

    append_indent(builder, depth);
    builder.append("</DL><p>\n"sv);
}

ErrorOr<String> export_bookmarks_to_html(ReadonlySpan<BookmarkItem> items)
{
    StringBuilder builder;

    builder.append("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"sv);
    builder.append("<!-- This is an automatically generated file.\n"sv);
    builder.append("     It will be read and overwritten.\n"sv);
    builder.append("     DO NOT EDIT! -->\n"sv);
    builder.append("<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"sv);
    builder.append("<TITLE>Bookmarks</TITLE>\n"sv);
    builder.append("<H1>Bookmarks</H1>\n"sv);

    export_items(builder, items, 0);

    return builder.to_string();
}

}
