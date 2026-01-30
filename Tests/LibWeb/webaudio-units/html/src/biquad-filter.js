export function createBiquadFilterProcessor(log, ui) {
    const { h, createCheckboxRow, createSelectRow, createRangeNumberRow, clampNumber, safeConnect, safeDisconnect } =
        ui;

    let ctx = null;
    let input = null;
    let filter = null;
    let output = null;

    let bypass = false;

    let rememberedType = "lowpass";
    let rememberedFrequency = 1000;
    let rememberedDetune = 0;
    let rememberedQ = 1;
    let rememberedGain = 0;

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
        label: "Type",
        optionsList: [
            { value: "lowpass", text: "lowpass", selected: true },
            { value: "highpass", text: "highpass" },
            { value: "bandpass", text: "bandpass" },
            { value: "lowshelf", text: "lowshelf" },
            { value: "highshelf", text: "highshelf" },
            { value: "peaking", text: "peaking" },
            { value: "notch", text: "notch" },
            { value: "allpass", text: "allpass" },
        ],
        onChange: v => {
            rememberedType = v;
            apply();
        },
    });

    const freqRow = createRangeNumberRow({
        label: "Frequency (Hz)",
        range: { min: "0", max: "24000", value: String(rememberedFrequency), step: "1" },
        number: { min: "0", max: "24000", value: String(rememberedFrequency), step: "1" },
        onChange: () => {
            const v = Number(freqRow.numberEl.value);
            rememberedFrequency = Number.isFinite(v) ? v : 1000;
            apply();
        },
    });

    const detuneRow = createRangeNumberRow({
        label: "Detune (cents)",
        range: { min: "-1200", max: "1200", value: "0", step: "1" },
        number: { min: "-24000", max: "24000", value: "0", step: "1" },
        onChange: () => {
            const v = Number(detuneRow.numberEl.value);
            rememberedDetune = Number.isFinite(v) ? v : 0;
            apply();
        },
    });

    const qRow = createRangeNumberRow({
        label: "Q",
        range: { min: "0", max: "100", value: "1", step: "0.01" },
        number: { min: "0", max: "100000", value: "1", step: "0.01" },
        onChange: () => {
            const v = Number(qRow.numberEl.value);
            rememberedQ = Number.isFinite(v) ? v : 1;
            apply();
        },
    });

    const gainRow = createRangeNumberRow({
        label: "Gain (dB)",
        range: { min: "-40", max: "40", value: "0", step: "0.1" },
        number: { min: "-100", max: "100", value: "0", step: "0.1" },
        onChange: () => {
            const v = Number(gainRow.numberEl.value);
            rememberedGain = Number.isFinite(v) ? v : 0;
            apply();
        },
    });

    uiRoot.append(bypassRow.row);
    uiRoot.append(typeRow.row);
    uiRoot.append(freqRow.row);
    uiRoot.append(detuneRow.row);
    uiRoot.append(qRow.row);
    uiRoot.append(gainRow.row);

    function ensureAudio(context) {
        if (filter) return;
        ctx = context;

        input = ctx.createGain();
        output = ctx.createGain();

        try {
            filter = ctx.createBiquadFilter();
        } catch (e) {
            filter = null;
            log(`createBiquadFilter failed: ${e}`);
            return;
        }

        apply();
    }

    function apply() {
        if (!input || !output || !filter) return;

        try {
            input.disconnect();
        } catch (_) {}
        try {
            filter.disconnect();
        } catch (_) {}

        if (bypass) {
            safeConnect(input, output);
        } else {
            safeConnect(input, filter);
            safeConnect(filter, output);
        }

        const nyquist = ctx ? ctx.sampleRate / 2 : 24000;
        const f = Number.isFinite(rememberedFrequency) ? clampNumber(rememberedFrequency, 0, nyquist) : 1000;
        const q = Number.isFinite(rememberedQ) ? rememberedQ : 1;
        const d = Number.isFinite(rememberedDetune) ? rememberedDetune : 0;
        const g = Number.isFinite(rememberedGain) ? rememberedGain : 0;

        try {
            filter.type = rememberedType;
        } catch (e) {
            log(`biquad.type set failed: ${e}`);
        }
        try {
            filter.frequency.value = f;
        } catch (e) {
            log(`biquad.frequency set failed: ${e}`);
        }
        try {
            filter.detune.value = d;
        } catch (e) {
            log(`biquad.detune set failed: ${e}`);
        }
        try {
            filter.Q.value = q;
        } catch (e) {
            log(`biquad.Q set failed: ${e}`);
        }
        try {
            filter.gain.value = g;
        } catch (e) {
            log(`biquad.gain set failed: ${e}`);
        }

        typeRow.select.disabled = bypass;
        freqRow.rangeEl.disabled = bypass;
        freqRow.numberEl.disabled = bypass;
        detuneRow.rangeEl.disabled = bypass;
        detuneRow.numberEl.disabled = bypass;
        qRow.rangeEl.disabled = bypass;
        qRow.numberEl.disabled = bypass;
        gainRow.rangeEl.disabled = bypass;
        gainRow.numberEl.disabled = bypass;
    }

    return {
        kind: "biquadFilter",
        title: "BiquadFilterNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                bypass,
                type: rememberedType,
                frequency: rememberedFrequency,
                detune: rememberedDetune,
                q: rememberedQ,
                gain: rememberedGain,
            };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            rememberedType = state.type || rememberedType;
            if (state.frequency !== undefined) rememberedFrequency = Number(state.frequency);
            if (state.detune !== undefined) rememberedDetune = Number(state.detune);
            if (state.q !== undefined) rememberedQ = Number(state.q);
            if (state.gain !== undefined) rememberedGain = Number(state.gain);

            typeRow.select.value = rememberedType;
            typeRow.select.dispatchEvent(new Event("change"));

            freqRow.numberEl.value = String(rememberedFrequency);
            detuneRow.numberEl.value = String(rememberedDetune);
            qRow.numberEl.value = String(rememberedQ);
            gainRow.numberEl.value = String(rememberedGain);
            freqRow.numberEl.dispatchEvent(new Event("input"));
            detuneRow.numberEl.dispatchEvent(new Event("input"));
            qRow.numberEl.dispatchEvent(new Event("input"));
            gainRow.numberEl.dispatchEvent(new Event("input"));

            bypassRow.cb.checked = bypass;
            apply();
        },
        disconnectAll() {
            // Preserve the input->filter->output chain; just detach our output from the outer graph.
            safeDisconnect(output);
        },
        audioNode() {
            if (!input) throw new Error("BiquadFilterNode input missing");
            return input;
        },
        outputNode() {
            if (!output) throw new Error("BiquadFilterNode output missing");
            return output;
        },
        teardownAudio() {
            safeDisconnect(input);
            safeDisconnect(filter);
            safeDisconnect(output);
            input = null;
            filter = null;
            output = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
