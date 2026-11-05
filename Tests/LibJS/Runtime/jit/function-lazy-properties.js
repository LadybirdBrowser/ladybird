const triggers = {
    defineName: object => Object.defineProperty(object, "name", { value: "renamed" }),
    deleteLength: object => delete object.length,
    becomePrototype: object => Object.setPrototypeOf({}, object),
    ownKeys: object => Reflect.ownKeys(object),
    getOwnName: object => Object.getOwnPropertyDescriptor(object, "name"),
};

const factories = {
    normal: () => new Function(""),
    arrow: () => () => {},
    async: () => async function () {},
};

for (const [kind, make] of Object.entries(factories)) {
    for (const [name, trigger] of Object.entries(triggers)) {
        test(`${kind} function reifies lazy properties during a call: ${name}`, () => {
            const victim = make();
            // Give each test a fresh shape, so earlier tests cannot have made it unstable.
            victim[Symbol()] = 0;
            victim.a = 10;
            victim.b = 20;
            let armed = false;
            const poke = object => {
                if (armed) trigger(object);
            };
            const read = new Function(
                "poke",
                "object",
                "const first = object.b; poke(object); return first + object.b;"
            );
            jit.prepare(read);
            for (let i = 0; i < 50; ++i) read(poke, victim);
            expect(jit.compile(read)).toBe(jit.enabled);

            armed = true;
            expect(read(poke, victim)).toBe(40);
            expect(victim.a).toBe(10);
            expect(victim.b).toBe(20);
            expect(read(poke, victim)).toBe(40);
        });
    }
}
