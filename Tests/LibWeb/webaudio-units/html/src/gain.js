export function createGainProcessor(log, ui) {
    const { h, createCheckboxRow, createRangeNumberRow, clampNumber, safeDisconnect } = ui;

    let ctx = null;
    let node = null;

    let bypass = false;
    let rememberedGain = 0.75;

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

    const gainRow = createRangeNumberRow({
        label: "Gain",
        // Use integer slider values to avoid float/step bugs in range inputs.
        range: { min: "0", max: "1000", value: "750", step: "5" },
        number: { min: "0", max: "1", value: "0.75", step: "0.005" },
        map: {
            rangeToNumber: v => {
                const milli = Number(v);
                const g = Number.isFinite(milli) ? clampNumber(milli / 1000, 0, 1) : 0.75;
                return g.toFixed(3);
            },
            numberToRange: v => {
                const n = Number(v);
                const g = Number.isFinite(n) ? clampNumber(n, 0, 1) : 0.75;
                return String(Math.round(g * 1000));
            },
        },
        onChange: () => {
            const v = Number(gainRow.numberEl.value);
            rememberedGain = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.75;
            apply();
        },
    });

    uiRoot.append(bypassRow.row);
    uiRoot.append(gainRow.row);

    function ensureAudio(context) {
        if (node) return;
        ctx = context;
        node = ctx.createGain();
        apply();
    }

    function apply() {
        if (!node) return;
        const g = bypass ? 1 : rememberedGain;
        try {
            node.gain.value = g;
        } catch (e) {
            log(`gain.gain set failed: ${e}`);
        }

        gainRow.rangeEl.disabled = bypass;
        gainRow.numberEl.disabled = bypass;
    }

    return {
        kind: "gain",
        title: "GainNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return { bypass, gain: rememberedGain };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            if (state.gain !== undefined) rememberedGain = clampNumber(Number(state.gain), 0, 1);
            gainRow.numberEl.value = rememberedGain.toFixed(3);
            gainRow.numberEl.dispatchEvent(new Event("input"));
            bypassRow.cb.checked = bypass;
            apply();
        },
        disconnectAll() {
            safeDisconnect(node);
        },
        audioNode() {
            if (!node) throw new Error("GainNode missing");
            return node;
        },
        outputNode() {
            if (!node) throw new Error("GainNode missing");
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
