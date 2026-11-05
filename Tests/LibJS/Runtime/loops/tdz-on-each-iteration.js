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
});
