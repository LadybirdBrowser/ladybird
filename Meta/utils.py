# Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import signal
import subprocess
import sys

from typing import Optional
from typing import Union


def run_command(
    command: list[str],
    input: Union[str, None] = None,
    return_output: bool = False,
    exit_on_failure: bool = False,
) -> Optional[str]:
    stdin = subprocess.PIPE if type(input) is str else None
    stdout = subprocess.PIPE if return_output else None

    try:
        # FIXME: For Windows, set the working directory so DLLs are found.
        with subprocess.Popen(command, stdin=stdin, stdout=stdout, text=True) as process:
            (output, _) = process.communicate(input=input)

            if process.returncode != 0:
                if exit_on_failure:
                    sys.exit(process.returncode)
                return None

    except KeyboardInterrupt:
        process.send_signal(signal.SIGINT)
        process.wait()

        sys.exit(process.returncode)

    if return_output:
        return output.strip()

    return None
