describe("a lexical binding is uninitialized again on each iteration", () => {
    test("let in a loop body", () => {
        function loop() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                try {
                    results.push(value);
                } catch (error) {
                    results.push(error.name);
                }
                let value = i;
            }
            return results;
        }

        expect(loop()).toEqual(["ReferenceError", "ReferenceError"]);
    });

    test("typeof in a loop body", () => {
        function loop() {
            const results = [];
            let i = 0;
            while (i < 2) {
                try {
                    results.push(typeof value);
                } catch (error) {
                    results.push(error.name);
                }
                const value = i++;
            }
            return results;
        }

        expect(loop()).toEqual(["ReferenceError", "ReferenceError"]);
    });

    test("class declaration in a loop body", () => {
        function loop() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                try {
                    results.push(typeof C);
                } catch (error) {
                    results.push(error.name);
                }
                class C {}
            }
            return results;
        }

        expect(loop()).toEqual(["ReferenceError", "ReferenceError"]);
    });

    test("initializer that throws after starting to build its value", () => {
        function loop() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                try {
                    try {
                        results.push(value);
                    } catch (error) {
                        results.push(error.name);
                    }
                    let value = [
                        i,
                        ...(() => {
                            throw new Error();
                        })(),
                    ];
                } catch {}
            }
            return results;
        }

        expect(loop()).toEqual(["ReferenceError", "ReferenceError"]);
    });

    test("switch case entered after an earlier iteration declared the binding", () => {
        function loop() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                switch (i) {
                    case 0:
                        let value = i;
                        break;
                    case 1:
                        try {
                            results.push(value);
                        } catch (error) {
                            results.push(error.name);
                        }
                }
            }
            return results;
        }

        expect(loop()).toEqual(["ReferenceError"]);
    });

    test("for-of head binding read by its own expression", () => {
        function loop() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                try {
                    for (let value of i === 0 ? [i] : [value]) {
                        results.push(value);
                    }
                } catch (error) {
                    results.push(error.name);
                }
            }
            return results;
        }

        expect(loop()).toEqual([0, "ReferenceError"]);
    });

    test("a block in a loop that an inner loop breaks out of", () => {
        function loop() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                for (;;) {
                    try {
                        results.push(value);
                    } catch (error) {
                        results.push(error.name);
                    }
                    let value = i;
                    break;
                }
            }
            return results;
        }

        expect(loop()).toEqual(["ReferenceError", "ReferenceError"]);
    });

    test("a scope around an inner loop that reads the binding", () => {
        function loop() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                {
                    for (let j = 0; j < 2; j++) {
                        try {
                            results.push(value);
                        } catch (error) {
                            results.push(error.name);
                        }
                    }
                    let value = i;
                }
            }
            return results;
        }

        expect(loop()).toEqual(["ReferenceError", "ReferenceError", "ReferenceError", "ReferenceError"]);
    });

    test("a loop that reads a binding of the scope around it keeps the binding", () => {
        function afterClass() {
            const results = [];
            {
                class C {}
                for (let i = 0; i < 2; i++) results.push(typeof C);
            }
            return results;
        }

        function afterCase(which) {
            const results = [];
            switch (which) {
                case 0:
                    let value = "initialized";
                case 1:
                    for (let i = 0; i < 2; i++) {
                        try {
                            results.push(value);
                        } catch (error) {
                            results.push(error.name);
                        }
                    }
            }
            return results;
        }

        expect(afterClass()).toEqual(["function", "function"]);
        expect(afterCase(0)).toEqual(["initialized", "initialized"]);
        expect(afterCase(1)).toEqual(["ReferenceError", "ReferenceError"]);
    });

    test("loop bodies that a statement around the scope starts", () => {
        function whileLoop() {
            const results = [];
            let i = 0;
            while (i < 2) {
                if (i >= 0) {
                    try {
                        results.push(value);
                    } catch (error) {
                        results.push(error.name);
                    }
                    let value = i++;
                }
            }
            return results;
        }

        function doWhileLoop() {
            const results = [];
            let i = 0;
            do {
                label: {
                    try {
                        results.push(value);
                    } catch (error) {
                        results.push(error.name);
                    }
                    let value = i++;
                    if (i > 0) break label;
                }
            } while (i < 2);
            return results;
        }

        function forInLoop() {
            const results = [];
            for (const key in { a: 1, b: 2 }) {
                try {
                    results.push(value);
                } catch (error) {
                    results.push(error.name);
                }
                let value = key;
            }
            return results;
        }

        function forOfLoop() {
            const results = [];
            for (const element of [1, 2]) {
                try {
                    results.push(value);
                } catch (error) {
                    results.push(error.name);
                }
                let value = element;
            }
            return results;
        }

        function* generatorLoop() {
            for (let i = 0; i < 2; i++) {
                try {
                    yield value;
                } catch (error) {
                    yield error.name;
                }
                let value = i;
                yield "initialized";
            }
        }

        const twice = ["ReferenceError", "ReferenceError"];
        expect(whileLoop()).toEqual(twice);
        expect(doWhileLoop()).toEqual(twice);
        expect(forInLoop()).toEqual(twice);
        expect(forOfLoop()).toEqual(twice);
        expect([...generatorLoop()]).toEqual(["ReferenceError", "initialized", "ReferenceError", "initialized"]);
    });

    test("head of a loop in a loop without a block around it", () => {
        function forLoopHead() {
            const results = [];
            for (let i = 0; i < 2; i++)
                try {
                    for (let value = value; ; ) break;
                } catch (error) {
                    results.push(error.name);
                }
            return results;
        }

        function forOfHeadInIf() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                try {
                    if (i >= 0) for (let value of i === 0 ? [i] : [value]) results.push(value);
                } catch (error) {
                    results.push(error.name);
                }
            }
            return results;
        }

        function forInHeadInLabel() {
            const results = [];
            for (let i = 0; i < 2; i++) {
                try {
                    label: for (let value in i === 0 ? { key: 1 } : value) results.push(value);
                } catch (error) {
                    results.push(error.name);
                }
            }
            return results;
        }

        expect(forLoopHead()).toEqual(["ReferenceError", "ReferenceError"]);
        expect(forOfHeadInIf()).toEqual([0, "ReferenceError"]);
        expect(forInHeadInLabel()).toEqual(["key", "ReferenceError"]);
    });

    test("code that a function does not hold the locals of", () => {
        expect(
            eval(`
                const results = [];
                for (let i = 0; i < 2; i++) {
                    try {
                        results.push(value);
                    } catch (error) {
                        results.push(error.name);
                    }
                    let value = i;
                }
                results;
            `)
        ).toEqual(["ReferenceError", "ReferenceError"]);

        const results = [];
        for (let i = 0; i < 2; i++) {
            class C {
                static {
                    for (let j = 0; j < 2; j++) {
                        try {
                            results.push(value);
                        } catch (error) {
                            results.push(error.name);
                        }
                        let value = j;
                    }
                }
            }
        }
        expect(results).toEqual(["ReferenceError", "ReferenceError", "ReferenceError", "ReferenceError"]);
    });

    test("for-in and for-of patterns that read their own bindings", () => {
        function run(loop) {
            const results = [];
            try {
                loop(results);
            } catch (error) {
                results.push(error.name);
            }
            return results;
        }

        expect(
            run(results => {
                for (let [a = b, b] of [
                    [1, 2],
                    [undefined, 3],
                ])
                    results.push(a);
            })
        ).toEqual([1, "ReferenceError"]);
        expect(
            run(results => {
                for (let [a = a] of [[1], []]) results.push(a);
            })
        ).toEqual([1, "ReferenceError"]);
        expect(
            run(results => {
                for (const { a = b, b } of [{ a: 1, b: 2 }, { b: 3 }]) results.push(a);
            })
        ).toEqual([1, "ReferenceError"]);
        expect(
            run(results => {
                for (let { [b]: a, b } of [{ x: 1, b: "x" }, { b: "y" }]) results.push(a);
            })
        ).toEqual(["ReferenceError"]);
        expect(
            run(results => {
                for (let [a = b, b] in { ab: 1, "": 2 }) results.push(a);
            })
        ).toEqual(["a", "ReferenceError"]);
    });

    test("for-await-of patterns that read their own bindings", () => {
        const results = [];
        (async () => {
            try {
                for await (let [a = b, b] of [
                    [1, 2],
                    [undefined, 3],
                ])
                    results.push(a);
            } catch (error) {
                results.push(error.name);
            }
        })();
        runQueuedPromiseJobs();
        expect(results).toEqual([1, "ReferenceError"]);
    });
});
