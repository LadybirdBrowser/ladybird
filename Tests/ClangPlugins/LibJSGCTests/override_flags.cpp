/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibGC/Cell.h>

// Class that overrides finalize without the flag - ERROR
class MissingFinalizeFlag : public GC::Cell {
    GC_CELL(MissingFinalizeFlag, GC::Cell);

    // expected-error@+1 {{Class MissingFinalizeFlag overrides finalize but does not set static constexpr bool OVERRIDES_FINALIZE = true}}
    virtual void finalize() override { Base::finalize(); }
};

// Class that correctly sets the finalize flag - OK
class CorrectFinalizeFlag : public GC::Cell {
    GC_CELL(CorrectFinalizeFlag, GC::Cell);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual void finalize() override { Base::finalize(); }
};

// Class that sets the flag to false - ERROR (flag must be true)
class FlagSetToFalse : public GC::Cell {
    GC_CELL(FlagSetToFalse, GC::Cell);

public:
    static constexpr bool OVERRIDES_FINALIZE = false;

    // expected-error@+1 {{Class FlagSetToFalse overrides finalize but does not set static constexpr bool OVERRIDES_FINALIZE = true}}
    virtual void finalize() override { Base::finalize(); }
};
