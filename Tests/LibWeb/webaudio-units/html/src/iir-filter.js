export function createIIRFilterProcessor(log, ui) {
    const { h, createCheckboxRow, createSelectRow, createInfoRow, safeConnect, safeDisconnect } = ui;

    let ctx = null;
    let input = null;
    let filter = null;
    let output = null;

    let bypass = false;
    let rememberedPreset = "lowpass-1000";

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

    const presetRow = createSelectRow({
        label: "Preset",
        optionsList: [
            { value: "lowpass-1000", text: "lowpass 1kHz", selected: true },
            { value: "highpass-1000", text: "highpass 1kHz" },
            { value: "allpass-1000", text: "allpass 1kHz" },
        ],
        onChange: v => {
            rememberedPreset = v;
            rebuildFilter();
            apply();
        },
    });

    const feedforwardRow = createInfoRow("Feedforward");
    const feedbackRow = createInfoRow("Feedback");

    uiRoot.append(bypassRow.row);
    uiRoot.append(presetRow.row);
    uiRoot.append(feedforwardRow.row);
    uiRoot.append(feedbackRow.row);

    function formatArray(arr) {
        return `[${arr.map(v => v.toFixed(6)).join(", ")}]`;
    }

    function computeCoefficients(preset, sampleRate) {
        const sr = Math.max(1, Number(sampleRate) || 48000);
        const freq = 1000;
        const c = Math.tan((Math.PI * freq) / sr);
        const a1 = (c - 1) / (c + 1);
        if (preset === "highpass-1000") {
            const b0 = 1 / (1 + c);
            const b1 = -b0;
            return { feedforward: [b0, b1], feedback: [1, a1] };
        }
        if (preset === "allpass-1000") {
            const b0 = a1;
            const b1 = 1;
            return { feedforward: [b0, b1], feedback: [1, a1] };
        }
        const b0 = c / (1 + c);
        const b1 = b0;
        return { feedforward: [b0, b1], feedback: [1, a1] };
    }

    function rebuildFilter() {
        if (!ctx) return;
        if (filter) {
            try {
                filter.disconnect();
            } catch (_) {}
        }

        const { feedforward, feedback } = computeCoefficients(rememberedPreset, ctx.sampleRate);
        try {
            filter = ctx.createIIRFilter(feedforward, feedback);
        } catch (e) {
            filter = null;
            log(`createIIRFilter failed: ${e}`);
            return;
        }

        feedforwardRow.value.textContent = formatArray(feedforward);
        feedbackRow.value.textContent = formatArray(feedback);
    }

    function ensureAudio(context) {
        if (filter) return;
        ctx = context;

        input = ctx.createGain();
        output = ctx.createGain();

        rebuildFilter();
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

        presetRow.select.disabled = bypass;
    }

    return {
        kind: "iirFilter",
        title: "IIRFilterNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                bypass,
                preset: rememberedPreset,
            };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            rememberedPreset = state.preset || rememberedPreset;

            presetRow.select.value = rememberedPreset;
            presetRow.select.dispatchEvent(new Event("change"));

            bypassRow.cb.checked = bypass;
            apply();
        },
        disconnectAll() {
            safeDisconnect(output);
        },
        audioNode() {
            if (!input) throw new Error("IIRFilterNode input missing");
            return input;
        },
        outputNode() {
            if (!output) throw new Error("IIRFilterNode output missing");
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
