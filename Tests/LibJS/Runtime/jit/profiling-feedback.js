// The feedback the profiling tier records for functions that jit.prepare() moved to it, as jit.feedback() describes
// it: one line per instruction with feedback slots, with what each slot recorded. Loads and calls keep the last value
// they produced, which jit.feedback() folds into the kinds of values its slot saw.

// What the slots of the first instruction named `name` in the feedback of `f` recorded.
function feedbackOf(f, name) {
    const marker = `] ${name} `;
    const line = jit
        .feedback(f)
        .split("\n")
        .find(line => line.includes(marker));
    return line?.substring(line.indexOf(marker) + marker.length);
}

test("arithmetic records the kinds of its operands", () => {
    function add(a, b) {
        return a + b;
    }
    jit.prepare(add);
    add(1, 2);
    expect(feedbackOf(add, "Add")).toBe("arith#0: Int32");
    add(1, 0.5);
    add(0x7fffffff, 1);
    expect(feedbackOf(add, "Add")).toBe("arith#0: Int32|Double|Int32Overflow");
    add("a", 1);
    add(1n, 2n);
    expect(feedbackOf(add, "Add")).toBe("arith#0: Int32|Double|Int32Overflow|String|BigInt");

    function negate(value) {
        return -value;
    }
    jit.prepare(negate);
    negate({});
    expect(feedbackOf(negate, "UnaryMinus")).toBe("arith#0: Other");
});

test("comparisons record the kinds of their operands", () => {
    function less(a, b) {
        return a < b;
    }
    function same(a, b) {
        return a === b;
    }
    jit.prepare(less);
    jit.prepare(same);
    less(1, 2);
    less(1.5, 2);
    same("a", "a");
    expect(feedbackOf(less, "LessThan")).toBe("arith#0: Int32|Double");
    expect(feedbackOf(same, "StrictlyEquals")).toBe("arith#0: String");
});

test("loads and calls record the kinds of the values they produce", () => {
    function read(object) {
        return object.value;
    }
    jit.prepare(read);
    read({ value: 1 });
    expect(feedbackOf(read, "GetById")).toBe("value#0: Int32");
    read({ value: "string" });
    expect(feedbackOf(read, "GetById")).toBe("value#0: Int32|String");
    read({ value: undefined });
    expect(feedbackOf(read, "GetById")).toBe("value#0: Int32|String|Undefined");

    function call(f) {
        return f();
    }
    jit.prepare(call);
    call(() => null);
    expect(feedbackOf(call, "Call")).toBe("value#0: Null call#0: target=function <anonymous> flags=none");
    call(() => 1.5);
    expect(feedbackOf(call, "Call")).toBe(
        "value#0: Double|Null call#0: target=function <anonymous> flags=Polymorphic|OtherFunctions"
    );
});

test("calls record their first callee and how the others differ", () => {
    function first() {}
    function second() {}
    function call(f) {
        return f();
    }
    jit.prepare(call);
    call(first);
    call(first);
    expect(feedbackOf(call, "Call")).toBe("value#0: Undefined call#0: target=function first flags=none");
    call(second);
    expect(feedbackOf(call, "Call")).toBe(
        "value#0: Undefined call#0: target=function first flags=Polymorphic|OtherFunctions"
    );
    call(Math.random);
    expect(feedbackOf(call, "Call")).toBe(
        "value#0: Double|Undefined call#0: target=function first flags=Polymorphic|SawNative|OtherFunctions"
    );

    function callClosure(f) {
        return f();
    }
    const makeClosure = value => () => value;
    jit.prepare(callClosure);
    callClosure(makeClosure(1));
    callClosure(makeClosure(2));
    expect(feedbackOf(callClosure, "Call")).toBe(
        "value#0: Int32 call#0: target=function <anonymous> flags=Polymorphic"
    );

    function construct(C) {
        return new C();
    }
    jit.prepare(construct);
    construct(Object);
    expect(feedbackOf(construct, "CallConstruct")).toBe(
        "value#0: Object call#0: target=native function Object flags=SawNative|SawConstruct"
    );
});

