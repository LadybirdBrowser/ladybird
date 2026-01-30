export function createWaveShaperProcessor(log, ui) {
    const { h, createCheckboxRow, createSelectRow, createRangeNumberRow, clampNumber, safeConnect, safeDisconnect } =
        ui;

    let ctx = null;
    let input = null;
    let shaper = null;
    let output = null;
    let bypass = false;
    let rememberedCurve = "soft";
    let rememberedAmount = 25;
    let rememberedOversample = "none";

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

    const curveRow = createSelectRow({
        label: "Curve",
        optionsList: [
            { value: "soft", text: "soft", selected: true },
            { value: "hard", text: "hard" },
            { value: "sine", text: "sine" },
            { value: "none", text: "none" },
        ],
        onChange: v => {
            rememberedCurve = v;
            apply();
        },
    });

    const amountRow = createRangeNumberRow({
        label: "Amount",
        range: { min: "0", max: "100", value: String(rememberedAmount), step: "1" },
        number: { min: "0", max: "100", value: String(rememberedAmount), step: "1" },
        onChange: () => {
            const v = Number(amountRow.numberEl.value);
            rememberedAmount = Number.isFinite(v) ? v : 25;
            apply();
        },
    });

    const oversampleRow = createSelectRow({
        label: "Oversample",
        optionsList: [
            { value: "none", text: "none", selected: true },
            { value: "2x", text: "2x" },
            { value: "4x", text: "4x" },
        ],
        onChange: v => {
            rememberedOversample = v;
            apply();
        },
    });

    uiRoot.append(bypassRow.row);
    uiRoot.append(curveRow.row);
    uiRoot.append(amountRow.row);
    uiRoot.append(oversampleRow.row);

    function buildCurve(kind, amount, samples) {
        const n = samples || 1024;
        const curve = new Float32Array(n);
        const k = clampNumber(amount, 0, 100) / 100;
        for (let i = 0; i < n; i++) {
            const x = (i / (n - 1)) * 2 - 1;
            let y = x;
            if (kind === "soft") {
                const drive = 0.25 + k * 4.0;
                y = Math.tanh(drive * x);
            } else if (kind === "hard") {
                const drive = 1.0 + k * 10.0;
                y = Math.max(-1, Math.min(1, x * drive));
            } else if (kind === "sine") {
                const mix = k;
                y = (1 - mix) * x + mix * Math.sin((Math.PI / 2) * x);
            }
            curve[i] = y;
        }
        return curve;
    }

    function ensureAudio(context) {
        if (shaper) return;
        ctx = context;

        input = ctx.createGain();
        output = ctx.createGain();

        try {
            shaper = ctx.createWaveShaper();
        } catch (e) {
            shaper = null;
            log(`createWaveShaper failed: ${e}`);
            return;
        }

        apply();
    }

    function apply() {
        if (!input || !output || !shaper) return;

        try {
            input.disconnect();
        } catch (_) {}
        try {
            shaper.disconnect();
        } catch (_) {}

        if (bypass) {
            safeConnect(input, output);
        } else {
            safeConnect(input, shaper);
            safeConnect(shaper, output);
        }

        try {
            shaper.oversample = rememberedOversample;
        } catch (e) {
            log(`waveShaper.oversample set failed: ${e}`);
        }

        try {
            if (rememberedCurve === "none") shaper.curve = null;
            else shaper.curve = buildCurve(rememberedCurve, rememberedAmount, 2048);
        } catch (e) {
            log(`waveShaper.curve set failed: ${e}`);
        }

        curveRow.select.disabled = bypass;
        amountRow.rangeEl.disabled = bypass;
        amountRow.numberEl.disabled = bypass;
        oversampleRow.select.disabled = bypass;
    }

    return {
        kind: "waveShaper",
        title: "WaveShaperNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                bypass,
                curve: rememberedCurve,
                amount: rememberedAmount,
                oversample: rememberedOversample,
            };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            rememberedCurve = state.curve || rememberedCurve;
            if (state.amount !== undefined) rememberedAmount = Number(state.amount);
            rememberedOversample = state.oversample || rememberedOversample;

            curveRow.select.value = rememberedCurve;
            curveRow.select.dispatchEvent(new Event("change"));

            amountRow.numberEl.value = String(rememberedAmount);
            amountRow.numberEl.dispatchEvent(new Event("input"));

            oversampleRow.select.value = rememberedOversample;
            oversampleRow.select.dispatchEvent(new Event("change"));

            bypassRow.cb.checked = bypass;
            apply();
        },
        disconnectAll() {
            safeDisconnect(output);
        },
        audioNode() {
            if (!input) throw new Error("WaveShaperNode input missing");
            return input;
        },
        outputNode() {
            if (!output) throw new Error("WaveShaperNode output missing");
            return output;
        },
        teardownAudio() {
            safeDisconnect(input);
            safeDisconnect(shaper);
            safeDisconnect(output);
            input = null;
            shaper = null;
            output = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
