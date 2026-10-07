/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>

#include <QDialog>

class QComboBox;
class QLineEdit;

namespace Ladybird {

class BookmarkDialog final : public QDialog {
public:
    AK_ALLOC_WITH_KMALLOC;

    enum class Type {
        AddBookmark,
        EditBookmark,
        AddFolder,
        EditFolder,
    };

    BookmarkDialog(QWidget* parent, Type, Optional<URL::URL const&> url = {}, Optional<String const&> title = {}, Optional<String const&> selected_folder_id = {}, ReadonlySpan<WebView::BookmarkItem> folders = {});

    QString url() const;
    QString title() const;
    QString selected_folder_id() const;

protected:
    virtual bool event(QEvent*) override;

private:
    void update_chrome_style();

    QLineEdit* m_url_edit { nullptr };
    QLineEdit* m_title_edit { nullptr };
    QComboBox* m_folder_combo { nullptr };
    bool m_is_updating_chrome_style { false };
};

}
