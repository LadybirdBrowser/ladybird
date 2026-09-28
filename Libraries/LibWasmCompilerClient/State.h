/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Mutex.h>
#include <AK/RefPtr.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>
#include <LibWasmCompilerClient/Forward.h>

namespace WasmCompilerClient {

class CompilerState {
public:
    Core::AnonymousBuffer compile(Core::AnonymousBuffer const&);
    void replace_connection(IPC::TransportHandle);

private:
    RefPtr<ThreadedClient> m_client;
    Mutex m_mutex;
};

CompilerState& compiler_state();

}
