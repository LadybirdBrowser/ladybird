/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/Random.h>
#include <LibWebCommon/WebDriver/BiDi.h>
#include <LibWebView/WebDriverBiDiNetwork.h>

namespace WebView {

using Web::WebDriver::as_js_uint;
using Web::WebDriver::Error;
using Web::WebDriver::ErrorCode;
using Web::WebDriver::Network::create_a_headers_list;
using Web::WebDriver::Network::UrlPattern;
using Web::WebDriver::Network::validate_interception_parameters;

static Error invalid_argument(StringView message)
{
    return Error::from_code(ErrorCode::InvalidArgument, message);
}

static ErrorOr<String, Error> get_string(JsonObject const& params, StringView name)
{
    auto value = params.get(name);
    if (!value.has_value() || !value->is_string())
        return invalid_argument(MUST(String::formatted("Parameter '{}' must be a string", name)));
    return value->as_string();
}

static ErrorOr<Vector<String>, Error> get_string_list(JsonObject const& params, StringView name)
{
    auto value = params.get(name);
    if (!value.has_value() || !value->is_array() || value->as_array().is_empty())
        return invalid_argument(MUST(String::formatted("Parameter '{}' must be a non-empty list of strings", name)));
    Vector<String> result;
    HashTable<String> seen;
    for (auto const& entry : value->as_array().values()) {
        if (!entry.is_string())
            return invalid_argument(MUST(String::formatted("Parameter '{}' must contain strings", name)));
        if (seen.set(entry.as_string()) == HashSetResult::InsertedNewEntry)
            result.append(entry.as_string());
    }
    return result;
}

static ErrorOr<Optional<Vector<String>>, Error> get_optional_string_list(JsonObject const& params, StringView name)
{
    if (!params.has(name))
        return Optional<Vector<String>> {};
    return TRY(get_string_list(params, name));
}

static ErrorOr<void, Error> validate_user_contexts(Vector<String> const& ids)
{
    // https://w3c.github.io/webdriver-bidi/#get-user-context
    // FIXME: Support user contexts other than the default user context.
    for (auto const& id : ids) {
        if (id != "default"sv)
            return Error::from_code(ErrorCode::NoSuchUserContext, "User context not found"sv);
    }
    return {};
}

ErrorOr<JsonValue, Error> WebDriverBiDiNetwork::run_command(StringView method, JsonObject const& params, ValidateContext const& validate_context)
{
    if (method == "network.addIntercept"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-addIntercept
        auto phases = TRY(get_string_list(params, "phases"sv));
        Intercept entry;
        for (auto const& phase : phases) {
            if (!phase.is_one_of("beforeRequestSent"sv, "responseStarted"sv, "authRequired"sv))
                return invalid_argument("Unknown interception phase"sv);
            entry.phases.set(phase);
        }
        auto patterns = params.get("urlPatterns"sv);
        if (patterns.has_value() && !patterns->is_array())
            return invalid_argument("URL patterns must be a list"sv);
        entry.browsing_contexts = TRY(get_optional_string_list(params, "contexts"sv));
        // 1. Let intercept be the string representation of a UUID.
        auto intercept = generate_random_uuid();
        // 2. Let url patterns be the urlPatterns field of command parameters if present, or an empty list otherwise.
        // 3. Let navigables be null.
        // 4. If the contexts field of command parameters is present:
        if (entry.browsing_contexts.has_value()) {
            // 1. Set navigables to an empty set.
            // 2. For each navigable id of command parameters["contexts"]
            for (auto const& id : *entry.browsing_contexts) {
                // 1. Let navigable be the result of trying to get a navigable with navigable id.
                // 2. If navigable is not a top-level traversable, return error with error code invalid argument.
                // 3. Append navigable to navigables.
                TRY(validate_context(id));
            }
            // 3. If navigables is an empty set, return error with error code invalid argument.
            // NB: get_string_list() already rejected an empty list of contexts.
        }
        // 5. Let intercept map be session's intercept map.
        // 6. Let parsed patterns be an empty list.
        // 7. For each url pattern in url patterns:
        if (patterns.has_value()) {
            for (auto const& pattern : patterns->as_array().values()) {
                // 1. Let parsed be the result of trying to parse url pattern with url pattern.
                // 2. Append parsed to parsed patterns.
                entry.url_patterns.append(TRY(UrlPattern::parse(pattern)));
            }
        }
        // 8. Set intercept map[intercept] to a struct with url patterns parsed patterns, phases command parameters["phases"] and browsingContexts navigables.
        m_intercept_map.set(intercept, move(entry));
        // 9. Return a new map matching the network.AddInterceptResult production with the intercept field set to intercept.
        JsonObject result;
        result.set("intercept"sv, move(intercept));
        return JsonValue { move(result) };
    }
    if (method == "network.removeIntercept"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-removeIntercept
        // 1. Let intercept be the value of the "intercept" field in command parameters.
        auto intercept = TRY(get_string(params, "intercept"sv));
        // 2. Let intercept map be session's intercept map.
        // 3. If intercept map does not contain intercept, return error with error code no such intercept.
        // 4. Remove intercept from intercept map.
        if (!m_intercept_map.remove(intercept))
            return Error::from_code(ErrorCode::NoSuchIntercept, "Intercept not found"sv);
        // 5. Return success with data null.
        return JsonValue {};
    }
    if (method == "network.addDataCollector"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-addDataCollector
        auto data_types = TRY(get_string_list(params, "dataTypes"sv));
        for (auto const& type : data_types) {
            if (!type.is_one_of("request"sv, "response"sv))
                return invalid_argument("Unknown network data type"sv);
        }
        auto size_value = params.get("maxEncodedDataSize"sv);
        auto size = size_value.has_value() ? as_js_uint(*size_value) : Optional<u64> {};
        if (!size.has_value())
            return invalid_argument("maxEncodedDataSize must be a js-uint"sv);
        auto collector_type = params.has("collectorType"sv) ? TRY(get_string(params, "collectorType"sv)) : "blob"_string;
        if (collector_type != "blob"sv)
            return invalid_argument("Unknown collector type"sv);
        auto contexts = TRY(get_optional_string_list(params, "contexts"sv));
        auto user_contexts = TRY(get_optional_string_list(params, "userContexts"sv));
        // 1. Let collector id be the string representation of a UUID.
        auto collector_id = generate_random_uuid();
        // 2. Let input context ids be an empty set.
        // 3. If the contexts field of command parameters is present, set input context ids to create a set with command parameters[contexts].
        // 4. Let data types be create a set with command parameters["dataTypes"].
        // 5. Let max encoded item size be command parameters["maxEncodedDataSize"].
        // 6. Let collector type be command parameters["collectorType"].
        // 7. Let input user context ids be an empty set.
        // 8. If the userContexts field of command parameters is present, set input user context ids to create a set with command parameters[userContexts].
        // 9. If input user context ids is not empty and input context ids is not empty, return error with error code invalid argument.
        if (contexts.has_value() && user_contexts.has_value())
            return invalid_argument("contexts and userContexts are mutually exclusive"sv);
        // 10. If max encoded item size is 0 or max encoded item size is greater than max total collected size, return error with error code invalid argument.
        // NB: The implementation-defined budget for collected data is 200 MB.
        static constexpr u64 MAX_TOTAL_COLLECTED_SIZE = 200000000;
        if (*size == 0 || *size > MAX_TOTAL_COLLECTED_SIZE)
            return invalid_argument("maxEncodedDataSize exceeds the collection budget"sv);
        // 11. If input context ids is not empty:
        if (contexts.has_value()) {
            // 1. Let navigables be the result of trying to get valid navigables by ids with input context ids.
            // 2. For each navigable in navigables:
            //     1. If navigable is not a top-level traversable, return error with error code invalid argument.
            for (auto const& id : *contexts)
                TRY(validate_context(id));
        }
        // 12. Otherwise, if input user context ids is not empty:
        //      1. For each user context id of input user context ids:
        //          1. Let user context be get user context with user context id.
        //          2. If user context is null, return error with error code no such user context.
        else if (user_contexts.has_value()) {
            TRY(validate_user_contexts(*user_contexts));
        }
        // 13. Let collector be a collector with max encoded item size field set to max encoded item size, data types field set to data types, collector field set to collector id, collector type field set to collector type, contexts field set to input context ids, user contexts field set to input user context ids.
        // 14. Set session's network collectors[collector id] to collector.
        m_network_collectors.set(collector_id, Collector { *size, move(data_types), move(collector_type), move(contexts).value_or(Vector<String> {}), move(user_contexts).value_or(Vector<String> {}) });
        // 15. Return a new map matching the network.AddDataCollectorResult production with the collector field set to collector id.
        JsonObject result;
        result.set("collector"sv, move(collector_id));
        return JsonValue { move(result) };
    }
    if (method == "network.removeDataCollector"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-removeDataCollector
        // 1. Let collector id be the value of the "collector" field in command parameters.
        auto collector_id = TRY(get_string(params, "collector"sv));
        // 2. Let collectors be session's network collectors.
        // 3. If collectors does not contain collector id, return error with error code no such network collector.
        // 4. Remove collector id from session's network collectors.
        if (!m_network_collectors.remove(collector_id))
            return Error::from_code(ErrorCode::NoSuchNetworkCollector, "Network collector not found"sv);
        // 5. For collected data in collected network data, remove collector from data with collected data and collector id.
        // FIXME: Release collected bodies once Fetch supports network data collection.
        // 6. Return success with data null.
        return JsonValue {};
    }
    if (method == "network.getData"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-getData
        // 1. Let data type be command parameters["dataType"].
        auto data_type = TRY(get_string(params, "dataType"sv));
        if (!data_type.is_one_of("request"sv, "response"sv))
            return invalid_argument("Unknown network data type"sv);
        // 2. Let request id be command parameters["request"].
        TRY(get_string(params, "request"sv));
        auto disown_value = params.get("disown"sv);
        if (disown_value.has_value() && !disown_value->is_bool())
            return invalid_argument("Parameter 'disown' must be a boolean"sv);
        // 3. Let collector id be null.
        Optional<String> collector_id;
        // 4. If command parameters contains "collector":
        if (params.has("collector"sv)) {
            auto collector = TRY(get_string(params, "collector"sv));
            // 1. Let collectors be session's network collectors.
            // 2. If collectors does not contain collector id, return error with error code no such network collector.
            if (!m_network_collectors.contains(collector))
                return Error::from_code(ErrorCode::NoSuchNetworkCollector, "Network collector not found"sv);
            // 3. Set collector id to command parameters["collector"].
            collector_id = move(collector);
        }
        // 5. Let disown be command parameters["disown"].
        auto disown = disown_value.has_value() && disown_value->as_bool();
        // 6. If disown is true and collector id is null, return error with error code invalid argument.
        if (disown && !collector_id.has_value())
            return invalid_argument("Disowning data requires a collector"sv);
        // 7. Let collected data be get collected data given request id and data type.
        // 8. If collected data is null:
        //     1. Return error with error code no such network data.
        // FIXME: Look up collected data once Fetch supports network data collection.
        return Error::from_code(ErrorCode::NoSuchNetworkData, "Network data not found"sv);
    }
    if (method == "network.disownData"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-disownData
        // 1. Let data type be the value of the "dataType" field in command parameters.
        auto data_type = TRY(get_string(params, "dataType"sv));
        if (!data_type.is_one_of("request"sv, "response"sv))
            return invalid_argument("Unknown network data type"sv);
        // 2. Let collector id be the value of the "collector" field in command parameters.
        auto collector_id = TRY(get_string(params, "collector"sv));
        // 3. Let request id be the value of the "request" field in command parameters.
        TRY(get_string(params, "request"sv));
        // 4. Let collectors be session's network collectors.
        // 5. If collectors does not contain collector id, return error with error code no such network collector.
        if (!m_network_collectors.contains(collector_id))
            return Error::from_code(ErrorCode::NoSuchNetworkCollector, "Network collector not found"sv);
        // 6. Let collected data be get collected data with request id and data type.
        // 7. If collected data is null, return error with error code no such network data.
        // FIXME: Look up collected data once Fetch supports network data collection.
        return Error::from_code(ErrorCode::NoSuchNetworkData, "Network data not found"sv);
    }
    if (method == "network.setExtraHeaders"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-setExtraHeaders
        auto contexts = TRY(get_optional_string_list(params, "contexts"sv));
        auto user_contexts = TRY(get_optional_string_list(params, "userContexts"sv));
        // 1. If command parameters contains "userContexts" and command parameters contains "contexts", return error with error code invalid argument.
        if (contexts.has_value() && user_contexts.has_value())
            return invalid_argument("contexts and userContexts are mutually exclusive"sv);
        // 2. Let headers be the result of trying to create a headers list with command parameters["headers"].
        auto header_value = params.get("headers"sv);
        if (!header_value.has_value())
            return invalid_argument("Missing headers"sv);
        auto headers = TRY(create_a_headers_list(*header_value));
        // 3. If command parameters contains "userContexts":
        if (user_contexts.has_value()) {
            // 1. Let user contexts be an empty list.
            // 2. For user context id in command parameters["userContexts"]:
            //     1. Let user context be get user context with user context id.
            //     2. If user context is null, return error with error code no such user context.
            //     3. Append user context to user contexts.
            TRY(validate_user_contexts(*user_contexts));
            // 3. Let target be session's extra headers' user context headers
            // 4. For user context in user contexts:
            //     1. Set target[user context] to headers.
            for (auto const& id : *user_contexts)
                m_user_context_headers.set(id, headers);
            // 5. Return success with data null.
            return JsonValue {};
        }
        // 4. If command parameters contains "contexts":
        if (contexts.has_value()) {
            // 1. Let navigables be the result of trying to get valid top-level traversables by ids with command parameters["contexts"].
            for (auto const& id : *contexts)
                TRY(validate_context(id));
            // 2. Let target be session's extra headers' navigable headers
            // 3. For navigable in navigables:
            //     1. Set target[navigable] to headers.
            for (auto const& id : *contexts)
                m_navigable_headers.set(id, headers);
            // 4. Return success with data null.
            return JsonValue {};
        }
        // 5. Set session's extra headers' default headers to headers.
        m_default_headers = move(headers);
        // 6. Return success with data null.
        return JsonValue {};
    }
    if (method == "network.setCacheBehavior"sv) {
        // https://w3c.github.io/webdriver-bidi/#command-network-setCacheBehavior
        // 1. Let behavior be command parameters["cacheBehavior"].
        auto behavior = TRY(get_string(params, "cacheBehavior"sv));
        if (!behavior.is_one_of("default"sv, "bypass"sv))
            return invalid_argument("Unknown cache behavior"sv);
        auto contexts = TRY(get_optional_string_list(params, "contexts"sv));
        // 2. If command parameters does not contain "contexts":
        if (!contexts.has_value()) {
            // 1. Set the default cache behavior to behavior.
            m_default_cache_behavior = move(behavior);
            // 2. Clear navigable cache behavior map.
            m_navigable_cache_behavior_map.clear();
            // 3. Switch on the value of behavior:
            //     "bypass"
            //         Perform implementation-defined steps to disable any implementation-specific resource caches.
            //     "default"
            //         Perform implementation-defined steps to enable any implementation-specific resource caches that
            //         are usually enabled in the current remote end configuration.
            // FIXME: Apply the cache behavior to Fetch and implementation-specific resource caches.
            // 4. Return success with data null.
            return JsonValue {};
        }
        // 3. Let navigables be an empty set.
        // 4. For each navigable id of command parameters["contexts"]:
        //     1. Let context be the result of trying to get a navigable with navigable id.
        //     2. If context is not a top-level browsing context, return error with error code invalid argument.
        //     3. Append context to navigables.
        for (auto const& id : *contexts)
            TRY(validate_context(id));
        // 5. For each navigable in navigables:
        for (auto const& id : *contexts) {
            // 1. If navigable cache behavior map contains navigable, and navigable cache behavior map[navigable] is
            //    equal to behavior then continue.
            if (m_navigable_cache_behavior_map.get(id) == behavior)
                continue;
            // 2. Switch on the value of behavior:
            //     "bypass"
            //         Perform implementation-defined steps to disable any implementation-specific resource caches for
            //         network requests originating from any browsing context for which navigable is the top-level
            //         browsing context.
            //     "default"
            //         Perform implementation-defined steps to enable any implementation-specific resource caches that
            //         are usually enabled in the current remote end configuration for network requests originating
            //         from any browsing context for which navigable is the top-level browsing context.
            // FIXME: Apply the cache behavior to Fetch and implementation-specific resource caches.
            // 3. If behavior is equal to default cache behavior:
            if (behavior == m_default_cache_behavior) {
                // 1. If navigable cache behavior map contains navigable, remove navigable cache behavior map[navigable].
                m_navigable_cache_behavior_map.remove(id);
            } else {
                // 4. Otherwise:
                //     1. Set navigable cache behavior map[navigable] to behavior.
                m_navigable_cache_behavior_map.set(id, behavior);
            }
        }
        // 6. Return success with data null.
        return JsonValue {};
    }
    if (method.is_one_of("network.continueRequest"sv, "network.continueResponse"sv, "network.provideResponse"sv, "network.continueWithAuth"sv, "network.failRequest"sv)) {
        // https://w3c.github.io/webdriver-bidi/#command-network-continueRequest
        // https://w3c.github.io/webdriver-bidi/#command-network-continueResponse
        // https://w3c.github.io/webdriver-bidi/#command-network-provideResponse
        // https://w3c.github.io/webdriver-bidi/#command-network-continueWithAuth
        // https://w3c.github.io/webdriver-bidi/#command-network-failRequest
        TRY(validate_interception_parameters(method, params));
        // 1. Let blocked requests be session's blocked request map.
        // 2. Let request id be command parameters["request"].
        // 3. If blocked requests does not contain request id then return error with error code no such request.
        // NB: Nothing adds requests to the blocked request map until Fetch supports interception, so it is always
        //     empty. network.continueResponse and network.provideResponse perform this lookup in update the
        //     response.
        // FIXME: Look up blocked requests once Fetch supports interception.
        return Error::from_code(ErrorCode::NoSuchRequest, "Blocked request not found"sv);
    }
    return Error::from_code(ErrorCode::UnknownCommand, "Command is not supported"sv);
}

void WebDriverBiDiNetwork::context_destroyed(String const& id)
{
    // NB: Navigable headers and the navigable cache behavior map are weak maps keyed by top-level traversables. We key
    //     them by navigable ID, so we remove the entries of a top-level traversable once it is gone.
    m_navigable_headers.remove(id);
    m_navigable_cache_behavior_map.remove(id);
}

}
