/*
 * Copyright (c) 2023, Martin Janiczek <martin@janiczek.cz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibTest/Randomized/Generator.h>
#include <LibTest/Randomized/RandomRun.h>
#include <LibTest/Randomized/RandomnessSource.h>
#include <LibTest/Randomized/ShrinkCommand.h>
#include <LibTest/TestResult.h>

#include <AK/String.h>

namespace Test {
namespace Randomized {

enum class WasImprovement {
    Yes,
    No,
};

struct ShrinkResult {
    WasImprovement was_improvement;
    RandomRun run;
};

inline ShrinkResult no_improvement(RandomRun run)
{
    return ShrinkResult { WasImprovement::No, run };
}

// When calling keep_if_better we have a new RandomRun that might be better than
// our current_best (which is guaranteed to generate a value and fail the test).
//
// We need to try to generate a value from the new_run and run the test. If the
// generated value fails the test, we say it was an improvement (because of our
// convention for generators that _shorter / smaller RandomRuns lead to simpler
// values_). In all other cases we say it wasn't an improvement.
template<typename Fn>
ShrinkResult keep_if_better(RandomRun const& new_run, RandomRun const& current_best, Fn const& test_function)
{
    if (!new_run.is_shortlex_smaller_than(current_best))
        // The new run is worse or equal to the current best. Let's not even try!
        return no_improvement(current_best);

    set_randomness_source(RandomnessSource::recorded(new_run));
    set_current_test_result(TestResult::NotRun);
    test_function();
    if (current_test_result() == TestResult::NotRun) {
        set_current_test_result(TestResult::Passed);
    }

    switch (current_test_result()) {
    case TestResult::Failed:
        // Our smaller RandomRun resulted in a simpler failing value!
        // Let's keep it.
        return ShrinkResult { WasImprovement::Yes, new_run };
    case TestResult::Passed:
    case TestResult::Rejected:
    case TestResult::Overrun:
        // Passing:  We shrank from a failing value to a passing value.
        // Rejected: We shrank to value that doesn't get past the ASSUME(...)
        //           macro.
        // Overrun:  Generators can't draw enough random bits to generate all
        //           needed values.
        // In all cases: Let's try something else.
        return no_improvement(current_best);
    case TestResult::NotRun:
    default:
        // We've literally just set it to Passed if it was NotRun!
        VERIFY_NOT_REACHED();
        return no_improvement(current_best);
    }
}

template<typename Fn, typename UpdateRunFn>
ShrinkResult binary_shrink(u64 orig_low, u64 orig_high, UpdateRunFn update_run, RandomRun const& orig_run, Fn const& test_function)
{
    if (orig_low == orig_high) {
        return no_improvement(orig_run);
    }

    RandomRun current_best = orig_run;
    u64 low = orig_low;
    u64 high = orig_high;

    // Let's try with the best case (low = most shrunk) first
    RandomRun run_with_low = update_run(low, current_best);
    ShrinkResult after_low = keep_if_better(run_with_low, current_best, test_function);
    switch (after_low.was_improvement) {
    case WasImprovement::Yes:
        // We can't do any better
        return after_low;
    case WasImprovement::No:
        break;
    }

    // Ah well, gotta do some actual work.
    //
    // We're already guaranteed that `high` makes the test fail. We're trying to
    // get as low as `low` (but we know `low` doesn't make the test fail, else
    // the `if` above would succeed and return).
    //
    // Failing value above, passing value below; we try to find the lowest value
    // that still fails.
    //
    // `high` is always guaranteed to fail, `low` is always guaranteed to
    // pass/reject/overrun.
    ShrinkResult result = after_low;
    while (low + 1 < high) {
        u64 mid = low + (high - low) / 2;
        RandomRun run_with_mid = update_run(mid, current_best);
        ShrinkResult after_mid = keep_if_better(run_with_mid, current_best, test_function);
        switch (after_mid.was_improvement) {
        case WasImprovement::Yes:
            high = mid;
            break;
        case WasImprovement::No:
            low = mid;
            break;
        }
        result = after_mid;
        current_best = after_mid.run;
    }

    // did we get below the original `high` at all?
    if (current_best.is_shortlex_smaller_than(orig_run)) {
        result.was_improvement = WasImprovement::Yes;
    } else {
        result.was_improvement = WasImprovement::No;
        result.run = orig_run;
    }
    set_current_test_result(TestResult::Failed);
    return result;
}

template<typename Fn>
ShrinkResult shrink_zero(ZeroChunk command, RandomRun const& run, Fn const& test_function)
{
    RandomRun new_run = run;
    size_t end = command.chunk.index + command.chunk.size;
    for (size_t i = command.chunk.index; i < end; i++) {
        new_run[i] = 0;
    }
    return keep_if_better(new_run, run, test_function);
}

template<typename Fn>
ShrinkResult shrink_sort(SortChunk command, RandomRun const& run, Fn const& test_function)
{
    RandomRun new_run = run.with_sorted(command.chunk);
    return keep_if_better(new_run, run, test_function);
}

template<typename Fn>
ShrinkResult shrink_delete(DeleteChunkAndMaybeDecPrevious command, RandomRun const& run, Fn const& test_function)
{
    RandomRun run_deleted = run.with_deleted(command.chunk);
    // Optional: decrement the previous value. This deals with a non-optimal but
    // relatively common generation pattern: run length encoding.
    //
    // Example: let's say somebody generates lists in this way:
    // * generate a random integer >=0 for the length of the list.
    // * then, generate that many items
    //
    // This results in RandomRuns like this one:
    // [ 3 (length), 50 (item 1), 21 (item 2), 1 (item 3) ]
    //
    // Then if we tried deleting the second item without decrementing
    // the length, it would fail:
    // [ 3 (length), 21 (item 1), 1 (item 2) ] ... runs out of randomness when
    // trying to generate the third item!
    //
    // That's why we try to decrement the number right before the deleted items:
    // [ 2 (length), 21 (item 1), 1 (item 2) ] ... generates fine!
    //
    // Aside: this is why we're generating lists in a different way that plays
    // nicer with shrinking: we flip a coin (generate a bool which is `true` with
    // a certain probability) to see whether to generate another item. This makes
    // items "local" instead of entangled with the non-local length.
    if (run_deleted.size() > command.chunk.index - 1 && run_deleted[command.chunk.index - 1] > 0) {
        RandomRun run_decremented = run_deleted;
        run_decremented[command.chunk.index - 1]--;
        return keep_if_better(run_decremented, run, test_function);
    }

    // Decrementing didn't work; let's try with just the deletion.
    return keep_if_better(run_deleted, run, test_function);
}

template<typename Fn>
ShrinkResult shrink_minimize(MinimizeChoice command, RandomRun const& run, Fn const& test_function)
{
    u64 value = run[command.index];

    // We can't minimize 0! Already the best possible case.
    if (value == 0) {
        return no_improvement(run);
    }

    return binary_shrink(
        0,
        value,
        [&](u64 new_value, RandomRun const& run) {
            RandomRun copied_run = run;
            copied_run[command.index] = new_value;
            return copied_run;
        },
        run,
        test_function);
}

template<typename Fn>
ShrinkResult shrink_swap_chunk(SwapChunkWithNeighbour command, RandomRun const& run, Fn const& test_function)
{
    RandomRun run_swapped = run;
    // The safety of these swaps was guaranteed by has_a_chance() earlier
    for (size_t i = command.chunk.index; i < command.chunk.index + command.chunk.size; ++i) {
        AK::swap(run_swapped[i], run_swapped[i + command.chunk.size]);
    }
    return keep_if_better(run_swapped, run, test_function);
}

template<typename Fn>
ShrinkResult shrink_redistribute(RedistributeChoicesAndMaybeInc command, RandomRun const& run, Fn const& test_function)
{
    RandomRun current_best = run;
    RandomRun run_after_swap = current_best;

    // First try to swap them if they're out of order.
    if (run_after_swap[command.left_index] > run_after_swap[command.right_index])
        AK::swap(run_after_swap[command.left_index], run_after_swap[command.right_index]);

    ShrinkResult after_swap = keep_if_better(run_after_swap, current_best, test_function);
    current_best = after_swap.run;
    u64 constant_sum = current_best[command.right_index] + current_best[command.left_index];

    ShrinkResult after_redistribute = binary_shrink(
        0,
        current_best[command.left_index],
        [&](u64 new_value, RandomRun const& run) {
            RandomRun copied_run = run;
            copied_run[command.left_index] = new_value;
            copied_run[command.right_index] = constant_sum - new_value;
            return copied_run;
        },
        current_best,
        test_function);

    switch (after_redistribute.was_improvement) {
    case WasImprovement::Yes:
        return after_redistribute;
        break;
    case WasImprovement::No:
        break;
    }

    // If the redistribute failed, this can sometimes signal that a value needs
    // to fall into the next `int_frequency` bucket. We can try one last-ditch
    // attempt and see if incrementing the number right before the right index
    // helps.

    if (command.left_index == command.right_index - 1) {
        // There's no "bucket index" between the left and right index.
        // Let's not even try.
        return after_swap;
    }

    RandomRun run_after_increment = after_redistribute.run;
    ++run_after_increment[command.right_index - 1];

    ShrinkResult after_inc_redistribute = binary_shrink(
        0,
        current_best[command.left_index],
        [&](u64 new_value, RandomRun const& run) {
            RandomRun copied_run = run;
            copied_run[command.left_index] = new_value;
            copied_run[command.right_index] = constant_sum - new_value;
            return copied_run;
        },
        current_best,
        test_function);

    switch (after_inc_redistribute.was_improvement) {
    case WasImprovement::Yes:
        return after_inc_redistribute;
        break;
    case WasImprovement::No:
        break;
    }

    return after_swap;
}

template<typename Fn>
ShrinkResult shrink_with_command(ShrinkCommand command, RandomRun const& run, Fn const& test_function)
{
    return command.visit(
        [&](ZeroChunk c) { return shrink_zero(c, run, test_function); },
        [&](SortChunk c) { return shrink_sort(c, run, test_function); },
        [&](DeleteChunkAndMaybeDecPrevious c) { return shrink_delete(c, run, test_function); },
        [&](MinimizeChoice c) { return shrink_minimize(c, run, test_function); },
        [&](RedistributeChoicesAndMaybeInc c) { return shrink_redistribute(c, run, test_function); },
        [&](SwapChunkWithNeighbour c) { return shrink_swap_chunk(c, run, test_function); });
}

template<typename Fn>
RandomRun shrink_once(RandomRun const& run, Fn const& test_function)
{
    RandomRun current = run;

    auto commands = ShrinkCommand::for_run(run);
    for (ShrinkCommand command : commands) {
        // We're keeping the list of ShrinkCommands we generated from the initial
        // RandomRun, as we try to shrink our current best RandomRun.
        //
        // That means some of the ShrinkCommands might have no chance to
        // successfully finish (eg. the command's chunk is out of bounds of the
        // run). That's what we check here and based on what we skip those
        // commands early.
        //
        // In the next `shrink -> shrink_once` loop we'll generate a better set
        // of commands, more tailored to the current best RandomRun.
        if (!command.has_a_chance(current)) {
            continue;
        }
        ShrinkResult result = shrink_with_command(command, current, test_function);
        switch (result.was_improvement) {
        case WasImprovement::Yes:
            current = result.run;
            break;
        case WasImprovement::No:
            break;
        }
    }
    return current;
}

template<typename Fn>
RandomRun shrink(RandomRun const& first_failure, Fn const& test_function)
{
    if (first_failure.is_empty()) {
        // We can't do any better
        return first_failure;
    }

    RandomRun next = first_failure;
    RandomRun current;
    do {
        current = next;
        next = shrink_once(current, test_function);
    } while (next.is_shortlex_smaller_than(current));

    set_current_test_result(TestResult::Failed);

    return next;
}

} // namespace Randomized
} // namespace Test
