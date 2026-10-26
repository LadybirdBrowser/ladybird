export const ordersAfterAwait = [];

for (const usePromise of [false, true]) {
    const order = [];
    Promise.resolve()
        .then(() => order.push("tick 1"))
        .then(() => order.push("tick 2"))
        .then(() => order.push("tick 3"))
        .then(() => order.push("tick 4"));

    for (let i = 1; i <= 4; ++i) {
        await (usePromise ? Promise.resolve(i) : i);
        order.push(`await ${i}`);
    }
    ordersAfterAwait.push(order);
}

export const orderBeforeAwait = [];
Promise.resolve()
    .then(() => orderBeforeAwait.push("tick 1"))
    .then(() => orderBeforeAwait.push("tick 2"))
    .then(() => orderBeforeAwait.push("tick 3"))
    .then(() => orderBeforeAwait.push("tick 4"));

for (let i = 1; i <= 4; ++i) {
    await orderBeforeAwait.push(`await ${i}`);
}

export const passed = true;
