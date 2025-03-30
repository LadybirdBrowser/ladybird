/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibRequests/Forward.h>

namespace WebView {

struct AutocompleteEngine {
    StringView name;
    StringView query_url;
};

ReadonlySpan<AutocompleteEngine> autocomplete_engines();
Optional<AutocompleteEngine const&> find_autocomplete_engine_by_name(StringView name);

class Autocomplete {
public:
    Autocomplete();
    ~Autocomplete();

    Function<void(Vector<String>)> on_autocomplete_query_complete;

    void query_autocomplete_engine(String);

private:
    static ErrorOr<Vector<String>> received_autocomplete_respsonse(AutocompleteEngine const&, StringView response);
    void invoke_autocomplete_query_complete(Vector<String> suggestions) const;

    String m_query;
    RefPtr<Requests::Request> m_request;
};

}
