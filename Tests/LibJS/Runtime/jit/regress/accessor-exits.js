// Exits inside getters and setters that compiled code inlined into a GetById or PutById must rebuild the accessor's
// frame and the frame of the access, so the interpreter finishes the access with the right receiver and value.

test("exits in inlined getters", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            class Box {
                constructor(value) {
                    this.raw = value;
                }
                get doubled() {
                    return this.raw * 2;
                }
            }
            return value => {
                const box = new Box(value);
                return [box.doubled + 1, box.raw];
            };
        },
        {
            warmUp: (log, i) => [i],
            probes: {
                double: () => [0.5],
                string: () => ["4"],
                overflow: () => [2147483647],
                undefined: () => [undefined],
            },
        }
    );
    expect(failures).toEqual([]);
});

test("exits in inlined setters", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const prototype = {
                set value(v) {
                    if (v > 100) throw new RangeError(`too big: ${v}`);
                    this.stored = v + 1;
                },
            };
            return (value, log) => {
                const object = Object.create(prototype);
                try {
                    object.value = value;
                } catch (e) {
                    log.push(e.message);
                }
                return object.stored;
            };
        },
        {
            warmUp: (log, i) => [i, log],
            probes: { throwing: log => [1000, log], double: log => [1.25, log], string: log => ["9", log] },
        }
    );
    expect(failures).toEqual([]);
});
