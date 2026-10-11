for (const operator of ["==", "!=", "===", "!=="]) {
    for (const branch of [false, true]) {
        const body = branch ? `if (a ${operator} b) return true; return false;` : `return a ${operator} b;`;
        const negated = operator === "!=" || operator === "!==";
        const loose = operator === "==" || operator === "!=";

        test(`compiled object ${operator} ${branch ? "branch" : "value"} compares identities without coercion`, () => {
            const compare = new Function("a", "b", body);
            const object = {
                [Symbol.toPrimitive]() {
                    throw new Error("objects must not be coerced");
                },
            };
            const other = {};
            jit.prepare(compare);
            for (let i = 0; i < 10; ++i) {
                compare(object, object);
                compare(object, other);
            }
            expect(jit.compile(compare)).toBe(jit.enabled);
            const proxy = new Proxy(object, {
                get() {
                    throw new Error("proxies must not be inspected");
                },
            });
            const objects = [object, other, proxy, [], function () {}, new Number(1)];
            for (const a of objects) {
                for (const b of objects) expect(compare(a, b)).toBe(negated ? a !== b : a === b);
            }
            expect(jit.exitCount(compare)).toBe(0);
        });

        test(`compiled object ${operator} ${branch ? "branch" : "value"} handles changed operand types`, () => {
            for (const swapped of [false, true]) {
                const compare = new Function("a", "b", body);
                let coercions = 0;
                const object = {
                    [Symbol.toPrimitive]() {
                        ++coercions;
                        return 7;
                    },
                };
                jit.prepare(compare);
                for (let i = 0; i < 10; ++i) compare(object, {});
                expect(jit.compile(compare)).toBe(jit.enabled);
                expect(swapped ? compare(7, object) : compare(object, 7)).toBe(loose !== negated);
                expect(coercions).toBe(loose ? 1 : 0);
                expect(compare(null, undefined)).toBe(loose !== negated);
                expect(compare(false, 0)).toBe(loose !== negated);
                expect(compare("7", 7)).toBe(loose !== negated);
                expect(compare(7n, 7)).toBe(loose !== negated);
                expect(compare(NaN, NaN)).toBe(negated);
                expect(compare(-0, 0)).toBe(!negated);
                expect(compare(object, object)).toBe(!negated);
                expect(compare(object, {})).toBe(negated);
            }
        });

        test(`mixed feedback for ${operator} ${branch ? "branch" : "value"} preserves other types`, () => {
            for (const objectsFirst of [false, true]) {
                const compare = new Function("a", "b", body);
                const object = {};
                jit.prepare(compare);
                for (let i = 0; i < 10; ++i) {
                    if (objectsFirst) compare(object, {});
                    compare(null, undefined);
                    compare(false, false);
                    if (!objectsFirst) compare(object, {});
                }
                expect(jit.compile(compare)).toBe(jit.enabled);
                expect(compare(null, undefined)).toBe(loose !== negated);
                expect(compare(false, false)).toBe(!negated);
                expect(compare(object, object)).toBe(!negated);
                expect(compare(object, {})).toBe(negated);
            }
        });
    }
}
