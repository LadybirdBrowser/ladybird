/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/URL.h>
#include <UI/Qt/AutoComplete.h>
#include <UI/Qt/Settings.h>

namespace Ladybird {

AutoComplete::AutoComplete(QWidget* parent)
    : QCompleter(parent)
{
    m_tree_view = new QTreeView(parent);
    m_manager = new QNetworkAccessManager(this);
    m_auto_complete_model = new AutoCompleteModel(this);

    setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    setModel(m_auto_complete_model);
    setPopup(m_tree_view);

    m_tree_view->setRootIsDecorated(false);
    m_tree_view->setHeaderHidden(true);

    connect(this, QOverload<QModelIndex const&>::of(&QCompleter::activated), this, [&](QModelIndex const& index) {
        emit activated(index);
    });

    connect(m_manager, &QNetworkAccessManager::finished, this, [&](QNetworkReply* reply) {
        auto result = got_network_response(reply);
        if (result.is_error())
            dbgln("AutoComplete::got_network_response: Error {}", result.error());
    });
}

ErrorOr<Vector<String>> AutoComplete::parse_google_autocomplete(Vector<JsonValue> const& json)
{
    if (json.size() != 5)
        return Error::from_string_literal("Invalid JSON, expected 5 elements in array");

    if (!json[0].is_string())
        return Error::from_string_literal("Invalid JSON, expected first element to be a string");
    auto query = json[0].as_string();

    if (!json[1].is_array())
        return Error::from_string_literal("Invalid JSON, expected second element to be an array");
    auto suggestions_array = json[1].as_array().values();

    if (query != m_query)
        return Error::from_string_literal("Invalid JSON, query does not match");

    Vector<String> results;
    results.ensure_capacity(suggestions_array.size());
    for (auto& suggestion : suggestions_array)
        results.unchecked_append(suggestion.as_string());

    return results;
}

ErrorOr<Vector<String>> AutoComplete::parse_duckduckgo_autocomplete(Vector<JsonValue> const& json)
{
    Vector<String> results;

    for (auto const& suggestion : json) {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid JSON, expected value to be an object");

        if (auto value = suggestion.as_object().get_string("phrase"sv); !value.has_value())
            results.append(*value);
    }

    return results;
}

ErrorOr<Vector<String>> AutoComplete::parse_yahoo_autocomplete(JsonObject const& json)
{
    auto query = json.get_string("q"sv);
    if (!query.has_value())
        return Error::from_string_view("Invalid JSON, expected \"q\" to be a string"sv);
    if (query != m_query)
        return Error::from_string_literal("Invalid JSON, query does not match");

    auto suggestions = json.get_array("r"sv);
    if (!suggestions.has_value())
        return Error::from_string_view("Invalid JSON, expected \"r\" to be an object"sv);

    Vector<String> results;
    results.ensure_capacity(suggestions->size());

    for (auto const& suggestion : suggestions->values()) {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid JSON, expected value to be an object");

        auto result = suggestion.as_object().get_string("k"sv);
        if (!result.has_value())
            return Error::from_string_view("Invalid JSON, expected \"k\" to be a string"sv);

        results.unchecked_append(*result);
    }

    return results;
}

ErrorOr<void> AutoComplete::got_network_response(QNetworkReply* reply)
{
    if (reply->error() == QNetworkReply::NetworkError::OperationCanceledError)
        return {};

    auto reply_data = ak_string_from_qstring(reply->readAll());
    auto json = TRY(JsonValue::from_string(reply_data));

    auto engine_name = Settings::the()->autocomplete_engine().name;
    Vector<String> results;
    if (engine_name == "Google") {
        results = TRY(parse_google_autocomplete(json.as_array().values()));
    } else if (engine_name == "DuckDuckGo") {
        results = TRY(parse_duckduckgo_autocomplete(json.as_array().values()));
    } else if (engine_name == "Yahoo")
        results = TRY(parse_yahoo_autocomplete(json.as_object()));
    else {
        return Error::from_string_literal("Invalid engine name");
    }

    constexpr size_t MAX_AUTOCOMPLETE_RESULTS = 6;
    if (results.is_empty()) {
        results.append(m_query);
    } else if (results.size() > MAX_AUTOCOMPLETE_RESULTS) {
        results.shrink(MAX_AUTOCOMPLETE_RESULTS);
    }

    m_auto_complete_model->replace_suggestions(move(results));
    return {};
}

String AutoComplete::auto_complete_url_from_query(StringView query)
{
    auto autocomplete_engine = ak_string_from_qstring(Settings::the()->autocomplete_engine().url);
    return MUST(autocomplete_engine.replace("{}"sv, URL::percent_encode(query), ReplaceMode::FirstOnly));
}

void AutoComplete::clear_suggestions()
{
    m_auto_complete_model->clear();
}

void AutoComplete::get_search_suggestions(String search_string)
{
    m_query = move(search_string);
    if (m_reply)
        m_reply->abort();

    QNetworkRequest request { QUrl(qstring_from_ak_string(auto_complete_url_from_query(m_query))) };
    m_reply = m_manager->get(request);
}

}
