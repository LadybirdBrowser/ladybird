/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <UI/Qt/BookmarkDialog.h>
#include <UI/Qt/BookmarkPopover.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/StringUtils.h>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPOVER_WIDTH = 320;

// The folder holding a bookmark, or none for the Bookmarks Bar, and the number of items in it.
struct BookmarkPlace {
    Optional<String> folder_id;
    size_t sibling_count { 0 };
};

static Optional<BookmarkPlace> find_bookmark_place(ReadonlySpan<WebView::BookmarkItem> items, StringView id, Optional<String> folder_id = {})
{
    for (auto const& item : items) {
        if (item.id == id)
            return BookmarkPlace { folder_id, items.size() };
        if (item.is_folder()) {
            if (auto place = find_bookmark_place(item.folder().children, id, item.id); place.has_value())
                return place;
        }
    }
    return {};
}

static size_t item_count_in_folder(ReadonlySpan<WebView::BookmarkItem> items, Optional<String> const& folder_id)
{
    if (!folder_id.has_value())
        return items.size();
    for (auto const& item : items) {
        if (!item.is_folder())
            continue;
        if (item.id == *folder_id)
            return item.folder().children.size();
        if (auto count = item_count_in_folder(item.folder().children, folder_id); count > 0)
            return count;
    }
    return 0;
}

BookmarkPopover::BookmarkPopover(QWidget* parent)
    : Popover(parent)
{
    setAccessibleName("Bookmark");
    card().setObjectName("LadybirdBookmarkPopover");
    card().setFixedWidth(POPOVER_WIDTH);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    m_title_label = new QLabel(&card());
    m_title_label->setObjectName("LadybirdBookmarkPopoverTitle");
    layout->addWidget(m_title_label);

    auto add_field = [&](QString const& label_text, QWidget* field) {
        auto* row = new QWidget(&card());
        auto* row_layout = new QVBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(5);
        auto* label = new QLabel(label_text, row);
        label->setObjectName("LadybirdBookmarkPopoverLabel");
        label->setBuddy(field);
        row_layout->addWidget(label);
        field->setParent(row);
        row_layout->addWidget(field);
        layout->addWidget(row);
        return row;
    };

    m_name_field = new QLineEdit;
    m_name_field->setObjectName("LadybirdBookmarkPopoverName");
    // NB: The style sheet draws the field's focus, so the native focus ring isn't drawn over it.
    m_name_field->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_name_field->setAccessibleName("Name");
    QObject::connect(m_name_field, &QLineEdit::returnPressed, this, [this] { close(); });
    add_field("Name", m_name_field);

    // NB: The picker is made anew each time the popover opens, as the folders may have changed.
    m_folder_row = add_field("Folder", new QWidget);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 2, 0, 0);
    buttons->setSpacing(8);
    auto* remove_button = new QPushButton("Remove", &card());
    remove_button->setObjectName("LadybirdBookmarkPopoverRemove");
    // NB: The platform style lays out a push button by a frame of its own rather than by the button, which would
    //     make it overlap its neighbors.
    remove_button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    QObject::connect(remove_button, &QPushButton::clicked, this, [this] {
        m_is_removed = true;
        Function<void()> remove = [this] { WebView::Application::bookmark_store().remove_item(m_bookmark_id); };
        if (on_remove)
            on_remove(remove);
        else
            remove();
        close();
    });
    buttons->addWidget(remove_button);
    buttons->addStretch(1);
    auto* done_button = new QPushButton("Done", &card());
    done_button->setObjectName("LadybirdBookmarkPopoverDone");
    done_button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    done_button->setDefault(true);
    QObject::connect(done_button, &QPushButton::clicked, this, [this] { close(); });
    buttons->addWidget(done_button);
    layout->addLayout(buttons);
}

void BookmarkPopover::set_bookmark(String id, JustAdded just_added)
{
    m_bookmark_id = AK::move(id);
    m_is_removed = false;
    m_title_label->setText(just_added == JustAdded::Yes ? "Bookmark added" : "Edit bookmark");

    auto& store = WebView::Application::bookmark_store();
    auto item = store.find_item_by_id(m_bookmark_id);
    if (!item.has_value() || !item->is_bookmark())
        return;

    auto const& bookmark = item->bookmark();
    m_name_field->setText(qstring_from_ak_string(bookmark.title.value_or(bookmark.url.serialize())));

    auto place = find_bookmark_place(store.root_items(), m_bookmark_id);
    auto folder_id = place.has_value() ? place->folder_id : Optional<String> {};
    if (m_folder_picker)
        m_folder_picker->deleteLater();
    m_folder_picker = create_bookmark_folder_picker(m_folder_row, store.root_items(), folder_id);
    m_folder_picker->setObjectName("LadybirdBookmarkPopoverFolder");
    // NB: Long folder paths are cut short rather than widening the popover.
    m_folder_picker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_folder_picker->setMinimumContentsLength(1);
    // NB: The platform style pads drop-downs for its own frame, which would shift the label above.
    m_folder_picker->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    m_folder_picker->setAccessibleName("Folder");
    if (auto* row_layout = m_folder_row->layout(); row_layout->count() > 1) {
        auto* item = row_layout->takeAt(1);
        delete item->widget();
        delete item;
    }
    m_folder_row->layout()->addWidget(m_folder_picker);
    adjustSize();
}

void BookmarkPopover::save()
{
    auto& store = WebView::Application::bookmark_store();
    auto item = store.find_item_by_id(m_bookmark_id);
    if (!item.has_value() || !item->is_bookmark())
        return;

    auto url = item->bookmark().url;
    // NB: A bookmark without a title is shown by its URL, which is only kept as its title once it is edited.
    auto title = ak_string_from_qstring(m_name_field->text().trimmed());
    auto shown_title = item->bookmark().title.value_or(url.serialize());
    if (!title.is_empty() && title != shown_title)
        store.edit_bookmark(m_bookmark_id, url, AK::move(title));

    auto place = find_bookmark_place(store.root_items(), m_bookmark_id);
    auto target_folder_text = m_folder_picker ? m_folder_picker->currentData().toString() : QString {};
    auto target_folder_id = target_folder_text.isEmpty() ? Optional<String> {} : Optional<String> { ak_string_from_qstring(target_folder_text) };
    if (place.has_value() && place->folder_id != target_folder_id) {
        // A bookmark moved to another folder goes to the end of it.
        auto index = item_count_in_folder(store.root_items(), target_folder_id);
        store.move_item(m_bookmark_id, target_folder_id, index);
    }
}

void BookmarkPopover::showEvent(QShowEvent* event)
{
    Popover::showEvent(event);
    m_name_field->setFocus();
    m_name_field->selectAll();
}

void BookmarkPopover::hideEvent(QHideEvent* event)
{
    if (!m_is_removed)
        save();
    Popover::hideEvent(event);
}

void BookmarkPopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::bookmark_popover_style_sheet(palette));
}

}
