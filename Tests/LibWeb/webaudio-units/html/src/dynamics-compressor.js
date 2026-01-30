export function createDynamicsCompressorProcessor(log, ui) {
    const {
        h,
        createCheckboxRow,
        createRangeNumberRow,
        createInfoRow,
        clampNumber,
        safeConnect,
        safeDisconnect,
        setupStatusTextLoop,
    } = ui;

    let ctx = null;
    let input = null;
    let node = null;
    let output = null;

    let bypass = false;

    let rememberedThreshold = -24;
    let rememberedKnee = 30;
    let rememberedRatio = 12;
    let rememberedAttack = 0.003;
    let rememberedRelease = 0.25;

    let reductionLoop = null;

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

    const thresholdRow = createRangeNumberRow({
        label: "Threshold (dB)",
        range: { min: "-100", max: "0", value: String(rememberedThreshold), step: "0.1" },
        number: { min: "-100", max: "0", value: String(rememberedThreshold), step: "0.1" },
        onChange: () => {
            const v = Number(thresholdRow.numberEl.value);
            rememberedThreshold = Number.isFinite(v) ? clampNumber(v, -100, 0) : -24;
            apply();
        },
    });

    const kneeRow = createRangeNumberRow({
        label: "Knee (dB)",
        range: { min: "0", max: "40", value: String(rememberedKnee), step: "0.1" },
        number: { min: "0", max: "40", value: String(rememberedKnee), step: "0.1" },
        onChange: () => {
            const v = Number(kneeRow.numberEl.value);
            rememberedKnee = Number.isFinite(v) ? clampNumber(v, 0, 40) : 30;
            apply();
        },
    });

    const ratioRow = createRangeNumberRow({
        label: "Ratio",
        range: { min: "1", max: "20", value: String(rememberedRatio), step: "0.1" },
        number: { min: "1", max: "20", value: String(rememberedRatio), step: "0.1" },
        onChange: () => {
            const v = Number(ratioRow.numberEl.value);
            rememberedRatio = Number.isFinite(v) ? clampNumber(v, 1, 20) : 12;
            apply();
        },
    });

    const attackRow = createRangeNumberRow({
        label: "Attack (s)",
        range: { min: "0", max: "1000", value: "3", step: "1" },
        number: { min: "0", max: "1", value: String(rememberedAttack), step: "0.001" },
        onChange: () => {
            const v = Number(attackRow.numberEl.value);
            rememberedAttack = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.003;
            apply();
        },
    });
    attackRow.rangeEl.addEventListener("input", () => {
        const milli = Number(attackRow.rangeEl.value);
        if (Number.isFinite(milli)) attackRow.numberEl.value = (milli / 1000).toFixed(3);
        attackRow.numberEl.dispatchEvent(new Event("input"));
    });
    attackRow.numberEl.addEventListener("input", () => {
        const v = Number(attackRow.numberEl.value);
        const a = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.003;
        attackRow.rangeEl.value = String(Math.round(a * 1000));
    });

    const releaseRow = createRangeNumberRow({
        label: "Release (s)",
        range: { min: "0", max: "1000", value: "250", step: "1" },
        number: { min: "0", max: "1", value: String(rememberedRelease), step: "0.001" },
        onChange: () => {
            const v = Number(releaseRow.numberEl.value);
            rememberedRelease = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.25;
            apply();
        },
    });
    releaseRow.rangeEl.addEventListener("input", () => {
        const milli = Number(releaseRow.rangeEl.value);
        if (Number.isFinite(milli)) releaseRow.numberEl.value = (milli / 1000).toFixed(3);
        releaseRow.numberEl.dispatchEvent(new Event("input"));
    });
    releaseRow.numberEl.addEventListener("input", () => {
        const v = Number(releaseRow.numberEl.value);
        const r = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.25;
        releaseRow.rangeEl.value = String(Math.round(r * 1000));
    });

    const reductionRow = createInfoRow("Reduction (dB)");

    uiRoot.append(bypassRow.row);
    uiRoot.append(thresholdRow.row);
    uiRoot.append(kneeRow.row);
    uiRoot.append(ratioRow.row);
    uiRoot.append(attackRow.row);
    uiRoot.append(releaseRow.row);
    uiRoot.append(reductionRow.row);

    function ensureAudio(context) {
        if (node) return;
        ctx = context;

        input = ctx.createGain();
        output = ctx.createGain();

        try {
            node = ctx.createDynamicsCompressor();
        } catch (e) {
            node = null;
            log(`createDynamicsCompressor failed: ${e}`);
            return;
        }

        reductionLoop = setupStatusTextLoop({
            target: reductionRow.value,
            intervalMs: 200,
            text: () => {
                if (!node) return "-";
                try {
                    const r = Number(node.reduction);
                    if (!Number.isFinite(r)) return "-";
                    return r.toFixed(2);
                } catch (_) {
                    return "-";
                }
            },
        });
        reductionLoop.start();

        apply();
    }

    function apply() {
        if (!input || !output || !node) return;

        try {
            input.disconnect();
        } catch (_) {}
        try {
            node.disconnect();
        } catch (_) {}

        if (bypass) {
            safeConnect(input, output);
        } else {
            safeConnect(input, node);
            safeConnect(node, output);
        }

        const t = Number.isFinite(rememberedThreshold) ? clampNumber(rememberedThreshold, -100, 0) : -24;
        const k = Number.isFinite(rememberedKnee) ? clampNumber(rememberedKnee, 0, 40) : 30;
        const r = Number.isFinite(rememberedRatio) ? clampNumber(rememberedRatio, 1, 20) : 12;
        const a = Number.isFinite(rememberedAttack) ? clampNumber(rememberedAttack, 0, 1) : 0.003;
        const rel = Number.isFinite(rememberedRelease) ? clampNumber(rememberedRelease, 0, 1) : 0.25;

        try {
            node.threshold.value = t;
        } catch (e) {
            log(`compressor.threshold set failed: ${e}`);
        }
        try {
            node.knee.value = k;
        } catch (e) {
            log(`compressor.knee set failed: ${e}`);
        }
        try {
            node.ratio.value = r;
        } catch (e) {
            log(`compressor.ratio set failed: ${e}`);
        }
        try {
            node.attack.value = a;
        } catch (e) {
            log(`compressor.attack set failed: ${e}`);
        }
        try {
            node.release.value = rel;
        } catch (e) {
            log(`compressor.release set failed: ${e}`);
        }

        thresholdRow.rangeEl.disabled = bypass;
        thresholdRow.numberEl.disabled = bypass;
        kneeRow.rangeEl.disabled = bypass;
        kneeRow.numberEl.disabled = bypass;
        ratioRow.rangeEl.disabled = bypass;
        ratioRow.numberEl.disabled = bypass;
        attackRow.rangeEl.disabled = bypass;
        attackRow.numberEl.disabled = bypass;
        releaseRow.rangeEl.disabled = bypass;
        releaseRow.numberEl.disabled = bypass;
    }

    return {
        kind: "dynamicsCompressor",
        title: "DynamicsCompressorNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                bypass,
                threshold: rememberedThreshold,
                knee: rememberedKnee,
                ratio: rememberedRatio,
                attack: rememberedAttack,
                release: rememberedRelease,
            };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            if (state.threshold !== undefined) rememberedThreshold = clampNumber(Number(state.threshold), -100, 0);
            if (state.knee !== undefined) rememberedKnee = clampNumber(Number(state.knee), 0, 40);
            if (state.ratio !== undefined) rememberedRatio = clampNumber(Number(state.ratio), 1, 20);
            if (state.attack !== undefined) rememberedAttack = clampNumber(Number(state.attack), 0, 1);
            if (state.release !== undefined) rememberedRelease = clampNumber(Number(state.release), 0, 1);

            thresholdRow.numberEl.value = String(rememberedThreshold);
            kneeRow.numberEl.value = String(rememberedKnee);
            ratioRow.numberEl.value = String(rememberedRatio);
            attackRow.numberEl.value = String(rememberedAttack);
            releaseRow.numberEl.value = String(rememberedRelease);

            thresholdRow.numberEl.dispatchEvent(new Event("input"));
            kneeRow.numberEl.dispatchEvent(new Event("input"));
            ratioRow.numberEl.dispatchEvent(new Event("input"));
            attackRow.numberEl.dispatchEvent(new Event("input"));
            releaseRow.numberEl.dispatchEvent(new Event("input"));

            bypassRow.cb.checked = bypass;
            apply();
        },
        disconnectAll() {
            // Keep internal input->compressor->output wiring; just detach external output.
            safeDisconnect(output);
        },
        audioNode() {
            if (!input) throw new Error("DynamicsCompressorNode input missing");
            return input;
        },
        outputNode() {
            if (!output) throw new Error("DynamicsCompressorNode output missing");
            return output;
        },
        teardownAudio() {
            if (reductionLoop) reductionLoop.stop();
            reductionLoop = null;
            safeDisconnect(input);
            safeDisconnect(node);
            safeDisconnect(output);
            input = null;
            node = null;
            output = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
