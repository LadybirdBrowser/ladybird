// Compiled code allocating an object literal must store the last of several initializations of the same property:
// `{ a: 1, a: 2, a: 3 }` has `a === 3`, and `{ a: x, a: 3 }` has `a === 3`.

test("the last initialization of a duplicate key wins in compiled code", () => {
    const constants = new Function("return { a: 1, a: 2, a: 3 };");
    const mixed = new Function("x", "return { a: x, a: 3 };");
    const mixedReversed = new Function("x", "return { a: 3, a: x };");
    for (const f of [constants, mixed, mixedReversed]) jit.prepare(f);
    for (let i = 0; i < 5; ++i) {
        constants();
        mixed(i);
        mixedReversed(i);
    }
    for (const f of [constants, mixed, mixedReversed]) jit.compile(f);
    for (let i = 0; i < 5; ++i) {
        expect(constants().a).toBe(3);
        expect(mixed(i).a).toBe(3);
        expect(mixedReversed(i).a).toBe(i);
        expect(Object.keys(mixed(i))).toEqual(["a"]);
    }
});
