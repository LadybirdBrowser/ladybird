test("compound assignments retain the environment resolved before the getter", () => {
    const cases = [
        ["+=", 6, 2, 8],
        ["-=", 6, 2, 4],
        ["*=", 6, 2, 12],
        ["/=", 6, 2, 3],
        ["%=", 6, 4, 2],
        ["**=", 6, 2, 36],
        ["<<=", 6, 2, 24],
        [">>=", 6, 1, 3],
        [">>>=", 6, 1, 3],
        ["&=", 6, 3, 2],
        ["|=", 6, 3, 7],
        ["^=", 6, 3, 5],
        ["&&=", true, 2, 2],
        ["||=", false, 2, 2],
        ["??=", null, 2, 2],
    ];

    for (const [operator, initial, rhs, expected] of cases) {
        const scope = {
            get x() {
                delete this.x;
                return initial;
            },
        };
        const run = new Function(
            "scope",
            "rhs",
            `var x = 100; var result; with (scope) { result = x ${operator} rhs; } return [result, x];`
        );
        expect(run(scope, rhs)).toEqual([expected, 100]);
        expect(scope.x).toBe(expected);
    }
});

test("compound assignments retain the environment through RHS side effects", () => {
    for (const operator of ["+=", "&&=", "||=", "??="]) {
        const initial = operator === "||=" ? false : operator === "??=" ? null : 1;
        const scope = { x: initial };
        const rhs = () => {
            delete scope.x;
            return 2;
        };
        const run = new Function("scope", "rhs", `var x = 100; with (scope) { x ${operator} rhs(); } return x;`);
        expect(run(scope, rhs)).toBe(100);
        expect(scope.x).toBe(operator === "+=" ? 3 : 2);
    }
});

test("updates retain the environment resolved before the getter", () => {
    for (const [expression, expected, result] of [
        ["++x", 3, 3],
        ["x++", 3, 2],
        ["--x", 1, 1],
        ["x--", 1, 2],
    ]) {
        const scope = {
            get x() {
                delete this.x;
                return 2;
            },
        };
        const run = new Function(
            "scope",
            `var x = 100; var result; with (scope) { result = ${expression}; } return [result, x];`
        );
        expect(run(scope)).toEqual([result, 100]);
        expect(scope.x).toBe(expected);
    }
});

test("updates resolve unscopables only once", () => {
    for (const expression of ["++x", "x++", "--x", "x--"]) {
        let lookups = 0;
        const scope = {
            x: 2,
            get [Symbol.unscopables]() {
                lookups++;
                return { x: false };
            },
        };
        const run = new Function("scope", `with (scope) { return ${expression}; }`);
        run(scope);
        expect(lookups).toBe(1);
    }
});
