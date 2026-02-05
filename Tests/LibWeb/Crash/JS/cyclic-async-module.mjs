(async function () {
    const first = import("./cyclic-async-module-a.mjs");
    const second = import("./cyclic-async-module-b.mjs");

    await first;
    await second;
})();
