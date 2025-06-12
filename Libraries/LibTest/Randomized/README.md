# LibTest/Randomized

Classes in this folder implement a randomized ("property based") testing library
for SerenityOS.

## Example

```cpp
RANDOMIZED_TEST_CASE(addition_commutative)
{
    GEN(int_a, Gen::unsigned_int());
    GEN(int_b, Gen::unsigned_int());
    CSSPixels a(int_a);
    CSSPixels b(int_b);
    EXPECT_EQ(a + b, b + a);
}
```

The runner will then run the test case many times, with random data in each of
the `GEN`'d variables. If it finds any set of values where the expectation fails,
it shrinks it to a minimal failing example and presents it to the user:

```
Running test 'addition_commutative'.
int_a = 0
int_b = 1
FAIL: /.../SomeTest.cpp:26: EXPECT_EQ(a + b, b + a) failed with lhs=0 and rhs=1
Failed test 'addition_commutative' in 0ms
```

(Note that the runner most likely started out with much larger values like
1587197 and 753361. The 0 and 1 are the shrunk, minimal values.)

In the above case, the test runner tells us that `CSSPixels(0) + CSSPixels(1)`
wasn't equal to `CSSPixels(1) + CSSPixels(0)`.

> [!NOTE]
> I made this failing example up. Don't worry, `CSSPixels` is fine :^)

## Property based testing?

Property based testing (PBT) involves generating random test inputs and checking
that they satisfy certain properties. This allows testing a large number of
possible cases automatically, and is exceptionally good at finding edge cases.

When a failure is found, it's automatically shrunk to a minimal reproducing
case. (This is good, because random values contain a lot of unimportant details
that only hamper understanding of the root cause. See http://sscce.org)

This particular implementation belongs to the "internal shrinking" family of PBT
libraries (Hypothesis, minithesis, elm-test, ...). This is important for the DX
as it gives the most resilient shrinkers without any user effort, compared to
the "integrated shrinking" lazy-tree based implementations (Hedgehog etc.) or
the manual shrinking ones (original QuickCheck etc.). (More information in this
talk: https://www.youtube.com/watch?v=WE5bmt0zBxg)

## Implementation

On a very high level, the PBT runner remembers the random bits (`RandomRun`)
used when generating the random value, and when it finds a value that fails the
user's test, instead of trying to shrink the generated value it tries to shrink
these random bits and then generate a new value from them. 

This makes shrinking work automatically on any data you might want to generate.

Generators are implemented in such a way that a smaller/shorter `RandomRun` will
result in a simpler generated value.

If the new RandomRun can be successfully turned into a value and if the test
still fails on that new simpler value, we keep the RandomRun as our new best and
try again. Once we can't improve things anymore, we turn on error reporting, run
the test one last time with the final RandomRun, and report the failing value to
the user.

## Code organization

- `TestResult.h`
  - Defines an enum class TestResult.
    This expands the typical "passed / failed": we also need to care about
    a generator rejecting a RandomRun (eg. when the user calls the ASSUME(...)
    macro with a predicate that can't be satisfied).

- `Generator.h`
  - Contains generators: fns of shape T(), eg. `u32 Gen::unsigned_int(u32 max)`
    - These implicitly depend on a RandomnessSource held by the singleton
      TestSuite.
  - These can be called directly, but the top-level use by the user should always
    happen via the GEN(...) macro which makes sure the generated value gets
    logged to the user in case of a failure.
  - Example:
    `Gen::vector(1, 4, []() { return Gen::unsigned_int(5); })`
    generates vectors of length between 1 and 4, of unsigned ints in range 0..5.
    Eg. `{2,5,3}`, `{0}`, `{1,5,5,2}`.

- `RandomnessSource.h`
  - A source of random bits.
  - There are two variants of `RandomnessSource`:
    - Live: gives `AK/Random` u32 values and remembers them into a `RandomRun`
    - Recorded: gives (replays) u32 values from a static `RandomRun`

- `RandomRun.h`
  - A finite sequence of random bits (in practice, `u32`s).
  - Example: `{2,5,0,11,8,0,0,1}`

- `ShrinkCommand.h`
  - A high-level recipe for how to try and minimize a given `RandomRun`.
  - For example, "zero this contiguous chunk of it" or "minimize the number on
    this index using binary search".
  - These later get interpreted by the PBT runner on a specific `RandomRun`.

- `Chunk.h`
  - A description of a contiguous `RandomRun` slice.
  - Example: `Chunk{size = 4, index = 2}`:  [_,_,X,X,X,X,...]

- `Shrink.h`
  - Algorithms for interpreting `ShrinkCommand`s and the main shrinking loop

- `TestCase.h`
  - The `TestCase::randomized(...)` function contains the main testing loop
