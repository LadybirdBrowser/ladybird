// Collections that move cells move array buffers, typed arrays and data views, whose bytes stay where they are.

const holder = {
    buffer: new ArrayBuffer(16),
    resizable: new ArrayBuffer(8, { maxByteLength: 32 }),
};
holder.bytes = new Uint8Array(holder.buffer);
holder.words = new Uint32Array(holder.buffer, 4, 2);
holder.view = new DataView(holder.buffer);
holder.tracking = new Uint8Array(holder.resizable);
holder.floats = new Float64Array([1.5, 2.5]);
holder.bytes.set([1, 2, 3, 4, 5, 6, 7, 8]);
holder.tracking.fill(7);

function sumOfBytes(bytes) {
    let sum = 0;
    for (let i = 0; i < bytes.length; ++i) sum += bytes[i];
    return sum;
}
for (let i = 0; i < 100; ++i) sumOfBytes(holder.bytes);

// NB: test-js runs each test more than once with LIBJS_TEST_REPEAT, but the cells moved once, so what a test cannot undo
//     happens once, and every run checks what it saw.
function once(steps) {
    let result;
    let ran = false;
    return () => {
        if (!ran) {
            result = steps();
            ran = true;
        }
        return result;
    };
}

const addresses = {
    buffer: addressOf(holder.buffer),
    bytes: addressOf(holder.bytes),
    view: addressOf(holder.view),
};

runAfterMovingCollection(() => {
    test("array buffers, typed arrays and data views move", () => {
        expect(addressOf(holder.buffer)).not.toBe(addresses.buffer);
        expect(addressOf(holder.bytes)).not.toBe(addresses.bytes);
        expect(addressOf(holder.view)).not.toBe(addresses.view);
    });

    const sumOfBytesBeforeWrites = once(() => {
        const sum = sumOfBytes(holder.bytes);
        holder.view.setUint8(15, 9);
        holder.words[0] = 0;
        return sum;
    });
    test("views of a buffer that moved share its bytes", () => {
        expect(holder.bytes.buffer).toBe(holder.buffer);
        expect(holder.words.buffer).toBe(holder.buffer);
        expect(holder.view.buffer).toBe(holder.buffer);
        expect(sumOfBytesBeforeWrites()).toBe(36);
        expect(holder.bytes[15]).toBe(9);
        expect(Array.from(holder.bytes.subarray(4, 8))).toEqual([0, 0, 0, 0]);
        expect(holder.floats[1]).toBe(2.5);
    });

    const grownTracking = once(() => {
        holder.resizable.resize(16);
        const grown = Array.from(holder.tracking);
        holder.resizable.resize(4);
        return grown;
    });
    test("views that track a resizable buffer that moved follow its resizes", () => {
        const grown = grownTracking();
        expect(grown.length).toBe(16);
        expect(grown[7]).toBe(7);
        expect(grown[8]).toBe(0);
        expect(holder.tracking.length).toBe(4);
    });

    const transfer = once(() => holder.buffer.transfer());
    test("a buffer that moved detaches its views when transferred", () => {
        const transferred = transfer();
        expect(holder.buffer.detached).toBeTrue();
        expect(holder.bytes.length).toBe(0);
        expect(new Uint8Array(transferred)[15]).toBe(9);
    });
});
