export function createConstantSource(log, ui) {
    const { h, createRangeNumberRow, clampNumber, safeDisconnect } = ui;

    let ctx = null;
    let src = null;

    const uiRoot = h("div", null);

    const offsetRow = createRangeNumberRow({
        label: "Offset",
        range: { min: "-100", max: "100", value: "0", step: "1" },
        number: { min: "-1", max: "1", value: "0", step: "0.01" },
        onChange: () => {
            if (!src) return;
            const v = Number(offsetRow.numberEl.value);
            const off = Number.isFinite(v) ? clampNumber(v, -1, 1) : 0;
            try {
                src.offset.value = off;
            } catch (e) {
                log(`offset set failed: ${e}`);
            }
        },
    });
    offsetRow.rangeEl.addEventListener("input", () => {
        const pct = Number(offsetRow.rangeEl.value);
        if (Number.isFinite(pct)) offsetRow.numberEl.value = (pct / 100).toFixed(2);
        offsetRow.numberEl.dispatchEvent(new Event("input"));
    });
    offsetRow.numberEl.addEventListener("input", () => {
        const v = Number(offsetRow.numberEl.value);
        const off = Number.isFinite(v) ? clampNumber(v, -1, 1) : 0;
        offsetRow.rangeEl.value = String(Math.round(off * 100));
    });

    uiRoot.append(offsetRow.row);

    function ensureAudio(context) {
        if (src) return;
        ctx = context;
        src = ctx.createConstantSource();

        try {
            src.offset.value = clampNumber(Number(offsetRow.numberEl.value), -1, 1);
        } catch (_) {}

        try {
            src.start();
        } catch (e) {
            log(`constantSource.start failed: ${e}`);
        }
    }

    return {
        kind: "constantSource",
        title: "ConstantSourceNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return { offset: offsetRow.numberEl.value };
        },
        applyState(state) {
            if (!state) return;
            if (state.offset !== undefined) offsetRow.numberEl.value = String(state.offset);
            offsetRow.numberEl.dispatchEvent(new Event("input"));
        },
        disconnectAll() {
            if (src) safeDisconnect(src);
        },
        outputNode() {
            return src;
        },
        teardownAudio() {
            safeDisconnect(src);
            try {
                if (src) src.stop();
            } catch (_) {}
            src = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
