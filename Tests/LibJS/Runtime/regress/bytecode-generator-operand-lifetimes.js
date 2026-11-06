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

    test("tagged templates preserve the tag, receiver, and earlier substitutions", () => {
        function localTag() {
            let tag = () => "old";
            return tag`${(tag = () => "new")}`;
        }

        function memberTag() {
            let base = {
                name: "old",
                tag() {
                    return this.name;
                },
            };
            const newer = {
                name: "new",
                tag() {
                    return this.name;
                },
            };
            return base.tag`${(base = newer)}`;
        }

        function substitutionTag() {
            let value = 1;
            return ((strings, first, second) => [first, second])`${value}${(value = 2)}`;
        }

        expect(localTag()).toBe("old");
        expect(memberTag()).toBe("old");
        expect(substitutionTag()).toEqual([1, 2]);
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

    test("converting a computed key keeps the value of `this`", () => {
        const receiver = {
            toString() {
                return "key";
            },
        };

        function objectLiteral() {
            const object = { [this]: 1 };
            return [this, object.key];
        }

        function bindingPattern() {
            const { [this]: value } = { key: 1 };
            return [this, value];
        }

        function classElement() {
            class C {
                [this]() {}
            }
            return [this, Object.hasOwn(C.prototype, "key")];
        }

        expect(objectLiteral.call(receiver)).toEqual([receiver, 1]);
        expect(bindingPattern.call(receiver)).toEqual([receiver, 1]);
        expect(classElement.call(receiver)).toEqual([receiver, true]);
    });

    test("a destructuring assignment evaluates to the value it started with", () => {
        function objectPattern() {
            const source = { a: 1 };
            let value = source;
            const result = ({ a: value } = value);
            return [result === source, value];
        }

        function arrayPattern() {
            const source = [1];
            let value = source;
            const result = ([value] = value);
            return [result === source, value];
        }

        function computedKey() {
            const source = { a: 1 };
            let value = source;
            let target;
            const result = ({ [((value = null), "a")]: target } = value);
            return [result === source, target];
        }

        expect(objectPattern()).toEqual([true, 1]);
        expect(arrayPattern()).toEqual([true, 1]);
        expect(computedKey()).toEqual([true, 1]);
    });
});
