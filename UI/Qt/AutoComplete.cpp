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

ErrorOr<Vector<String>> AutoComplete::parse_google_autocomplete(JsonValue const& json)
{
    if (!json.is_array())
        return Error::from_string_literal("Expected Google autocomplete response to be a JSON array");

    auto const& values = json.as_array();

    if (values.size() != 5)
        return Error::from_string_literal("Invalid Google autocomplete response, expected 5 elements in array");

    if (!values[0].is_string())
        return Error::from_string_literal("Invalid Google autocomplete response, expected first element to be a string");

    auto const& query = values[0].as_string();
    if (query != m_query)
        return Error::from_string_literal("Invalid Google autocomplete response, query does not match");

    if (!values[1].is_array())
        return Error::from_string_literal("Invalid Google autocomplete response, expected second element to be an array");
    auto const& suggestions_array = values[1].as_array().values();

    Vector<String> results;
    results.ensure_capacity(suggestions_array.size());
    for (auto const& suggestion : suggestions_array)
        results.unchecked_append(suggestion.as_string());

    return results;
}

ErrorOr<Vector<String>> AutoComplete::parse_duckduckgo_autocomplete(JsonValue const& json)
{
    if (!json.is_array())
        return Error::from_string_literal("Expected DuckDuckGo autocomplete response to be a JSON array");

    Vector<String> results;
    results.ensure_capacity(json.as_array().size());

    for (auto const& suggestion : json.as_array().values()) {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid DuckDuckGo autocomplete response, expected value to be an object");

        if (auto value = suggestion.as_object().get_string("phrase"sv); value.has_value())
            results.unchecked_append(*value);
    }

    return results;
}

ErrorOr<Vector<String>> AutoComplete::parse_yahoo_autocomplete(JsonValue const& json)
{
    if (!json.is_object())
        return Error::from_string_literal("Expected Yahoo autocomplete response to be a JSON array");

    auto query = json.as_object().get_string("q"sv);
    if (!query.has_value())
        return Error::from_string_literal("Invalid Yahoo autocomplete response, expected \"q\" to be a string");
    if (query != m_query)
        return Error::from_string_literal("Invalid Yahoo autocomplete response, query does not match");

    auto suggestions = json.as_object().get_array("r"sv);
    if (!suggestions.has_value())
        return Error::from_string_literal("Invalid Yahoo autocomplete response, expected \"r\" to be an object");

    Vector<String> results;
    results.ensure_capacity(suggestions->size());

    for (auto const& suggestion : suggestions->values()) {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid Yahoo autocomplete response, expected value to be an object");

        auto result = suggestion.as_object().get_string("k"sv);
        if (!result.has_value())
            return Error::from_string_literal("Invalid Yahoo autocomplete response, expected \"k\" to be a string");

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

    auto const& engine_name = Settings::the()->autocomplete_engine().name;

    Vector<String> results;
    if (engine_name == "Google")
        results = TRY(parse_google_autocomplete(json));
    else if (engine_name == "DuckDuckGo")
        results = TRY(parse_duckduckgo_autocomplete(json));
    else if (engine_name == "Yahoo")
        results = TRY(parse_yahoo_autocomplete(json));
    else
        return Error::from_string_literal("Invalid engine name");

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
