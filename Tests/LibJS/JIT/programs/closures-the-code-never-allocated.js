// Closures that compiled code never allocates, because nothing but the frames of inlined calls of them refers to them,
// must be created as they were when code exits in them, or after them, or walks the stack through them.

function apply(f, x) {
    return f(x);
}

// Exits in the inlined closure, which reads a binding it captured, once the values stop being int32.
function capturing(values) {
    let offset = 1;
    let sum = 0;
    for (const value of values) {
        sum += apply(x => x * 2 + offset, value);
        offset++;
    }
    return sum;
}

// Exits with the closure in a local slot, before it is called again.
function heldInLocal(values) {
    let sum = 0;
    for (const value of values) {
        const f = x => x + 1;
        sum += apply(f, value);
        sum += value * 3;
        sum += apply(f, value);
    }
    return sum;
}

// Exits in a closure created in a method of a class, which has a private environment.
class Counter {
    #step = 2;
    total(values) {
        let sum = 0;
        for (const value of values) sum += apply(x => x * this.#step, value);
        return sum;
    }
}

// Sloppy functions show the frames of inlined calls of them as the legacy caller of the functions they call.
function whoCalled() {
    return whoCalled.caller;
}
function legacyCaller(count) {
    let same = 0;
    for (let i = 0; i < count; i++) {
        const caller = apply(function (x) {
            return [whoCalled(), whoCalled(), x];
        }, i);
        if (caller[0] === caller[1] && typeof caller[0] === "function") same++;
    }
    return same;
}

// Sloppy arrow functions are legacy callers too, which must stay the same object across the calls they make.
function countCalls() {
    const caller = countCalls.caller;
    caller.calls = (caller.calls | 0) + 1;
    // NB: Too large to inline, so that it runs without the frames of inlined calls.
    let padding = 0;
    for (let i = 0; i < 3; i++) padding += i * 2 + 1;
    for (let i = 0; i < 3; i++) padding -= i * 2 + 1;
    for (let i = 0; i < 3; i++) padding += i % 2;
    for (let i = 0; i < 3; i++) padding -= i % 2;
    return caller.calls + padding;
}
function arrowCaller() {
    const arrow = () => countCalls();
    apply(arrow, 0);
    return apply(arrow, 0);
}

// Stack traces through the frames of inlined calls name the closures.
function traced(count) {
    let names = "";
    for (let i = 0; i < count; i++) {
        const stack = apply(function named(x) {
            return x === count - 1 ? new Error().stack : "";
        }, i);
        if (stack)
            names = stack
                .split("\n")
                .slice(1, 3)
                .join(" | ")
                .replace(/\(.*?\)/g, "");
    }
    return names;
}

const ints = Array.from({ length: 50 }, (_, i) => i);
const mixed = ints.concat([1.5, 2.5, "3", 4]);
for (let i = 0; i < 300; i++) {
    capturing(ints);
    heldInLocal(ints);
    new Counter().total(ints);
}
print(capturing(ints), capturing(mixed));
print(heldInLocal(ints), heldInLocal(mixed));
print(new Counter().total(ints), new Counter().total(mixed));
for (let i = 0; i < 300; i++) legacyCaller(5);
print(legacyCaller(10));
let arrowCalls = 0;
for (let i = 0; i < 300; i++) arrowCalls += arrowCaller();
print(arrowCalls);
for (let i = 0; i < 300; i++) traced(3);
print(traced(3));
