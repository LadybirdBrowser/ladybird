// Properties of the global object that hold an object and were never assigned are constants in compiled code, which
// depends on them staying unassigned. Each case compiles a function that calls such a property, then changes the
// property in another way, from compiled code where it can, and checks that the caller sees the change.

function compile(f, warmUp = () => {}) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) warmUp(f, i);
    jit.compile(f);
    return f;
}

const callEach = f => f();

test("a declared function is assigned by compiled code", () => {
    evaluateSource("function jitConstantAssigned() { return 1; }");
    const caller = compile(() => jitConstantAssigned(), callEach);
    expect(caller()).toBe(1);
    const assign = compile(value => {
        jitConstantAssigned = value;
    });
    assign(() => 2);
    expect(caller()).toBe(2);

    // Once assigned, the property is loaded, and compiled code that stores to it like the caches do is seen too.
    const assignAgain = compile(
        value => {
            jitConstantAssigned = value;
        },
        (f, i) => f(() => 3 + i)
    );
    const callerAgain = compile(() => jitConstantAssigned(), callEach);
    expect(callerAgain()).toBe(12);
    assignAgain(() => 20);
    expect(callerAgain()).toBe(20);
    expect(caller()).toBe(20);
});

test("a function a var initialized is assigned", () => {
    evaluateSource("var jitConstantVar = function () { return 1; };");
    const caller = compile(() => jitConstantVar(), callEach);
    expect(caller()).toBe(1);
    evaluateSource("jitConstantVar = function () { return 2; };");
    expect(caller()).toBe(2);
});

test("compiled code that stores to properties of other objects stores to the global object", () => {
    evaluateSource("function jitConstantNamed() { return 1; }");
    const caller = compile(() => jitConstantNamed(), callEach);
    expect(caller()).toBe(1);
    const store = compile(
        (object, value) => {
            object.jitConstantNamed = value;
        },
        (f, i) => f({ jitConstantNamed: i }, i)
    );
    store(globalThis, () => 2);
    expect(caller()).toBe(2);
});

test("compiled code that stores to keyed properties of other objects stores to the global object", () => {
    evaluateSource("function jitConstantKeyed() { return 1; }");
    const caller = compile(() => jitConstantKeyed(), callEach);
    expect(caller()).toBe(1);
    const store = compile(
        (object, key, value) => {
            object[key] = value;
        },
        (f, i) => f({ jitConstantKeyed: i }, "jitConstantKeyed", i)
    );
    store(globalThis, "jitConstantKeyed", () => 2);
    expect(caller()).toBe(2);
});

test("a declared function is redefined", () => {
    evaluateSource("function jitConstantDefined() { return 1; }");
    const caller = compile(() => jitConstantDefined(), callEach);
    expect(caller()).toBe(1);
    Object.defineProperty(globalThis, "jitConstantDefined", { value: () => 2 });
    expect(caller()).toBe(2);
});

test("a function is replaced by Object.assign()", () => {
    evaluateSource("function jitConstantAssign() { return 1; }");
    const caller = compile(() => jitConstantAssign(), callEach);
    expect(caller()).toBe(1);
    Object.assign(globalThis, { jitConstantAssign: () => 2 });
    expect(caller()).toBe(2);
});

test("a property becomes an accessor", () => {
    globalThis.jitConstantAccessor = () => 1;
    const caller = compile(() => jitConstantAccessor(), callEach);
    expect(caller()).toBe(1);
    Object.defineProperty(globalThis, "jitConstantAccessor", { get: () => () => 2 });
    expect(caller()).toBe(2);
});

test("a property is deleted, and one after it moves", () => {
    globalThis.jitConstantDeleted = () => 1;
    globalThis.jitConstantMoved = () => 2;
    const callDeleted = compile(() => jitConstantDeleted(), callEach);
    const callMoved = compile(() => jitConstantMoved(), callEach);
    expect(callDeleted()).toBe(1);
    expect(callMoved()).toBe(2);
    delete globalThis.jitConstantDeleted;
    expect(callDeleted).toThrowWithMessage(ReferenceError, "'jitConstantDeleted' is not defined");
    expect(callMoved()).toBe(2);
    globalThis.jitConstantDeleted = () => 3;
    expect(callDeleted()).toBe(3);
    expect(callMoved()).toBe(2);
});

test("a later script declares a binding that shadows the property", () => {
    globalThis.jitConstantShadowed = () => 1;
    const caller = compile(() => jitConstantShadowed(), callEach);
    expect(caller()).toBe(1);
    evaluateSource("let jitConstantShadowed = () => 2;");
    expect(caller()).toBe(2);
});

test("a variable is initialized and then assigned by the same code", () => {
    evaluateSource("var jitConstantInitialized;");
    const assign = value => {
        jitConstantInitialized = value;
    };
    assign(() => 1);
    const caller = compile(() => jitConstantInitialized(), callEach);
    expect(caller()).toBe(1);
    assign(() => 2);
    expect(caller()).toBe(2);
});
