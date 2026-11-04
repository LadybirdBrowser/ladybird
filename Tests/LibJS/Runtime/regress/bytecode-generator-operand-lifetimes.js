describe("bytecode generator operand lifetime regressions", () => {
    test("computed member calls keep the original base", () => {
        function callMethod() {
            let base = {
                name: "old",
                method() {
                    return this.name;
                },
            };
            const newer = {
                name: "new",
                method() {
                    return this.name;
                },
            };
            return base[((base = newer), "method")]();
        }

        expect(callMethod()).toBe("old");
    });

    test("computed delete keeps the original base", () => {
        function deleteProperty() {
            let base = { x: 1 };
            const old = base;
            const newer = { x: 1 };
            delete base[((base = newer), "x")];
            return [Object.hasOwn(old, "x"), Object.hasOwn(newer, "x")];
        }

        expect(deleteProperty()).toEqual([false, true]);
    });

    test("destructuring assignment keeps its evaluated member base", () => {
        function assignPattern() {
            let base = { x: 0 };
            const old = base;
            const newer = { x: 0 };
            [base[((base = newer), "x")]] = [7];
            return [old.x, newer.x];
        }

        expect(assignPattern()).toEqual([7, 0]);
    });

    test("for-of expression targets keep their evaluated member base", () => {
        function assignLoopTarget() {
            let base = { x: 0 };
            const old = base;
            const newer = { x: 0 };
            for (base[((base = newer), "x")] of [7]) {
            }
            return [old.x, newer.x];
        }

        expect(assignLoopTarget()).toEqual([7, 0]);
    });

    test("computed member reads keep the original base", () => {
        function read() {
            let base = { x: "old" };
            const newer = { x: "new" };
            return base[((base = newer), "x")];
        }

        function optionalRead() {
            let base = { x: "old" };
            const newer = { x: "new" };
            return base?.[((base = newer), "x")];
        }

        expect(read()).toBe("old");
        expect(optionalRead()).toBe("old");
    });
});
