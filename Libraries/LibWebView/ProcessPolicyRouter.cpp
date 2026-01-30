/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ProcessPolicyRouter.h>

namespace WebView {

ProcessScope ProcessPolicyRouter::default_scope_for(ProcessType type)
{
    switch (type) {
    case ProcessType::Browser:
    case ProcessType::RequestServer:
    case ProcessType::ImageDecoder:
    case ProcessType::AudioServer:
        return ProcessScope::Singleton;
    case ProcessType::WebContent:
        return ProcessScope::PerView;
    case ProcessType::WebWorker:
        return ProcessScope::PerRequest;
    case ProcessType::WebAudioWorker:
        // WebAudioWorker is a helper process used to mint per-client WebAudio
        // IPC sockets. Route it per page (page_id), which is a practical (for now)
        // approximation of scoping it to the requesting agent cluster.
        // Future policy may scope or pool by an agent cluster key (e.g. site).
        return ProcessScope::PerView;
    }
    VERIFY_NOT_REACHED();
}

bool ProcessPolicyRouter::should_maintain_spare_web_content_process(BrowserOptions const& options)
{
    // Disable spare processes when debugging WebContent. Otherwise, it breaks running
    // gdb attach -p $(pidof WebContent).
    if (options.debug_helper_process == ProcessType::WebContent)
        return false;

    // Disable spare processes when profiling WebContent. This reduces callgrind logging
    // we are not interested in.
    if (options.profile_helper_process == ProcessType::WebContent)
        return false;

    return true;
}

Vector<ProcessType> ProcessPolicyRouter::singleton_services_to_launch()
{
    // Keep this order stable: other code assumes these services exist early.
    return {
        ProcessType::RequestServer,
        ProcessType::ImageDecoder,
        ProcessType::AudioServer,
    };
}

}
