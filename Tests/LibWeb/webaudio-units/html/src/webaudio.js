export function $(id) {
    return document.getElementById(id);
}

function isNearBottom(scrollElement, thresholdPx) {
    const remaining = scrollElement.scrollHeight - (scrollElement.scrollTop + scrollElement.clientHeight);
    return remaining <= thresholdPx;
}

export function createTimestampedPreLogger(preElement, options) {
    const stickToBottomThresholdPx = (options && options.stickToBottomThresholdPx) || 10;
    const scrollContainer = preElement.closest(".logbox") || preElement;

    return line => {
        const now = new Date();
        const ts = now.toISOString().split("T")[1].replace("Z", "");
        const shouldStick = isNearBottom(scrollContainer, stickToBottomThresholdPx);

        preElement.append(document.createTextNode(`[${ts}] ${line}\n`));

        if (shouldStick) scrollContainer.scrollTop = scrollContainer.scrollHeight;
    };
}

export function createDefaultLogger(options) {
    const preId = (options && options.preId) || "log";
    const pre = document.getElementById(preId);
    if (pre) return createTimestampedPreLogger(pre);
    return line => {
        // Keep noisy logs out of normal browser consoles, but still allow debugging.
        try {
            // eslint-disable-next-line no-console
            console.log(line);
        } catch (_) {}
    };
}

const defaultLog = createDefaultLogger();

export function createPillController(elements) {
    return (text, ok) => {
        elements.text.textContent = text;
        elements.dot.classList.toggle("ok", !!ok);
        elements.dot.classList.toggle("bad", ok === false);
    };
}

export function parseNumber(input, fallback) {
    const v = Number(input.value);
    return Number.isFinite(v) ? v : fallback;
}

export function clampNumber(v, lo, hi) {
    if (!Number.isFinite(v)) return lo;
    return Math.min(hi, Math.max(lo, v));
}

export function safeDisconnect(node) {
    try {
        if (node && node.disconnect) node.disconnect();
    } catch (_) {}
}

export function safeConnect(a, b) {
    try {
        a.connect(b);
    } catch (e) {
        defaultLog(`connect failed: ${e}`);
    }
}

export function formatBytes(bytes) {
    const b = Number(bytes);
    if (!Number.isFinite(b) || b < 0) return "-";
    if (b < 1024) return `${Math.round(b)} B`;
    const kb = b / 1024;
    if (kb < 1024) return `${kb.toFixed(1)} KiB`;
    const mb = kb / 1024;
    if (mb < 1024) return `${mb.toFixed(2)} MiB`;
    const gb = mb / 1024;
    return `${gb.toFixed(2)} GiB`;
}

export function formatBitrate(bitsPerSecond) {
    const bps = Number(bitsPerSecond);
    if (!Number.isFinite(bps) || bps <= 0) return "-";
    if (bps >= 1_000_000) return `${(bps / 1_000_000).toFixed(2)} Mbps`;
    return `${Math.round(bps / 1000)} kbps`;
}

export function syncRangeAndNumber(rangeEl, numberEl, onChange) {
    rangeEl.addEventListener("input", () => {
        numberEl.value = rangeEl.value;
        onChange();
    });
    numberEl.addEventListener("input", () => {
        rangeEl.value = numberEl.value;
        onChange();
    });
}

export function createTimeDomainOscilloscope(options) {
    let raf = 0;

    function stop() {
        cancelAnimationFrame(raf);
        raf = 0;
        options.setPill("idle", null);
    }

    function start(params) {
        cancelAnimationFrame(raf);
        raf = 0;

        if (!params.enabled || !params.analyser) {
            options.setPill("idle", null);
            return;
        }

        const g = options.canvas.getContext("2d");
        if (!g) {
            options.setPill("no canvas", false);
            return;
        }

        const analyser = params.analyser;
        const buffer = new Float32Array(analyser.fftSize);

        options.setPill("running", true);

        const draw = () => {
            if (!params.enabled || !params.analyser) {
                options.setPill("idle", null);
                return;
            }

            try {
                analyser.getFloatTimeDomainData(buffer);
            } catch (e) {
                options.setPill("AnalyserNode missing?", false);
                options.log(`Scope error: ${e}`);
                return;
            }

            const w = options.canvas.width;
            const h = options.canvas.height;

            g.clearRect(0, 0, w, h);

            g.strokeStyle = "rgba(255,255,255,0.08)";
            g.lineWidth = 1;
            g.beginPath();
            for (let i = 1; i < 10; i++) {
                const x = (w * i) / 10;
                g.moveTo(x, 0);
                g.lineTo(x, h);
            }
            for (let i = 1; i < 6; i++) {
                const y = (h * i) / 6;
                g.moveTo(0, y);
                g.lineTo(w, y);
            }
            g.stroke();

            g.strokeStyle = "rgba(122,162,255,0.95)";
            g.lineWidth = 2;
            g.beginPath();

            const n = buffer.length;
            for (let i = 0; i < n; i++) {
                const x = (i / (n - 1)) * w;
                const y = (0.5 - buffer[i] * 0.45) * h;
                if (i === 0) g.moveTo(x, y);
                else g.lineTo(x, y);
            }
            g.stroke();

            raf = requestAnimationFrame(draw);
        };

        raf = requestAnimationFrame(draw);
    }

    return { start, stop };
}

