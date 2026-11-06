test("the update of a for loop leaves nothing behind for the next function", () => {
    // NB: Which rows of the two functions line up depends on the padding.
    for (let padding = 0; padding < 32; ++padding) {
        const first = new Function("let a = 1; for (; false; a--); return a;");
        const second = new Function(`let a = 1; return [${"0, ".repeat(padding)}a--];`);
        expect(first()).toBe(1);
        expect(second().at(-1)).toBe(1);
    }
});
