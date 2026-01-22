test('Do not throw syntax error when "await" is used in an arrow function definition assigned to a default function parameter', async () => {
    async function f(
        g = async () => {
            await 1;
        }
    ) {
        return await g();
    }

    expect(await f()).toBe(1);
});

test('Do not throw syntax error when "await" is used in a function definition assigned to a default function parameter', async () => {
    async function f(
        g = async function () {
            await 1;
        }
    ) {
        return await g();
    }

    expect(await f()).toBe(1);
});
