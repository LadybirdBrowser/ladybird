/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <LibRequests/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API FileDownloader {
public:
    FileDownloader();
    ~FileDownloader();

    void download_file(URL::URL const&, LexicalPath);

private:
    HashMap<u64, NonnullRefPtr<Requests::Request>> m_requests;
};

}
