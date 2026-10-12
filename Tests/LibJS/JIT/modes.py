#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""The JIT stress modes the test tools run code under, as LIBJS_JIT values.

Every mode is compared with the interpreter (LIBJS_JIT=off): whatever runs in a mode must behave exactly like it does
in the interpreter. Modes with "sync" compile at the same points in every run, so their failures reproduce with the
same command. "{seed}" in a mode is replaced by the run's seed.
"""

from dataclasses import dataclass
from typing import List


@dataclass(frozen=True)
class Mode:
    name: str
    jit: str
    description: str

    def environment_value(self, seed: int) -> str:
        return self.jit.replace("{seed}", str(seed))


INTERPRETER = Mode("interpreter", "off", "The interpreter alone: the reference every other mode is compared with.")

# NB: "warmup=0,threshold=2" profiles every function from its first call and compiles it after about two more, so that
#     even code that runs a handful of times (like a test repeated a few times) gets compiled with real feedback.
WARM = "on,warmup=0,threshold=2,sync"

MODES = [
    Mode(
        "eager", "on,warmup=0,threshold=0,sync,verify-ir", "Compile every function at its first call, without feedback."
    ),
    Mode("warm", f"{WARM},verify-ir", "Compile every function after a few calls, from the feedback they collected."),
    Mode("exits", f"{WARM},stress-exits=5,verify-ir", "Like warm, and exit from compiled code at every few checks."),
    Mode("exits-always", f"{WARM},stress-exits=1", "Like warm, and exit from compiled code at every check."),
    Mode("osr", f"{WARM},stress-osr,verify-ir", "Like warm, with an on-stack replacement entry at every loop."),
    Mode(
        "invalidate",
        f"{WARM},stress-invalidate=5,verify-ir",
        "Like warm, and invalidate code as if what it depends on stopped holding, every few tier-up checks.",
    ),
    Mode("registers", f"{WARM},stress-registers,verify-ir", "Like warm, with very few registers for values."),
    Mode("no-inline", f"{WARM},inline-depth=0", "Like warm, without inlining."),
    Mode(
        "deep-inline",
        f"{WARM},inline-depth=10,inline-max-size=2000,inline-budget=100000,verify-ir",
        "Like warm, inlining every call it can, as deep as it can.",
    ),
    Mode(
        "random",
        "on,warmup=2,threshold=10,random-thresholds,sync,stress-exits=50,seed={seed}",
        "Compile each function after a random number of calls, and exit at random checks.",
    ),
    Mode(
        "async",
        "on,warmup=1,threshold=4,stress-install,seed={seed}",
        "Compile on the compile threads, installing the code after a random number of tier-up checks.",
    ),
    Mode("default", "on", "The JIT with its default options, as the browser runs it."),
]

MODES_BY_NAME = {mode.name: mode for mode in [INTERPRETER] + MODES}

TIERS = {
    # Every commit: minutes.
    "fast": ["eager", "warm", "exits"],
    # Nightly: every mode.
    "full": [mode.name for mode in MODES],
}


def modes_for(names: List[str]) -> List[Mode]:
    """The modes of a list of mode and tier names."""
    modes = []
    for name in names:
        for mode_name in TIERS.get(name, [name]):
            if mode_name not in MODES_BY_NAME:
                raise ValueError(f"Unknown mode or tier '{mode_name}'")
            mode = MODES_BY_NAME[mode_name]
            if mode not in modes:
                modes.append(mode)
    return modes