test("calls record where their callee forwarded them to", () => {
    function target(a, b) {
        return a + b;
    }
    function viaCall(f) {
        return f.call(null, 1, 2);
    }
    function viaApply(f) {
        return f.apply(null, [1, 2, 3]);
    }
    function viaBound(f) {
        return f(2);
    }
    function viaCallback(values) {
        let sum = 0;
        values.forEach(function add(value) {
            sum += value;
        });
        return sum;
    }
    for (const f of [viaCall, viaApply, viaBound, viaCallback]) jit.prepare(f);
    viaCall(target);
    viaApply(target);
    viaBound(target.bind(null, 1));
    viaCallback([1, 2]);
    expect(feedbackOf(viaCall, "Call")).toBe(
        "value#1: Int32 call#0: target=native function call flags=SawNative forwarded by call with 2 arguments to function target"
    );
    expect(feedbackOf(viaApply, "Call")).toBe(
        "value#1: Int32 call#0: target=native function apply flags=SawNative forwarded by apply with 3 arguments to function target"
    );
    expect(feedbackOf(viaBound, "Call")).toBe(
        "value#0: Int32 call#0: target=function target flags=SawNative forwarded by bound with 2 arguments to function target"
    );
    expect(feedbackOf(viaCallback, "Call")).toBe(
        "value#1: none call#0: target=native function forEach flags=SawNative forwarded by callback with 0 arguments to function add"
    );

    function other(a, b) {
        return a * b;
    }
    viaCall(other);
    expect(feedbackOf(viaCall, "Call")).toBe(
        "value#1: Int32 call#0: target=native function call flags=SawNative|ForwardedPolymorphic forwarded by call with 2 arguments to function target"
    );
});

test("keyed accesses record their keys and elements", () => {
    function get(object, key) {
        return object[key];
    }
    jit.prepare(get);
    get([1, 2, 3], 1);
    expect(feedbackOf(get, "GetByValue")).toBe("value#0: Int32 keyed#0: keys=Int32Index elements=Packed");
    get([1, , 3], 1);
    expect(feedbackOf(get, "GetByValue")).toBe(
        "value#0: Int32|Undefined keyed#0: keys=Int32Index elements=Packed|Holey out-of-bounds"
    );
    get(new Float64Array(2), 0);
    get({ x: 1 }, "x");
    expect(feedbackOf(get, "GetByValue")).toBe(
        'value#0: Int32|Undefined keyed#0: keys=Int32Index|String elements=Packed|Holey|Other|Float64Array out-of-bounds last_key="x"'
    );
    get({ y: 1 }, "y");
    expect(feedbackOf(get, "GetByValue")).toBe(
        'value#0: Int32|Undefined keyed#0: keys=Int32Index|String|MultipleKeys elements=Packed|Holey|Other|Float64Array out-of-bounds last_key="y"'
    );

    function fill(count) {
        const values = new Array(count);
        for (let i = 0; i < count; ++i) values[i] = i;
        return values;
    }
    jit.prepare(fill);
    fill(4);
    expect(feedbackOf(fill, "PutByValue")).toBe("keyed#0: keys=Int32Index elements=Holey out-of-bounds");
});

test("feedback forgets callees and keys that died", () => {
    function call(f) {
        return f();
    }
    function get(object, key) {
        return object[key];
    }
    jit.prepare(call);
    jit.prepare(get);
    (() => {
        call(function temporary() {});
        get({}, Symbol("temporary"));
    })();
    expect(feedbackOf(call, "Call")).toBe("value#0: Undefined call#0: target=function temporary flags=none");
    expect(feedbackOf(get, "GetByValue")).toBe(
        "value#0: Undefined keyed#0: keys=Symbol elements=Other last_key=Symbol(temporary)"
    );
    gc();
    expect(feedbackOf(call, "Call")).toBe("value#0: Undefined call#0: target=none flags=none");
    expect(feedbackOf(get, "GetByValue")).toBe("value#0: Undefined keyed#0: keys=Symbol elements=Other");
});

test("functions that never ran have no feedback", () => {
    function neverCalled() {
        return 1;
    }
    expect(jit.feedback(neverCalled)).toBeUndefined();
    expect(() => jit.feedback(Math.max)).toThrowWithMessage(TypeError, "Not an ECMAScript function");
});
