describe("parsing freestanding async functions", () => {
    test("simple", () => {
        expect(`async function foo() {}`).toEval();
        // Although it does not create an async function it is valid.
        expect(`async
        function foo() {}`).toEval();

        expect(`async function await() {}`).toEval();
        expect(`async function yield() {}`).toEval();
    });
    test("await expression", () => {
        expect(`async function foo() { await bar(); }`).toEval();
        expect(`async function foo() { await; }`).not.toEval();
        expect(`function foo() { await bar(); }`).not.toEval();
        expect(`function foo() { await; }`).toEval();

        expect(`\\u0061sync function foo() { await bar(); }`).not.toEval();
        expect(`\\u0061sync function foo() { \\u0061wait bar(); }`).not.toEval();
        expect(`async function foo() { \\u0061wait bar(); }`).not.toEval();
    });
});

describe("parsing object literal async functions", () => {
    test("simple", () => {
        expect(`x = { async foo() { } }`).toEval();
        expect(`x = { async
                foo() { } }`).not.toEval();
    });

    test("property on object called async", () => {
        expect(`x = { async() { } }`).toEval();
        expect(`x = { async() { await 4; } }`).not.toEval();
        expect(`x = { async: 3 }`).toEval();
        expect(`x = { async: await 3, }`).not.toEval();
    });

    test("await expression", () => {
        expect(`x = { foo() { await bar(); } }`).not.toEval();
        expect(`x = { foo() { await; } }`).toEval();
        expect(`x = { async foo() { await bar(); } }`).toEval();
        expect(`x = { async foo() { await; } }`).not.toEval();
    });
});

describe("parsing classes with async methods", () => {
    test("simple", () => {
        expect(`class Foo { async foo() {} }`).toEval();
        expect(`class Foo { static async foo() {} }`).toEval();
        expect(`class Foo { async foo() { await bar(); } }`).toEval();
        expect(`class Foo { async foo() { await; } }`).not.toEval();
        expect(`class Foo { async constructor() {} }`).not.toEval();
    });
});

test("function expression names equal to 'await'", () => {
    expect(`async function foo() { (function await() {}); }`).toEval();
    expect(`async function foo() { function await() {} }`).not.toEval();
});

test("async function cannot use await in default parameters", () => {
    expect("async function foo(x = await 3) {}").not.toEval();
    expect("async function foo(x = await 3) {}").not.toEval();

    // Even as a reference to some variable it is not allowed
    expect(`
        var await = 4;
        async function foo(x = await) {}
    `).not.toEval();
});

describe("async arrow functions", () => {
    test("basic syntax", () => {
        expect("async () => await 3;").toEval();
        expect("async param => await param();").toEval();
        expect("async (param) => await param();").toEval();
        expect("async (a, b) => await a();").toEval();

        expect("async () => { await 3; }").toEval();
        expect("async param => { await param(); }").toEval();
        expect("async (param) => { await param(); }").toEval();
        expect("async (a, b) => { await a(); }").toEval();

        expect(`async
                () => await 3;`).not.toEval();

        expect("async async => await async()").toEval();
        expect("async => async").toEval();
        expect("async => await async()").not.toEval();

        expect("async (b = await) => await b;").not.toEval();
        expect("async (b = await 3) => await b;").not.toEval();

        // Cannot escape the async keyword and get an async arrow function.
        expect("\\u0061sync () => await 3").not.toEval();

        expect("for (async of => {};false;) {}").toEval();
        expect("for (async of []) {}").not.toEval();

        expect("for (\\u0061sync of []) {}").toEval();
        expect("for (\\u0061sync of => {};false;) {}").not.toEval();
        expect("for (\\u0061sync => {};false;) {}").toEval();
    });

    test("async within a for-loop", () => {
        let called = false;
        // Unfortunately we cannot really test the more horrible case above.
        for (
            const f = async of => {
                return of;
            };
            ;

        ) {
            expect(f(43)).toBeInstanceOf(Promise);

            called = true;
            break;
        }
        expect(called).toBeTrue();
    });
});

