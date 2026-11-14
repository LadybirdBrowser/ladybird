// REQUIRES: inline-allocation
// A concatenation that is the key of a keyed access looks for the string that it made before, which the caches of the
// access know. Other concatenations make rope strings.

function read(object, name) {
    return object["on" + name];
}
jit.prepare(read);
const handlers = { onchange: 1, onkeydown: 2 };
for (let i = 0; i < 10; ++i) read(handlers, i & 1 ? "change" : "keydown");
jit.compile(read);

// CHECK-FUNCTION: read
// CHECK-PASS: branch fusion
// CHECK: [[KEY:v[0-9]+]] = ConcatenateStrings
// CHECK-PASS: key strings
// CHECK-NOT: ConcatenateStrings
// CHECK: [[KEY]] = ConcatenateKeyStrings
// CHECK: ProbeKeyedCache

// The key is a local variable that a loop assigns, and the store and the `in` take it too.
function count(object, names) {
    let key = "";
    let found = 0;
    for (let i = 0; i < names.length; ++i) {
        key = "data-" + names[i];
        if (key in object) ++found;
        object[key] = i;
    }
    return found;
}
jit.prepare(count);
for (let i = 0; i < 10; ++i) count({}, ["first", "second", "first"]);
jit.compile(count);

// CHECK-FUNCTION: count
// CHECK-PASS: key strings
// CHECK-COUNT-1: ConcatenateKeyStrings
// CHECK-COUNT-0: = ConcatenateStrings

// The strings of a key that are concatenations themselves look for their strings too, like those of a template literal.
function readEvent(object, name) {
    return object[`handle_${name}_event`] + object["before_" + name + "_event"];
}
jit.prepare(readEvent);
for (let i = 0; i < 10; ++i) readEvent({ handle_change_event: 1 }, i & 1 ? "change" : "keydown");
jit.compile(readEvent);

// CHECK-FUNCTION: readEvent
// CHECK-PASS: key strings
// CHECK-COUNT-4: ConcatenateKeyStrings
// CHECK-COUNT-0: = ConcatenateStrings

// A string that a loop puts together is a key once, and its pieces are rope strings.
function lookUp(object, names) {
    let path = "";
    for (let i = 0; i < names.length; ++i) path = path + names[i];
    return object[path];
}
jit.prepare(lookUp);
for (let i = 0; i < 10; ++i) lookUp({}, ["first_piece", "second_piece", "third_piece"]);
jit.compile(lookUp);

// CHECK-FUNCTION: lookUp
// CHECK-PASS: key strings
// CHECK-COUNT-1: = ConcatenateStrings
// CHECK-COUNT-0: ConcatenateKeyStrings

// The same goes for a loop that has more than one way out.
function lookUpUntil(object, names, last) {
    let path = "";
    for (let i = 0; i < names.length; ++i) {
        path = path + names[i];
        if (i === last) break;
    }
    return object[path];
}
jit.prepare(lookUpUntil);
for (let i = 0; i < 10; ++i) lookUpUntil({}, ["first_piece", "second_piece", "third_piece"], i & 1 ? 1 : 5);
jit.compile(lookUpUntil);

// CHECK-FUNCTION: lookUpUntil
// CHECK-PASS: key strings
// CHECK-COUNT-1: = ConcatenateStrings
// CHECK-COUNT-0: ConcatenateKeyStrings

// A concatenation that is no key stays a rope string.
function join(first, second) {
    return first + second;
}
jit.prepare(join);
for (let i = 0; i < 10; ++i) join("first_half_", "second_half");
jit.compile(join);

// CHECK-FUNCTION: join
// CHECK-PASS: key strings
// CHECK-COUNT-1: = ConcatenateStrings
// CHECK-COUNT-0: ConcatenateKeyStrings
