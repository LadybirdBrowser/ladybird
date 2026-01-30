export function createConvolverProcessor(log, ui) {
    const { h, createCheckboxRow, createSelectRow, createRangeNumberRow, safeConnect, safeDisconnect } = ui;

    let ctx = null;
    let input = null;
    let convolver = null;
    let output = null;

    let bypass = false;
    let rememberedType = "noise";
    let rememberedDuration = 1.0;
    let rememberedDecay = 2.0;
    let rememberedNormalize = true;

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

    const typeRow = createSelectRow({
        label: "Impulse",
        optionsList: [
            { value: "noise", text: "noise", selected: true },
            { value: "reverse-noise", text: "reverse noise" },
            { value: "impulse", text: "single impulse" },
        ],
        onChange: v => {
            rememberedType = v;
            apply();
        },
    });

    const durationRow = createRangeNumberRow({
        label: "Duration (s)",
        range: { min: "0.1", max: "5.0", value: String(rememberedDuration), step: "0.1" },
        number: { min: "0.1", max: "10.0", value: String(rememberedDuration), step: "0.1" },
        onChange: () => {
            const v = Number(durationRow.numberEl.value);
            rememberedDuration = Number.isFinite(v) ? v : 1.0;
            apply();
        },
    });

    const decayRow = createRangeNumberRow({
        label: "Decay",
        range: { min: "0.1", max: "10.0", value: String(rememberedDecay), step: "0.1" },
        number: { min: "0.1", max: "20.0", value: String(rememberedDecay), step: "0.1" },
        onChange: () => {
            const v = Number(decayRow.numberEl.value);
            rememberedDecay = Number.isFinite(v) ? v : 2.0;
            apply();
        },
    });

    const normalizeRow = createCheckboxRow({
        label: "Normalize",
        checked: true,
        onChange: checked => {
            rememberedNormalize = checked;
            apply();
        },
        hint: "",
    });

    uiRoot.append(bypassRow.row);
    uiRoot.append(typeRow.row);
    uiRoot.append(durationRow.row);
    uiRoot.append(decayRow.row);
    uiRoot.append(normalizeRow.row);

    function buildImpulseBuffer(context) {
        const sr = context.sampleRate || 48000;
        const duration = Math.max(0.01, Number(rememberedDuration) || 1.0);
        const length = Math.max(1, Math.floor(sr * duration));
        const buffer = context.createBuffer(2, length, sr);

        for (let ch = 0; ch < buffer.numberOfChannels; ch++) {
            const channel = buffer.getChannelData(ch);
            for (let i = 0; i < length; i++) {
                const t = i / Math.max(1, length - 1);
                const decay = Math.pow(1 - t, Math.max(0.1, rememberedDecay));
                if (rememberedType === "impulse") {
                    channel[i] = i === 0 ? 1 : 0;
                } else if (rememberedType === "reverse-noise") {
                    channel[i] = (Math.random() * 2 - 1) * Math.pow(t, Math.max(0.1, rememberedDecay));
                } else {
                    channel[i] = (Math.random() * 2 - 1) * decay;
                }
            }
        }

        return buffer;
    }

    function ensureAudio(context) {
        if (convolver) return;
        ctx = context;

        input = ctx.createGain();
        output = ctx.createGain();

        try {
            convolver = ctx.createConvolver();
        } catch (e) {
            convolver = null;
            log(`createConvolver failed: ${e}`);
            return;
        }

        apply();
    }

    function apply() {
        if (!input || !output || !convolver) return;

        try {
            input.disconnect();
        } catch (_) {}
        try {
            convolver.disconnect();
        } catch (_) {}

        if (bypass) {
            safeConnect(input, output);
        } else {
            safeConnect(input, convolver);
            safeConnect(convolver, output);
        }

        try {
            convolver.normalize = !!rememberedNormalize;
        } catch (e) {
            log(`convolver.normalize set failed: ${e}`);
        }

        try {
            convolver.buffer = buildImpulseBuffer(ctx);
        } catch (e) {
            log(`convolver.buffer set failed: ${e}`);
        }

        typeRow.select.disabled = bypass;
        durationRow.rangeEl.disabled = bypass;
        durationRow.numberEl.disabled = bypass;
        decayRow.rangeEl.disabled = bypass;
        decayRow.numberEl.disabled = bypass;
        normalizeRow.cb.disabled = bypass;
    }

    return {
        kind: "convolver",
        title: "ConvolverNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                bypass,
                type: rememberedType,
                duration: rememberedDuration,
                decay: rememberedDecay,
                normalize: rememberedNormalize,
            };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            rememberedType = state.type || rememberedType;
            if (state.duration !== undefined) rememberedDuration = Number(state.duration);
            if (state.decay !== undefined) rememberedDecay = Number(state.decay);
            rememberedNormalize = state.normalize !== undefined ? !!state.normalize : rememberedNormalize;

            typeRow.select.value = rememberedType;
            typeRow.select.dispatchEvent(new Event("change"));

            durationRow.numberEl.value = String(rememberedDuration);
            durationRow.numberEl.dispatchEvent(new Event("input"));

            decayRow.numberEl.value = String(rememberedDecay);
            decayRow.numberEl.dispatchEvent(new Event("input"));

            normalizeRow.cb.checked = rememberedNormalize;
            apply();
        },
        disconnectAll() {
            safeDisconnect(output);
        },
        audioNode() {
            if (!input) throw new Error("ConvolverNode input missing");
            return input;
        },
        outputNode() {
            if (!output) throw new Error("ConvolverNode output missing");
            return output;
        },
        teardownAudio() {
            safeDisconnect(input);
            safeDisconnect(convolver);
            safeDisconnect(output);
            input = null;
            convolver = null;
            output = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
