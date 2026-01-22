describe("parsing", () => {
    test("single name", () => {
        expect(`var { a } = {};`).toEval();
        expect(`const { a } = {};`).toEval();
        expect(`let { a } = {};`).toEval();
    });

    test("single name with rest values", () => {
        expect(`var { a, ...rest } = {};`).toEval();
        expect(`const { a, ...rest } = {};`).toEval();
        expect(`let { a, ...rest } = {};`).toEval();
    });

    test("single aliased name", () => {
        expect(`var { a: b } = {};`).toEval();
        expect(`const { a: b } = {};`).toEval();
        expect(`let { a: b } = {};`).toEval();
    });

    test("single aliased name with rest values", () => {
        expect(`var { a: b, ...rest } = {};`).toEval();
        expect(`const { a: b, ...rest } = {};`).toEval();
        expect(`let { a: b, ...rest } = {};`).toEval();
    });

    test("array destructuring patterns", () => {
        expect(`var [ a ] = [];`).toEval();
        expect(`const [ a ] = [];`).toEval();
        expect(`let [ a ] = [];`).toEval();
    });

    test("destructuring assignment with rest assignments, array patterns", () => {
        expect(`var [ a, ...rest ] = [];`).toEval();
        expect(`const [ a, ...rest ] = [];`).toEval();
        expect(`let [ a, ...rest ] = [];`).toEval();
    });

    test("destructuring assignment with rest names, array patterns with recursive patterns", () => {
        expect(`var [ a, [ ...rest ] ] = [];`).toEval();
        expect(`const [ a, [ ...rest ] ] = [];`).toEval();
        expect(`let [ a, [ ...rest ] ] = [];`).toEval();
    });

    test("destructuring assignment with rest names, array patterns with recursive patterns 2", () => {
        expect(`var [ a, [ ...{length} ] ] = [];`).toEval();
        expect(`const [ a, [ ...{length} ] ] = [];`).toEval();
        expect(`let [ a, [ ...{length} ] ] = [];`).toEval();
    });

    test("function parameters cannot use member expressions", () => {
        expect("function f([a.b]) {}").not.toEval();
        expect("function f([b[0]]) {}").not.toEval();

        expect("function f({c:a.b}) {}").not.toEval();
        expect("function f({a:b[0]}) {}").not.toEval();

        expect("([a.b]) => 1").not.toEval();
        expect("([b[0]]) => 2").not.toEval();

        expect("({c:a.b}) => 3").not.toEval();
        expect("({a:b[0]}) => 4").not.toEval();
    });
});

describe("evaluating", () => {
    test("single name", () => {
        let o = { a: 1 };
        {
            var { a } = o;
            expect(a).toBe(o.a);
        }
        {
            const { a } = o;
            expect(a).toBe(o.a);
        }
        {
            let { a } = o;
            expect(a).toBe(o.a);
        }
    });

    test("single name with rest values", () => {
        let o = { a: 1, b: 2 };
        {
            var { a, ...b } = o;
            expect(a).toBe(o.a);
            expect(b).toEqual({ b: 2 });
        }
        {
            const { a, ...b } = o;
            expect(a).toBe(o.a);
            expect(b).toEqual({ b: 2 });
        }
        {
            let { a, ...b } = o;
            expect(a).toBe(o.a);
            expect(b).toEqual({ b: 2 });
        }
    });

    test("single aliased name", () => {
        let o = { a: 1 };
        {
            var { a: x } = o;
            expect(x).toBe(o.a);
        }
        {
            const { a: x } = o;
            expect(x).toBe(o.a);
        }
        {
            let { a: x } = o;
            expect(x).toBe(o.a);
        }
    });

    test("single aliased name with rest values", () => {
        let o = { a: 1, b: 2 };
        {
            var { a: x, ...b } = o;
            expect(x).toBe(o.a);
            expect(b).toEqual({ b: 2 });
        }
        {
            const { a: x, ...b } = o;
            expect(x).toBe(o.a);
            expect(b).toEqual({ b: 2 });
        }
        {
            let { a: x, ...b } = o;
            expect(x).toBe(o.a);
            expect(b).toEqual({ b: 2 });
        }
    });

    test("array patterns", () => {
        let o = [1, 2, 3, 4];
        {
            var [a, b] = o;
            expect(a).toBe(o[0]);
            expect(b).toBe(o[1]);
        }
        {
            const [a, b] = o;
            expect(a).toBe(o[0]);
            expect(b).toBe(o[1]);
        }
        {
            let [a, b] = o;
            expect(a).toBe(o[0]);
            expect(b).toBe(o[1]);
        }
    });

    test("destructuring assignment with rest names, array patterns", () => {
        let o = [1, 2, 3, 4];
        {
            var [a, ...b] = o;
            expect(a).toBe(o[0]);
            expect(b).toEqual(o.slice(1));
        }
        {
            const [a, ...b] = o;
            expect(a).toBe(o[0]);
            expect(b).toEqual(o.slice(1));
        }
        {
            let [a, ...b] = o;
            expect(a).toBe(o[0]);
            expect(b).toEqual(o.slice(1));
        }
    });

    test("destructuring assignment with rest names, array patterns with recursive patterns", () => {
        let o = [1, [2, 3, 4]];
        {
            var [a, [b, ...c]] = o;
            expect(a).toBe(o[0]);
            expect(b).toBe(o[1][0]);
            expect(c).toEqual(o[1].slice(1));
        }
        {
            const [a, [b, ...c]] = o;
            expect(a).toBe(o[0]);
            expect(b).toBe(o[1][0]);
            expect(c).toEqual(o[1].slice(1));
        }
        {
            let [a, [b, ...c]] = o;
            expect(a).toBe(o[0]);
            expect(b).toBe(o[1][0]);
            expect(c).toEqual(o[1].slice(1));
        }
    });

    test("destructuring assignment with rest names, array patterns with recursive patterns 2", () => {
        let o = [1, [2, 3, 4]];
        {
            var [a, [...{ length }]] = o;
            expect(a).toBe(o[0]);
            expect(length).toBe(o[1].length);
        }
        {
            const [a, [...{ length }]] = o;
            expect(a).toBe(o[0]);
            expect(length).toBe(o[1].length);
        }
        {
            let [a, [...{ length }]] = o;
            expect(a).toBe(o[0]);
            expect(length).toBe(o[1].length);
        }
        {
            expect(() => {
                let [a, b, [...{ length }]] = o;
            }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
        }
    });

    test("patterns with default", () => {
        let o = { a: 1 };
        let { x = "foo", a = "bar" } = o;
        expect(x).toBe("foo");
        expect(a).toBe(o.a);
    });

    test("can use big int values as number-like properties", () => {
        let o = { "99999999999999999": 1 };
        let { 123n: a = "foo", 99999999999999999n: b = "bar" } = o;
        expect(a).toBe("foo");
        expect(b).toBe(1);
    });
});
