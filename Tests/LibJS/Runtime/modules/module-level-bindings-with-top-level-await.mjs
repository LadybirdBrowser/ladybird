let counter = 0;

function increment() {
    return () => ++counter;
}

await Promise.resolve();
increment()();

{
    let blockBinding = 10;
    await Promise.resolve();
    counter += blockBinding;
}

export const counterAfterAwait = (() => counter)();

export const passed = true;
