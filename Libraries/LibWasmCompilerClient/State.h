/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibIPC/Forward.h>
#include <LibSync/Mutex.h>
#include <LibWasmCompilerClient/Forward.h>

namespace WasmCompilerClient {

class CompilerState {
public:
    void install_compiler_callback();
    void replace_connection(IPC::TransportHandle);

private:
    RefPtr<ThreadedClient> m_client;
    Sync::Mutex m_mutex;
};

CompilerState& compiler_state();

}
