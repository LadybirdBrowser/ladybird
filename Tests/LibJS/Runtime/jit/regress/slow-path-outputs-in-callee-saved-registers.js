// A slow path call whose output register is preserved by calls and also holds one of its inputs must leave spilled
// values intact. The null checks fill every register, even with two more on AArch64, and put `x` in one that calls
// preserve. The later slow paths of `aN + 1` keep the values in use, so that `x + 1` reuses the register of `x`. The
// result is joined because the comparison only checks the first 16 elements of an array.

test("slow path calls keep spilled values when their output register is preserved by calls", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const names = Array.from({ length: 26 }, (_, i) => `a${i}`);
            const checks = [...names.slice(0, 18), "x", ...names.slice(18)].map(
                name => `if (${name} === null) return 0;`
            );
            const results = names.map(name => `${name} + 1`);
            return new Function("x", ...names, `${checks.join("\n")} const y = x + 1; return [${results}, y].join();`);
        },
        {
            warmUp: (log, i) => Array(27).fill(i % 3 ? i : { valueOf: () => 5 }),
            probes: { object: () => [{ valueOf: () => 5 }, ...Array.from({ length: 26 }, (_, i) => i + 1)] },
        }
    );
    expect(failures).toEqual([]);
});
