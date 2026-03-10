# Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import os
import signal
import subprocess
import sys
import time

from pathlib import Path
from typing import Optional
from typing import Union


def run_command(
    command: list[str],
    input: Union[str, None] = None,
    return_output: bool = False,
    exit_on_failure: bool = False,
    cwd: Union[Path, None] = None,
) -> Optional[str]:
    stdin = subprocess.PIPE if type(input) is str else None
    stdout = subprocess.PIPE if return_output else None

    def terminate_process(process: subprocess.Popen, sig: signal.Signals) -> None:
        if process.poll() is not None:
            return

        try:
            if os.name != "nt":
                os.killpg(process.pid, sig)
            else:
                process.send_signal(sig)
        except ProcessLookupError:
            return

    def wait_for_exit(process: subprocess.Popen, timeout_seconds: float) -> None:
        end_time = time.time() + timeout_seconds
        while time.time() < end_time:
            if process.poll() is not None:
                return
            time.sleep(0.05)

    popen_kwargs = {
        "stdin": stdin,
        "stdout": stdout,
        "text": True,
        "cwd": cwd,
    }

    # Put the subprocess in its own process group so we can reliably tear down
    # the entire tree (e.g. WebDriver -> Ladybird/WebContent/AudioServer) on Ctrl-C.
    if os.name != "nt":
        popen_kwargs["start_new_session"] = True

    try:
        # FIXME: For Windows, set the working directory so DLLs are found.
        with subprocess.Popen(command, **popen_kwargs) as process:
            (output, _) = process.communicate(input=input)

            if process.returncode != 0:
                if exit_on_failure:
                    sys.exit(process.returncode)
                return None

    except KeyboardInterrupt:
        # Best-effort: terminate the whole spawned process tree.
        terminate_process(process, signal.SIGINT)
        wait_for_exit(process, 2.0)

        if process.poll() is None:
            terminate_process(process, signal.SIGTERM)
            wait_for_exit(process, 2.0)

        if process.poll() is None and os.name != "nt":
            terminate_process(process, signal.SIGKILL)
            wait_for_exit(process, 1.0)

        try:
            process.wait(timeout=1.0)
        except Exception:
            pass

        sys.exit(process.returncode or 130)

    if return_output:
        return output.strip()

    return None
