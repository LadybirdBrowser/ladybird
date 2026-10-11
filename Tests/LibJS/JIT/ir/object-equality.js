// Loose equality whose feedback only saw pairs of objects checks both operands and compares their identities.

const object = {};
const other = {};

function equal(a, b) {
    return a == b;
}
jit.prepare(equal);
for (let i = 0; i < 10; ++i) {
    equal(object, object);
    equal(object, other);
}
jit.compile(equal);
if (!equal(object, object) || equal(object, other)) throw new Error("wrong equality");

// CHECK-FUNCTION: equal
// CHECK-PASS: frame initialization
// CHECK: CheckObject ([[A:v[0-9]+]])
// CHECK: CheckObject ([[B:v[0-9]+]])
// CHECK: TaggedEquals ([[A]], [[B]])
// CHECK-NOT: CallSlowPath

function unequal(a, b) {
    return a != b;
}
jit.prepare(unequal);
for (let i = 0; i < 10; ++i) {
    unequal(object, object);
    unequal(object, other);
}
jit.compile(unequal);
if (unequal(object, object) || !unequal(object, other)) throw new Error("wrong inequality");

// CHECK-FUNCTION: unequal
// CHECK-PASS: frame initialization
// CHECK: CheckObject ([[A:v[0-9]+]])
// CHECK: CheckObject ([[B:v[0-9]+]])
// CHECK: TaggedNotEquals ([[A]], [[B]])
// CHECK-NOT: CallSlowPath

// The graph-node lookup loop branches directly on object identity.

function find(nodes, target) {
    for (let i = 0; i < nodes.length; ++i) {
        if (nodes[i].pos == target.pos) return i;
    }
    return -1;
}
const nodes = [{ pos: object }, { pos: other }];
jit.prepare(find);
for (let i = 0; i < 10; ++i) find(nodes, nodes[1]);
jit.compile(find);
if (find(nodes, nodes[1]) !== 1 || find(nodes, { pos: {} }) !== -1) throw new Error("wrong lookup");

// CHECK-FUNCTION: find
// CHECK-PASS: frame initialization
// CHECK: [[TARGET:v[0-9]+]] = LoadNamed
// CHECK: loop:
// CHECK: [[POSITION:v[0-9]+]] = LoadNamed
// CHECK: BranchTaggedEquals ([[POSITION]], [[TARGET]])
// CHECK-NOT: CallSlowPath JumpLooselyEquals

// Feedback from objects must not hide earlier or later comparisons of other types.

function mixed(a, b) {
    return a == b;
}
jit.prepare(mixed);
for (let i = 0; i < 10; ++i) {
    mixed(object, other);
    mixed(null, undefined);
}
jit.compile(mixed);
if (!mixed(null, undefined) || mixed(object, other)) throw new Error("wrong mixed equality");

// CHECK-FUNCTION: mixed
// CHECK-PASS: frame initialization
// CHECK-NOT: CheckObject
// CHECK: CallSlowPath LooselyEquals