test("basic functionality", () => {
    test("simple", () => {
        let executionValue = null;
        let resultValue = null;
        async function foo() {
            executionValue = "someValue";
            return "otherValue";
        }
        const returnValue = foo();
        expect(returnValue).toBeInstanceOf(Promise);
        returnValue.then(result => {
            resultValue = result;
        });
        runQueuedPromiseJobs();
        expect(executionValue).toBe("someValue");
        expect(resultValue).toBe("otherValue");
    });

    test("await", () => {
        let resultValue = null;
        async function foo() {
            return "someValue";
        }
        async function bar() {
            resultValue = await foo();
        }
        bar();
        runQueuedPromiseJobs();
        expect(resultValue).toBe("someValue");
    });
});

describe("non async function declaration usage of async still works", () => {
    test("async as a function", () => {
        function async(value = 4) {
            return value;
        }

        expect(async(0)).toBe(0);

        // We use eval here since it otherwise cannot find the async function.
        const evalResult = eval("async(1)");
        expect(evalResult).toBe(1);
    });

    test("async as a variable", () => {
        let async = 3;

        const evalResult = eval("async >= 2");
        expect(evalResult).toBeTrue();
    });

    test("async with line ending does not create a function", () => {
        expect(() => {
            // The ignore is needed otherwise prettier puts a ';' after async.
            // prettier-ignore
            async
            function f() {}
        }).toThrowWithMessage(ReferenceError, "'async' is not defined");

        expect(`async
                function f() { await 3; }`).not.toEval();
    });
});

describe("await cannot be used in class static init blocks", () => {
    test("directly", () => {
        expect("class A{ static { await; } }").not.toEval();
        expect("class A{ static { let await = 3; } }").not.toEval();
        expect("class A{ static { call(await); } }").not.toEval();
        expect("class A{ static { for(const await = 1; false ;) {} } }").not.toEval();
    });

    test("via declaration", () => {
        expect("class A{ static { class await {} } }").not.toEval();
        expect("class A{ static { function await() {} } }").not.toEval();
        expect("class A{ static { function* await() {} } }").not.toEval();
        expect("class A{ static { async function* await() {} } }").not.toEval();
    });
});

describe("await thenables", () => {
    test("async returning a thanable variable without fulfilling", () => {
        let isCalled = false;
        const obj = {
            then() {
                isCalled = true;
            },
        };

        const f = async () => await obj;
        f();
        runQueuedPromiseJobs();
        expect(isCalled).toBe(true);
    });

    test("async returning a thanable variable that fulfills", () => {
        let isCalled = false;
        const obj = {
            then(fulfill) {
                isCalled = true;
                fulfill(isCalled);
            },
        };

        const f = async () => await obj;
        f();
        runQueuedPromiseJobs();
        expect(isCalled).toBe(true);
    });

    test("async returning a thenable directly without fulfilling", () => {
        let isCalled = false;
        const f = async () => ({
            then() {
                isCalled = true;
            },
        });
        f();
        runQueuedPromiseJobs();
        expect(isCalled).toBe(true);
    });

    test("async returning a thenable directly that fulfills", () => {
        let isCalled = false;
        const f = async () => ({
            then(fulfill) {
                isCalled = true;
                fulfill(isCalled);
            },
        });
        f();
        runQueuedPromiseJobs();
        expect(isCalled).toBe(true);
    });
});

describe("await observably looks up constructor of Promise objects", () => {
    let calls = 0;
    function makeConstructorObservable(promise) {
        Object.defineProperty(promise, "constructor", {
            get() {
                calls++;
                return Promise;
            },
        });
        return promise;
    }

    async function test() {
        await makeConstructorObservable(Promise.resolve(1));
        await makeConstructorObservable(
            new Promise(resolve => {
                resolve();
            })
        );
        await makeConstructorObservable(new Boolean(true));
        await makeConstructorObservable({});
        await makeConstructorObservable(new Number(2));
        try {
            await makeConstructorObservable(Promise.reject(3));
        } catch {}
        try {
            return makeConstructorObservable(Promise.reject(1));
        } catch {
            return 2;
        }
    }
    test();
    runQueuedPromiseJobs();
    expect(calls).toBe(4);
});
