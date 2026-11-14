// Closures that inlined callees create must be what the interpreter would have created, when code exits in them or
// after them, when they escape, and when the stack is walked through them.

function apply(f, x) {
    return f(x);
}

// The inlined callee creates a closure that captures a binding of the environment the callee was created in.
function makeScaled() {
    let factor = 2;
    function scaled(x) {
        return apply(y => y * factor, x);
    }
    return {
        scaled,
        grow() {
            factor++;
        },
    };
}

// Exits in the closure of the inlined callee once the values stop being int32.
function sumScaled(scaler, values) {
    let sum = 0;
    for (const value of values) sum += scaler.scaled(value);
    return sum;
}

// The closure of the inlined callee escapes into an array.
function collect(values) {
    const closures = [];
    for (const value of values) closures.push(wrap(value));
    return closures.map(f => f()).reduce((a, b) => a + b, 0);
}
function wrap(value) {
    return () => value;
}

// The closure of the inlined callee is held across a call that exits.
function twice(x) {
    const f = y => y + 1;
    return apply(f, x) + apply(f, x * 2);
}
function sumTwice(values) {
    let sum = 0;
    for (const value of values) sum += twice(value);
    return sum;
}

// Sloppy closures of inlined callees show their frames as the legacy caller.
function whoCalled() {
    return whoCalled.caller;
}
function sloppyInner(x) {
    return apply(function (y) {
        return [whoCalled(), whoCalled(), y];
    }, x);
}
function legacyCaller(count) {
    let same = 0;
    for (let i = 0; i < count; i++) {
        const result = sloppyInner(i);
        if (result[0] === result[1] && typeof result[0] === "function") same++;
    }
    return same;
}

// Stack traces through the frames of closures of inlined callees name the closures.
function tracedInner(x, last) {
    return apply(function named(y) {
        return y === last ? new Error().stack : "";
    }, x);
}
function traced(count) {
    let names = "";
    for (let i = 0; i < count; i++) {
        const stack = tracedInner(i, count - 1);
        if (stack)
            names = stack
                .split("\n")
                .slice(1, 4)
                .join(" | ")
                .replace(/\(.*?\)/g, "");
    }
    return names;
}

// Methods of classes with private names have a private environment, which their closures keep.
class Counter {
    #step = 3;
    scaled(x) {
        return apply(y => y * this.#step, x);
    }
}
function sumCounter(counter, values) {
    let sum = 0;
    for (const value of values) sum += counter.scaled(value);
    return sum;
}

const ints = Array.from({ length: 50 }, (_, i) => i);
const mixed = ints.concat([1.5, 2.5, "3", 4]);
const scaler = makeScaled();
const counter = new Counter();
for (let i = 0; i < 300; i++) {
    sumScaled(scaler, ints);
    collect(ints);
    sumTwice(ints);
    sumCounter(counter, ints);
}
print(sumScaled(scaler, ints), sumScaled(scaler, mixed));
scaler.grow();
print(sumScaled(scaler, ints), sumScaled(scaler, mixed));
print(collect(ints), collect(mixed));
print(sumTwice(ints), sumTwice(mixed));
print(sumCounter(counter, ints), sumCounter(counter, mixed));
for (let i = 0; i < 300; i++) legacyCaller(5);
print(legacyCaller(10));
for (let i = 0; i < 300; i++) traced(3);
print(traced(3));
