// The jit object that test-js defines, which tests of the JIT use to compile functions where they choose and to check
// what their compiled code did. Without the JIT (LIBJS_JIT=off), nothing is compiled, and the checks of compiled
// code are skipped.
//
// NB: The functions come from new Function, so that each run of a test (see LIBJS_TEST_REPEAT) compiles new ones.

describe("jit testing object", () => {
    test("compiles prepared functions on demand", () => {
        const add = new Function("a", "b", "return a + b;");
        jit.prepare(add);
        for (let i = 0; i < 10; ++i) expect(add(i, 1)).toBe(i + 1);
        expect(jit.isCompiled(add)).toBeFalse();
        expect(jit.compile(add)).toBe(jit.enabled);
        expect(jit.isCompiled(add)).toBe(jit.enabled);
        expect(add(2, 3)).toBe(5);
        expect(jit.exitCount(add)).toBe(0);
    });

    test("counts and records exits", () => {
        const add = new Function("a", "b", "return a + b;");
        jit.prepare(add);
        for (let i = 0; i < 10; ++i) add(i, 1);
        jit.compile(add);
        expect(add(2147483647, 1)).toBe(2147483648);
        expect(add("a", 1)).toBe("a1");
        if (!jit.enabled) {
            expect(jit.exitCount(add)).toBe(0);
            expect(jit.exitSites(add)).toEqual([]);
            return;
        }
        // NB: Exits forced at random checks may come before the ones these calls cause, and are not counted.
        if (jit.forcesExits) {
            expect(jit.exitCount(add)).toBeLessThanOrEqual(2);
            return;
        }
        expect(jit.exitCount(add)).toBe(2);
        const kinds = jit.exitSites(add).map(site => site.split("@")[0]);
        expect(kinds).toContain("Overflow");
        expect(kinds).toContain("NotInt32");
    });

    test("discards code", () => {
        const negate = new Function("a", "return -a;");
        jit.prepare(negate);
        for (let i = 0; i < 10; ++i) negate(i);
        jit.compile(negate);
        jit.discard(negate);
        expect(jit.isCompiled(negate)).toBeFalse();
        expect(jit.discardCount(negate)).toBe(jit.enabled ? 1 : 0);
        expect(negate(3)).toBe(-3);
        expect(jit.compile(negate)).toBe(jit.enabled);
    });

    test("never compiles functions it is told not to", () => {
        const f = new Function("a", "return a * 2;");
        jit.neverCompile(f);
        for (let i = 0; i < 10; ++i) f(i);
        expect(jit.compile(f)).toBeFalse();
        expect(jit.isCompiled(f)).toBeFalse();
    });

    test("tells whether its caller runs in compiled code", () => {
        const probe = new Function("return jit.inCompiledCode();");
        jit.prepare(probe);
        expect(probe()).toBeFalse();
        jit.compile(probe);
        if (!jit.forcesExits) expect(probe()).toBe(jit.enabled);
        expect(jit.inCompiledCode()).toBeFalse();
    });

    test("only takes ECMAScript functions", () => {
        expect(() => jit.compile(Math.max)).toThrow(TypeError);
        expect(() => jit.prepare(1)).toThrow(TypeError);
    });
});
