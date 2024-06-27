/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "StringUtils.h"
#include <AK/Forward.h>
#include <AK/String.h>
#include <QCompleter>
#include <QNetworkReply>
#include <QTreeView>

namespace Ladybird {

class AutoCompleteModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AutoCompleteModel(QObject* parent)
        : QAbstractListModel(parent)
    {
    }

    virtual int rowCount(QModelIndex const& parent = QModelIndex()) const override { return parent.isValid() ? 0 : m_suggestions.size(); }
    virtual QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override
    {
        if (role == Qt::DisplayRole || role == Qt::EditRole)
            return qstring_from_ak_string(m_suggestions[index.row()]);
        return {};
    }

    void add(String const& result)
    {
        beginInsertRows({}, m_suggestions.size(), m_suggestions.size());
        m_suggestions.append(result);
        endInsertRows();
    }

    void clear()
    {
        beginResetModel();
        m_suggestions.clear();
        endResetModel();
    }

    void replace_suggestions(Vector<String> suggestions)
    {
        beginInsertRows({}, m_suggestions.size(), m_suggestions.size());
        m_suggestions = suggestions;
        endInsertRows();
    }

private:
    AK::Vector<String> m_suggestions;
};

class AutoComplete final : public QCompleter {
    Q_OBJECT

public:
    AutoComplete(QWidget* parent);

    virtual QString pathFromIndex(QModelIndex const& index) const override
    {
        return index.data(Qt::DisplayRole).toString();
    }

    void get_search_suggestions(String);
    void clear_suggestions();

signals:
    void activated(QModelIndex const&);

private:
    static String auto_complete_url_from_query(StringView query);

    ErrorOr<void> got_network_response(QNetworkReply* reply);

    ErrorOr<Vector<String>> parse_google_autocomplete(Vector<JsonValue> const&);
    ErrorOr<Vector<String>> parse_duckduckgo_autocomplete(Vector<JsonValue> const&);
    ErrorOr<Vector<String>> parse_yahoo_autocomplete(JsonObject const&);

    QNetworkAccessManager* m_manager;
    AutoCompleteModel* m_auto_complete_model;
    QTreeView* m_tree_view;
    QNetworkReply* m_reply { nullptr };

    String m_query;
};

}
