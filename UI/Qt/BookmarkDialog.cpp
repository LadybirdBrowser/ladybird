/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/BookmarkStore.h>
#include <UI/Qt/BookmarkDialog.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QLineEdit>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>

namespace Ladybird {

static constexpr int BOOKMARK_FOLDER_SEPARATOR_ROLE = Qt::UserRole + 1;

class BookmarkFolderDelegate final : public QStyledItemDelegate {
public:
    AK_ALLOC_WITH_KMALLOC;

    using QStyledItemDelegate::QStyledItemDelegate;

    virtual QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        if (index.data(BOOKMARK_FOLDER_SEPARATOR_ROLE).toBool())
            return { 0, 11 };
        return QStyledItemDelegate::sizeHint(option, index);
    }

    virtual void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        if (!index.data(BOOKMARK_FOLDER_SEPARATOR_ROLE).toBool()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        painter->fillRect(QRect { option.rect.left() + 8, option.rect.center().y(), option.rect.width() - 16, 1 }, ChromeStyle::chrome_separator(option.palette));
    }
};

class BookmarkFolderPicker final : public QComboBox {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit BookmarkFolderPicker(QWidget* parent)
        : QComboBox(parent)
        , m_arrow(create_chrome_icon(ChromeIcon::ChevronDown, palette()))
    {
        view()->window()->setAttribute(Qt::WA_TranslucentBackground);
        setItemDelegate(new BookmarkFolderDelegate(view()));
    }

protected:
    virtual bool event(QEvent* event) override
    {
        if (event->type() == QEvent::PaletteChange)
            m_arrow = create_chrome_icon(ChromeIcon::ChevronDown, palette());
        return QComboBox::event(event);
    }

    virtual void paintEvent(QPaintEvent* event) override
    {
        QComboBox::paintEvent(event);

        // Styling the drop-down removes the native macOS arrow along with its frame.
        QStyleOptionComboBox option;
        initStyleOption(&option);
        auto arrow_rect = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);

        QPainter painter(this);
        m_arrow.paint(&painter, arrow_rect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
    }

private:
    QIcon m_arrow;
};

static void add_bookmark_folder_options(QComboBox& folder_combo, ReadonlySpan<WebView::BookmarkItem> items, QString const& prefix, Optional<String const&> excluded_folder_id)
{
    for (auto const& item : items) {
        // A folder cannot be moved into itself or any of its descendants.
        if (!item.is_folder() || item.id == excluded_folder_id)
            continue;

        auto title = qstring_from_ak_string(item.folder().title.value_or("(no title)"_string));
        auto path = prefix.isEmpty() ? title : QString("%1 / %2").arg(prefix, title);
        folder_combo.addItem(path, qstring_from_ak_string(item.id));

        add_bookmark_folder_options(folder_combo, item.folder().children, path, excluded_folder_id);
    }
}

BookmarkDialog::BookmarkDialog(QWidget* parent, Type type, Optional<URL::URL const&> current_url, Optional<String const&> current_title, Optional<String const&> selected_folder_id, ReadonlySpan<WebView::BookmarkItem> folders, Optional<String const&> excluded_folder_id)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("LadybirdBookmarkDialog");

    switch (type) {
    case Type::AddBookmark:
        setWindowTitle("Add Bookmark");
        break;
    case Type::EditBookmark:
        setWindowTitle("Edit Bookmark");
        break;
    case Type::AddFolder:
        setWindowTitle("Add Folder");
        break;
    case Type::EditFolder:
        setWindowTitle("Edit Folder");
        break;
    }

    auto* layout = new QFormLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->setHorizontalSpacing(16);
    layout->setVerticalSpacing(12);

    auto create_text_field = [this] {
        auto* field = new QLineEdit(this);
        field->setMinimumWidth(320);
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return field;
    };

    auto is_bookmark = type == Type::AddBookmark || type == Type::EditBookmark;
    if (is_bookmark) {
        m_url_edit = create_text_field();
        if (current_url.has_value())
            m_url_edit->setText(qstring_from_ak_string(current_url->serialize()));
        layout->addRow("URL:", m_url_edit);
    }

    m_title_edit = create_text_field();
    if (current_title.has_value())
        m_title_edit->setText(qstring_from_ak_string(*current_title));
    layout->addRow("Title:", m_title_edit);

    m_folder_combo = new BookmarkFolderPicker(this);
    m_folder_combo->setMinimumWidth(320);
    m_folder_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_folder_combo->addItem("Bookmarks Bar", QString {});
    add_bookmark_folder_options(*m_folder_combo, folders, {}, excluded_folder_id);
    if (m_folder_combo->count() > 1) {
        m_folder_combo->insertSeparator(1);
        m_folder_combo->setItemData(1, true, BOOKMARK_FOLDER_SEPARATOR_ROLE);
    }

    if (selected_folder_id.has_value()) {
        auto index = m_folder_combo->findData(qstring_from_ak_string(*selected_folder_id));
        if (index >= 0)
            m_folder_combo->setCurrentIndex(index);
    }

    layout->addRow("Folder:", m_folder_combo);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);

    update_chrome_style();
    resize(is_bookmark ? 500 : 400, sizeHint().height());
}

QString BookmarkDialog::url() const
{
    return m_url_edit ? m_url_edit->text() : QString {};
}

QString BookmarkDialog::title() const
{
    return m_title_edit->text();
}

QString BookmarkDialog::selected_folder_id() const
{
    return m_folder_combo->currentData().toString();
}

bool BookmarkDialog::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    return QDialog::event(event);
}

void BookmarkDialog::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::bookmark_dialog_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

}
