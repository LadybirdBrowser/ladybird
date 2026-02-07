export function createStereoPannerProcessor(log, ui) {
    const { h, createCheckboxRow, createRangeNumberRow, clampNumber, safeDisconnect } = ui;

    /** @type {AudioContext|null} */
    let ctx = null;
    /** @type {StereoPannerNode|null} */
    let node = null;

    let bypass = false;
    let rememberedPan = 0;

    const uiRoot = h("div", null);

    const bypassRow = createCheckboxRow({
        label: "Bypass",
        checked: false,
        onChange: checked => {
            bypass = checked;
            apply();
        },
        hint: "",
    });

    const panRow = createRangeNumberRow({
        label: "Pan",
        range: { min: "-100", max: "100", value: "0", step: "1" },
        number: { min: "-1", max: "1", value: "0", step: "0.01" },
        onChange: () => {
            const v = Number(panRow.numberEl.value);
            rememberedPan = Number.isFinite(v) ? clampNumber(v, -1, 1) : 0;
            apply();
        },
    });
    panRow.rangeEl.addEventListener("input", () => {
        const pct = Number(panRow.rangeEl.value);
        if (Number.isFinite(pct)) panRow.numberEl.value = (pct / 100).toFixed(2);
        panRow.numberEl.dispatchEvent(new Event("input"));
    });
    panRow.numberEl.addEventListener("input", () => {
        const v = Number(panRow.numberEl.value);
        const p = Number.isFinite(v) ? clampNumber(v, -1, 1) : 0;
        panRow.rangeEl.value = String(Math.round(p * 100));
    });

    uiRoot.append(bypassRow.row);
    uiRoot.append(panRow.row);

    function ensureAudio(context) {
        if (node) return;
        ctx = context;
        try {
            node = ctx.createStereoPanner();
        } catch (e) {
            node = null;
            log(`createStereoPanner failed: ${e}`);
            return;
        }
        apply();
    }

    function apply() {
        if (!node) return;
        const p = bypass ? 0 : rememberedPan;
        try {
            node.pan.value = p;
        } catch (e) {
            log(`panner.pan set failed: ${e}`);
        }
        panRow.rangeEl.disabled = bypass;
        panRow.numberEl.disabled = bypass;
    }

    return {
        kind: "stereoPanner",
        title: "StereoPannerNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        disconnectAll() {
            safeDisconnect(node);
        },
        audioNode() {
            if (!node) throw new Error("StereoPannerNode missing");
            return node;
        },
        outputNode() {
            if (!node) throw new Error("StereoPannerNode missing");
            return node;
        },
        teardownAudio() {
            safeDisconnect(node);
            node = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
