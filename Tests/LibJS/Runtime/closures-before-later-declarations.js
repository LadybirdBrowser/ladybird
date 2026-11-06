test("closures in blocks see what gets declared after them", () => {
    function outer() {
        let first = 1;
        const closures = [];
        {
            let inner = 2;
            closures.push(() => first + inner + second);
        }
        let second = 3;
        {
            let inner = 4;
            closures.push(() => first + inner + second);
        }
        let third = 5;
        {
            let inner = 6;
            closures.push(() => first + inner + second + third);
        }
        return closures;
    }

    expect(outer().map(closure => closure())).toEqual([6, 8, 15]);
});

test("a closure that runs before a later declaration finds it in its dead zone", () => {
    function outer() {
        let closure;
        {
            let inner = 1;
            closure = () => inner + later;
        }
        expect(closure).toThrow(ReferenceError);
        let later = 2;
        return closure();
    }

    expect(outer()).toBe(3);
});

test("a later declaration shadows a global for closures made before it", () => {
    globalThis.declaredLaterInFunction = "global";
    function outer() {
        let closure;
        {
            let inner = 1;
            closure = () => [inner, declaredLaterInFunction];
        }
        let declaredLaterInFunction = "local";
        return closure();
    }

    expect(outer()).toEqual([1, "local"]);
    delete globalThis.declaredLaterInFunction;
});

test("closures made between declarations of nested scopes keep their coordinates", () => {
    function outer() {
        const closures = [];
        let a = "a";
        {
            let b = "b";
            closures.push(() => a + b);
            {
                let c = "c";
                closures.push(() => a + b + c + d);
            }
            let d = "d";
            closures.push(() => a + b + d + e);
        }
        let e = "e";
        return closures;
    }

    expect(outer().map(closure => closure())).toEqual(["ab", "abcd", "abde"]);
});

test("direct eval in a closure sees bindings declared after the closure", () => {
    function outer() {
        let closure;
        {
            let inner = 1;
            closure = code => eval(code);
        }
        let later = 2;
        return closure("inner + later");
    }

    expect(outer()).toBe(3);
});
