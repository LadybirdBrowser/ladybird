// Each source has an @ where the instruction that matters is.
function positionOfMarker(source) {
    const before = source.slice(0, source.indexOf("@")).split("\n");
    // NB: The function around the source starts two lines above it.
    return [before.length + 2, before[before.length - 1].length + 1];
}

function positionInStack(stack) {
    const frame = stack.split("\n").find(line => line.includes("at anonymous"));
    const match = frame.match(/(\d+):(\d+)\)?$/);
    return [Number(match[1]), Number(match[2])];
}

function positionSeenByProbe(source) {
    let position = null;
    const probe = () => {
        position ??= positionInStack(new Error().stack);
    };
    const object = {
        get getter() {
            probe();
            return 1;
        },
        set setter(value) {
            probe();
        },
        get callable() {
            probe();
            return () => {};
        },
        method() {
            probe();
        },
    };
    object.object = object;
    const value = {
        toString() {
            probe();
            return "";
        },
    };
    const iterable = {
        [Symbol.iterator]() {
            probe();
            return { next: () => ({ done: true }) };
        },
    };
    const proxy = new Proxy(
        {},
        {
            ownKeys() {
                probe();
                return [];
            },
            has(target, key) {
                if (key !== Symbol.unscopables) probe();
                return true;
            },
            get: () => () => {},
        }
    );
    new Function("object", "value", "iterable", "proxy", "call", "Class", "key", source.replace("@", ""))(
        object,
        value,
        iterable,
        proxy,
        () => probe(),
        function () {
            probe();
        },
        "setter"
    );
    return position;
}

function expectProbePositions(sources) {
    for (const source of sources)
        expect([source, positionSeenByProbe(source)]).toEqual([source, positionOfMarker(source)]);
}

function positionOfError(source) {
    const u = undefined;
    const object = {};
    try {
        new Function("u", "object", "key", source.replace("@", ""))(u, object, "key");
    } catch (error) {
        return positionInStack(error.stack);
    }
    return null;
}

function expectErrorPositions(sources) {
    for (const source of sources) expect([source, positionOfError(source)]).toEqual([source, positionOfMarker(source)]);
}

test("stores are where their operator is", () => {
    expectProbePositions([
        "object.setter @= 1;",
        "object.object.setter @= 1;",
        'object["setter"] @= 1;',
        "object[key] @= 1;",
        "(object).setter @= 1;",
        "object\n    .setter\n    @= 1;",
        "object.setter @+= 1;",
        "object[key] @+= 1;",
        "object.setter @||= 1;",
        "object.setter @??= 1;",
        "object.setter@++;",
        "object.setter@--;",
        "object[key]@++;",
        "@++object.setter;",
        "@--object[key];",
    ]);
});

test("stores of patterns are where the assignment is", () => {
    expectProbePositions([
        "[object.setter] @= [1];",
        "({ a: object.setter } @= { a: 1 });",
        "[object.setter = 1] @= [];",
        "[...object.setter] @= [];",
    ]);
});

test("stores of loop heads are where the keyword is", () => {
    expectProbePositions(["for (object.setter @of [1]);", "for (object.setter @in { a: 1 });"]);
});

test("calls are where their arguments start", () => {
    expectProbePositions([
        "call@();",
        "object.method@();",
        'object["method"]@();',
        "object\n    .method@();",
        "(object.method)@();",
        "@new Class();",
        "@new Class;",
    ]);
});

test("reading what gets called is where the call is", () => {
    expectProbePositions(["object.callable@();", "object\n    .callable\n    @();"]);
});

test("member accesses are where their operator is", () => {
    expectProbePositions([
        "object@.getter;",
        "object.object@.getter;",
        "object\n    @.getter;",
        'object@["getter"];',
        "object.object@['getter'];",
    ]);
});

test("everything in an optional chain is where the chain starts", () => {
    expectProbePositions(["object@?.getter;", "object@?.object.getter;", "object@?.method();", "object.method@?.();"]);
});

test("iteration of a spread element is where what it is in is", () => {
    expectProbePositions(["@[...iterable];", "@[1, ...iterable];", "call@(...iterable);", "@new Class(...iterable);"]);
});

test("patterns of declarations are where the declaration is", () => {
    expectProbePositions([
        "@var [a] = iterable;",
        "@let [a] = iterable;",
        "@const { getter } = object;",
        "for (var a @of iterable);",
        "@try { throw object; } catch ({ getter }) {}",
    ]);
});

test("other constructs", () => {
    expectProbePositions([
        "@`${value}`;",
        "(@{ ...proxy });",
        "with (proxy) { missing@(); }",
        "with (proxy) { missing @= 1; }",
        "with (proxy) { @typeof missing; }",
        "with (proxy) { @delete missing; }",
    ]);
});

test("errors", () => {
    expectErrorPositions([
        "u@.x;",
        "u@[key];",
        "u.x @= 1;",
        "u[key] @= 1;",
        "u.x @+= 1;",
        "u.x@++;",
        "@++u.x;",
        "u.x@();",
        "object.missing@();",
        "object.missing@.x;",
        "object.missing.x @= 1;",
        "@new u();",
        "@class C extends u {}",
        "@with (u) {}",
        "typeof @tdz; let tdz;",
    ]);
});