export function createFrequencyDomainSpectrum(options) {
    let raf = 0;

    function stop() {
        cancelAnimationFrame(raf);
        raf = 0;
        options.setPill("idle", null);
    }

    function start(params) {
        cancelAnimationFrame(raf);
        raf = 0;

        if (!params.enabled || !params.analyser) {
            options.setPill("idle", null);
            return;
        }

        const g = options.canvas.getContext("2d");
        if (!g) {
            options.setPill("no canvas", false);
            return;
        }

        const analyser = params.analyser;
        const buffer = new Uint8Array(analyser.frequencyBinCount);

        options.setPill("running", true);

        const draw = () => {
            if (!params.enabled || !params.analyser) {
                options.setPill("idle", null);
                return;
            }

            try {
                analyser.getByteFrequencyData(buffer);
            } catch (e) {
                options.setPill("AnalyserNode missing?", false);
                options.log(`Spectrum error: ${e}`);
                return;
            }

            const w = options.canvas.width;
            const h = options.canvas.height;
            g.clearRect(0, 0, w, h);

            // Grid
            g.strokeStyle = "rgba(255,255,255,0.08)";
            g.lineWidth = 1;
            g.beginPath();
            for (let i = 1; i < 10; i++) {
                const x = (w * i) / 10;
                g.moveTo(x, 0);
                g.lineTo(x, h);
            }
            for (let i = 1; i < 6; i++) {
                const y = (h * i) / 6;
                g.moveTo(0, y);
                g.lineTo(w, y);
            }
            g.stroke();

            // Bars
            const n = buffer.length;
            const barW = Math.max(1, Math.floor(w / n));
            g.fillStyle = "rgba(122,162,255,0.85)";

            for (let i = 0; i < n; i++) {
                const v = buffer[i] / 255;
                const barH = Math.max(1, Math.floor(v * h));
                const x = i * barW;
                const y = h - barH;
                g.fillRect(x, y, barW, barH);
            }

            raf = requestAnimationFrame(draw);
        };

        raf = requestAnimationFrame(draw);
    }

    return { start, stop };
}

export function setupMediaElementFilePicker(options) {
    const log = options.log || createDefaultLogger();

    let currentObjectUrl = null;

    function revoke() {
        if (!currentObjectUrl) return;
        try {
            URL.revokeObjectURL(currentObjectUrl);
        } catch (_) {}
        currentObjectUrl = null;
    }

    function setHasFile(hasFile) {
        if (options.onSelectionChanged) options.onSelectionChanged(hasFile);
    }

    options.fileInput.addEventListener("change", () => {
        const files = options.fileInput.files;
        if (!files || files.length === 0) {
            revoke();
            setHasFile(false);
            return;
        }

        const file = files[0];
        revoke();

        currentObjectUrl = URL.createObjectURL(file);
        options.mediaElement.src = currentObjectUrl;
        options.mediaElement.load();

        setHasFile(true);

        if (options.onFileSelected) options.onFileSelected(file);

        log(`Media file -> ${file.name}`);
    });

    // Default state: no file selected.
    setHasFile(false);

    return {
        teardown() {
            revoke();
        },
    };
}

export function setupStatusTextLoop(options) {
    const intervalMs = options.intervalMs || 250;
    let timer = 0;

    function stop() {
        if (timer) clearInterval(timer);
        timer = 0;
    }

    function start() {
        stop();
        timer = setInterval(() => {
            try {
                options.target.textContent = options.text();
            } catch (_) {}
        }, intervalMs);
    }

    return { start, stop };
}
