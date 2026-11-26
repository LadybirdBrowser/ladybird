/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/Application.h>

#include <QMimeData>
#include <QStringList>
#include <QVariant>

namespace {

// Offers its text under several types, but answers only text/plain, as some password managers do on X11.
class PartlyAnsweredMimeData final : public QMimeData {
public:
    QStringList formats() const override
    {
        return { "text/plain", "text/plain;charset=utf-8", "text/plain;charset=UTF-8", "text/html" };
    }

    bool hasFormat(QString const& mime_type) const override { return formats().contains(mime_type); }

protected:
    QVariant retrieveData(QString const& mime_type, QMetaType) const override
    {
        if (mime_type == "text/plain")
            return QByteArray("secret");
        if (mime_type == "text/html")
            return QByteArray("<b>secret</b>");
        return {};
    }
};

}

TEST_CASE(text_offered_under_types_that_go_unanswered_is_pasted_once_from_the_answered_type)
{
    PartlyAnsweredMimeData mime_data;
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 2u);
    if (representations.size() != 2)
        return;

    EXPECT_EQ(representations[0].name, "text/plain"sv);
    EXPECT_EQ(representations[0].data.get<ByteString>(), "secret"sv);
    EXPECT_EQ(representations[1].name, "text/html"sv);
    EXPECT_EQ(representations[1].data.get<ByteString>(), "<b>secret</b>"sv);
}

TEST_CASE(text_under_a_type_spelled_another_way_is_read_as_offered)
{
    QMimeData mime_data;
    mime_data.setData("text/plain; charset=utf-8", "hello");
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 1u);
    if (representations.size() != 1)
        return;
    EXPECT_EQ(representations[0].name, "text/plain"sv);
    EXPECT_EQ(representations[0].data.get<ByteString>(), "hello"sv);
}

TEST_CASE(text_in_another_charset_is_handed_over_as_utf8)
{
    QMimeData mime_data;
    mime_data.setData("text/plain;charset=ISO-8859-1", "caf\xe9");
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 1u);
    if (representations.size() != 1)
        return;
    EXPECT_EQ(representations[0].name, "text/plain"sv);
    EXPECT_EQ(representations[0].data.get<ByteString>(), "caf\xc3\xa9"sv);
}

TEST_CASE(text_in_a_charset_that_is_not_known_gives_way_to_another_type)
{
    QMimeData mime_data;
    mime_data.setData("text/plain;charset=x-not-a-charset", "garbled");
    mime_data.setData("text/plain;charset=ISO-8859-1", "caf\xe9");
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 1u);
    if (representations.size() != 1)
        return;
    EXPECT_EQ(representations[0].data.get<ByteString>(), "caf\xc3\xa9"sv);
}

TEST_CASE(a_utf8_type_is_read_before_a_type_in_another_charset)
{
    QMimeData mime_data;
    mime_data.setData("text/plain;charset=UTF-16LE", QByteArray("A\0B", 3));
    mime_data.setData("text/plain;charset=utf-8", "AB");
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 1u);
    if (representations.size() != 1)
        return;
    EXPECT_EQ(representations[0].data.get<ByteString>(), "AB"sv);
}

TEST_CASE(text_cut_off_partway_through_a_character_gives_way_to_another_type)
{
    QMimeData mime_data;
    mime_data.setData("text/plain;charset=UTF-16LE", QByteArray("A\0B", 3));
    mime_data.setData("text/plain;charset=ISO-8859-1", "AB");
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 1u);
    if (representations.size() != 1)
        return;
    EXPECT_EQ(representations[0].data.get<ByteString>(), "AB"sv);
}

TEST_CASE(complete_utf16_text_is_handed_over_as_utf8)
{
    QMimeData mime_data;
    mime_data.setData("text/plain;charset=UTF-16LE", QByteArray("A\0B\0", 4));
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 1u);
    if (representations.size() != 1)
        return;
    EXPECT_EQ(representations[0].data.get<ByteString>(), "AB"sv);
}

TEST_CASE(empty_text_is_handed_over_once)
{
    QMimeData mime_data;
    mime_data.setData("text/plain", {});
    mime_data.setData("text/plain;charset=utf-8", {});
    auto item = Ladybird::clipboard_item_from_mime_data(mime_data);

    auto const& representations = item.system_clipboard_representations;
    EXPECT_EQ(representations.size(), 1u);
    if (representations.size() != 1)
        return;
    EXPECT_EQ(representations[0].name, "text/plain"sv);
    EXPECT(representations[0].data.get<ByteString>().is_empty());
}
