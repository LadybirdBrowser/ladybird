describe("object destructuring keeps reading from the original value after rebinding its source", () => {
    test("var declaration whose pattern rebinds the source variable", () => {
        function run(source) {
            var { first, second: source, third, fourth = "default" } = source;
            return { first, source, third, fourth };
        }
        const result = run({ first: 1, second: 2, third: 3 });
        expect(result.first).toBe(1);
        expect(result.source).toBe(2);
        expect(result.third).toBe(3);
        expect(result.fourth).toBe("default");
    });

    test("destructuring assignment whose pattern rebinds the source variable", () => {
        let source = { first: 1, second: 2 };
        let first, second;
        ({ first, second: source, second } = source);
        expect(first).toBe(1);
        expect(source).toBe(2);
        expect(second).toBe(2);
    });

    test("nested pattern rebinding the outer source variable", () => {
        function run(source) {
            var {
                inner: { value: source },
                after,
            } = source;
            return { source, after };
        }
        const result = run({ inner: { value: "inner" }, after: "after" });
        expect(result.source).toBe("inner");
        expect(result.after).toBe("after");
    });

    test("rest property collected after the source variable is rebound", () => {
        function run(source) {
            var { first: source, ...rest } = source;
            return { source, rest };
        }
        const result = run({ first: 1, second: 2, third: 3 });
        expect(result.source).toBe(1);
        expect(result.rest).toEqual({ second: 2, third: 3 });
    });

    test("computed key read after the source variable is rebound", () => {
        function run(source) {
            var key = "second";
            var { first: source, [key]: second } = source;
            return { source, second };
        }
        const result = run({ first: 1, second: 2 });
        expect(result.source).toBe(1);
        expect(result.second).toBe(2);
    });
});
