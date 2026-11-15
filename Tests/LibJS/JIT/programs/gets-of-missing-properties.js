// Gets of properties that objects of a few shapes do not have give undefined, until the property appears on the
// objects, their prototypes or a prototype that replaces theirs, or the objects are of a class that has properties
// outside its shape.
const proto = {};
function make(i) {
    switch (i % 4) {
        case 0:
            return Object.create(proto);
        case 1: {
            const o = Object.create(proto);
            o.a = i;
            return o;
        }
        case 2:
            return [i];
        default:
            return new String("s" + i);
    }
}

function read(o) {
    return o.missing === undefined ? "u" : String(o.missing);
}

const objects = [];
for (let i = 0; i < 8; i++) objects.push(make(i));
const out = [];
for (let round = 0; round < 400; round++) {
    if (round === 100) proto.missing = "proto";
    if (round === 150) delete proto.missing;
    if (round === 200) objects[1].missing = "own";
    if (round === 250) Object.setPrototypeOf(objects[4], { missing: "replaced" });
    if (round === 300) Array.prototype.missing = "array";
    if (round === 350) Object.defineProperty(Object.prototype, "missing", { get: () => "getter", configurable: true });
    let s = "";
    for (const o of objects) s += read(o);
    out.push(s);
}
delete Array.prototype.missing;
delete Object.prototype.missing;
print(out[0], out[120], out[180], out[220], out[260], out[320], out[399]);
