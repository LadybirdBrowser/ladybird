test("object literal with >64 properties in a loop doesn't corrupt shared dictionary shape", () => {
    let objects = [];
    for (let i = 0; i < 200; i++) {
        let obj = {
            a0: 0,
            a1: 0,
            a2: 0,
            a3: 0,
            a4: 0,
            a5: 0,
            a6: 0,
            a7: 0,
            a8: 0,
            a9: 0,
            a10: 0,
            a11: 0,
            a12: 0,
            a13: 0,
            a14: 0,
            a15: 0,
            a16: 0,
            a17: 0,
            a18: 0,
            a19: 0,
            a20: 0,
            a21: 0,
            a22: 0,
            a23: 0,
            a24: 0,
            a25: 0,
            a26: 0,
            a27: 0,
            a28: 0,
            a29: 0,
            a30: 0,
            a31: 0,
            a32: 0,
            a33: 0,
            a34: 0,
            a35: 0,
            a36: 0,
            a37: 0,
            a38: 0,
            a39: 0,
            a40: 0,
            a41: 0,
            a42: 0,
            a43: 0,
            a44: 0,
            a45: 0,
            a46: 0,
            a47: 0,
            a48: 0,
            a49: 0,
            a50: 0,
            a51: 0,
            a52: 0,
            a53: 0,
            a54: 0,
            a55: 0,
            a56: 0,
            a57: 0,
            a58: 0,
            a59: 0,
            a60: 0,
            a61: 0,
            a62: 0,
            a63: 0,
            a64: 0,
        };
        obj["x" + i] = i;
        objects.push(obj);
    }
    gc();

    for (let i = 0; i < 200; i++) {
        expect(objects[i]["x" + i]).toBe(i);
        expect(objects[i].a0).toBe(0);
        expect(objects[i].a64).toBe(0);
    }
});
