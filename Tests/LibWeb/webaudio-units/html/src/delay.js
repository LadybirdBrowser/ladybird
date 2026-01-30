export function createDelayProcessor(log, ui) {
    const { h, createCheckboxRow, createRangeNumberRow, clampNumber, safeDisconnect } = ui;

    let ctx = null;
    let node = null;

    let bypass = false;
    let rememberedDelay = 0.05;

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

    const delayRow = createRangeNumberRow({
        label: "Delay (s)",
        range: { min: "0", max: "1000", value: "50", step: "1" },
        number: { min: "0", max: "1.0", value: "0.05", step: "0.001" },
        onChange: () => {
            const v = Number(delayRow.numberEl.value);
            rememberedDelay = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.05;
            apply();
        },
    });
    delayRow.rangeEl.addEventListener("input", () => {
        const milli = Number(delayRow.rangeEl.value);
        if (Number.isFinite(milli)) delayRow.numberEl.value = (milli / 1000).toFixed(3);
        delayRow.numberEl.dispatchEvent(new Event("input"));
    });
    delayRow.numberEl.addEventListener("input", () => {
        const v = Number(delayRow.numberEl.value);
        const d = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.05;
        delayRow.rangeEl.value = String(Math.round(d * 1000));
    });

    uiRoot.append(bypassRow.row);
    uiRoot.append(delayRow.row);

    function ensureAudio(context) {
        if (node) return;
        ctx = context;
        node = ctx.createDelay(1.0);
        apply();
    }

    function apply() {
        if (!node) return;
        const d = bypass ? 0 : rememberedDelay;
        try {
            node.delayTime.value = d;
        } catch (e) {
            log(`delay.delayTime set failed: ${e}`);
        }
        delayRow.rangeEl.disabled = bypass;
        delayRow.numberEl.disabled = bypass;
    }

    return {
        kind: "delay",
        title: "DelayNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return { bypass, delay: rememberedDelay };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            if (state.delay !== undefined) rememberedDelay = clampNumber(Number(state.delay), 0, 1);
            delayRow.numberEl.value = rememberedDelay.toFixed(3);
            delayRow.numberEl.dispatchEvent(new Event("input"));
            bypassRow.cb.checked = bypass;
            apply();
        },
        disconnectAll() {
            safeDisconnect(node);
        },
        audioNode() {
            if (!node) throw new Error("DelayNode missing");
            return node;
        },
        outputNode() {
            if (!node) throw new Error("DelayNode missing");
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
