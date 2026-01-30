// Common helpers for the WebAudio interactive HTML pages in this folder.

(() => {
    /** @param {string} id */
    function $(id) {
        // @ts-ignore
        return document.getElementById(id);
    }

    /**
     * @param {HTMLElement} scrollElement
     * @param {number} thresholdPx
     */
    function isNearBottom(scrollElement, thresholdPx) {
        const remaining = scrollElement.scrollHeight - (scrollElement.scrollTop + scrollElement.clientHeight);
        return remaining <= thresholdPx;
    }

    /**
     * @param {HTMLElement} preElement
     * @param {{ stickToBottomThresholdPx?: number }=} options
     * @returns {(line: string) => void}
     */
    function createTimestampedPreLogger(preElement, options) {
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

    /**
     * Returns a logger that targets a <pre> element if present (defaults to #log),
     * otherwise falls back to console.
     * @param {{ preId?: string }=} options
     */
    function createDefaultLogger(options) {
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

    /**
     * @param {{ dot: HTMLElement, text: HTMLElement }} elements
     * @returns {(text: string, ok: (boolean|null|undefined)) => void}
     */
    function createPillController(elements) {
        return (text, ok) => {
            elements.text.textContent = text;
            elements.dot.classList.toggle("ok", !!ok);
            elements.dot.classList.toggle("bad", ok === false);
        };
    }

    /** @param {HTMLInputElement} input @param {number} fallback */
    function parseNumber(input, fallback) {
        const v = Number(input.value);
        return Number.isFinite(v) ? v : fallback;
    }

    /** @param {number} v @param {number} lo @param {number} hi */
    function clampNumber(v, lo, hi) {
        if (!Number.isFinite(v)) return lo;
        return Math.min(hi, Math.max(lo, v));
    }

    /**
     * Synchronize a range input and numeric input.
     * @param {HTMLInputElement} rangeEl
     * @param {HTMLInputElement} numberEl
     * @param {() => void} onChange
     */
    function syncRangeAndNumber(rangeEl, numberEl, onChange) {
        rangeEl.addEventListener("input", () => {
            numberEl.value = rangeEl.value;
            onChange();
        });
        numberEl.addEventListener("input", () => {
            rangeEl.value = numberEl.value;
            onChange();
        });
    }

    /**
     * Simple time-domain oscilloscope renderer.
     * @param {{
     *   canvas: HTMLCanvasElement,
     *   log: (line: string) => void,
     *   setPill: (text: string, ok: (boolean|null|undefined)) => void,
     * }} options
     */
    function createTimeDomainOscilloscope(options) {
        let raf = 0;

        function stop() {
            cancelAnimationFrame(raf);
            raf = 0;
            options.setPill("idle", null);
        }

        /**
         * @param {{ analyser: (AnalyserNode|null), enabled: boolean }} params
         */
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

    /**
     * Simple frequency-domain spectrum renderer.
     * @param {{
     *   canvas: HTMLCanvasElement,
     *   log: (line: string) => void,
     *   setPill: (text: string, ok: (boolean|null|undefined)) => void,
     * }} options
     */
    function createFrequencyDomainSpectrum(options) {
        let raf = 0;

        function stop() {
            cancelAnimationFrame(raf);
            raf = 0;
            options.setPill("idle", null);
        }

        /**
         * @param {{ analyser: (AnalyserNode|null), enabled: boolean }} params
         */
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

    /**
     * Wires a standard AudioContext lifecycle header:
     * - #stateDot/#stateText pill
     * - #btnInit #btnResume #btnSuspend #btnClose
     *
     * The markup can be reused across multiple interactive pages.
     *
     * @param {{
     *   latencyHint?: AudioContextLatencyCategory,
     *   log?: ((line: string) => void),
     *   onContextChanged?: ((ctx: AudioContext|null) => void),
     *   onAfterResume?: ((ctx: AudioContext) => void),
     *   onStateChange?: ((state: AudioContextState) => void),
     *   onBeforeClose?: ((ctx: AudioContext) => (void|Promise<void>)),
     *   onAfterClose?: (() => void),
     * }=} options
     */
    function setupAudioContextControls(options) {
        const log = (options && options.log) || createDefaultLogger();

        const stateDot = document.getElementById("stateDot");
        const stateText = document.getElementById("stateText");
        const btnInit = /** @type {HTMLButtonElement|null} */ (document.getElementById("btnInit"));
        const btnResume = /** @type {HTMLButtonElement|null} */ (document.getElementById("btnResume"));
        const btnSuspend = /** @type {HTMLButtonElement|null} */ (document.getElementById("btnSuspend"));
        const btnClose = /** @type {HTMLButtonElement|null} */ (document.getElementById("btnClose"));

        const setStatePill =
            stateDot && stateText ? createPillController({ dot: stateDot, text: stateText }) : () => {};

        /** @type {AudioContext|null} */
        let ctx = null;

        function updateUI() {
            const hasCtx = !!ctx;
            const state = ctx ? ctx.state : "uninitialized";

            if (btnInit) btnInit.disabled = hasCtx;
            if (btnResume) btnResume.disabled = !hasCtx || state === "running";
            if (btnSuspend) btnSuspend.disabled = !hasCtx || state !== "running";
            if (btnClose) btnClose.disabled = !hasCtx;

            setStatePill(state, hasCtx && state === "running" ? true : hasCtx ? null : false);
        }

        async function initContext() {
            if (ctx) return ctx;

            ctx = new AudioContext({ latencyHint: (options && options.latencyHint) || "interactive" });
            log(`Created AudioContext (state=${ctx.state}, sampleRate=${ctx.sampleRate})`);

            ctx.onstatechange = () => {
                log(`AudioContext statechange -> ${ctx.state}`);
                if (options && options.onStateChange) options.onStateChange(ctx.state);
                updateUI();
            };

            if (options && options.onContextChanged) options.onContextChanged(ctx);
            updateUI();
            return ctx;
        }

        async function resume() {
            const context = await initContext();
            try {
                await context.resume();
                log("ctx.resume() ok");
            } catch (e) {
                log(`ctx.resume() failed: ${e}`);
            }
            updateUI();
            return context;
        }

        async function initAndResume() {
            const context = await resume();
            if (options && options.onAfterResume) options.onAfterResume(context);
            return context;
        }

        async function suspend() {
            if (!ctx) return;
            try {
                await ctx.suspend();
                log("ctx.suspend() ok");
            } catch (e) {
                log(`ctx.suspend() failed: ${e}`);
            }
            updateUI();
        }

        async function close() {
            if (!ctx) return;

            if (options && options.onBeforeClose) await options.onBeforeClose(ctx);

            try {
                await ctx.close();
                log("ctx.close() ok");
            } catch (e) {
                log(`ctx.close() failed: ${e}`);
            }

            ctx = null;
            if (options && options.onContextChanged) options.onContextChanged(null);
            if (options && options.onAfterClose) options.onAfterClose();
            updateUI();
        }

        if (btnInit) {
            btnInit.addEventListener("click", async () => {
                await initAndResume();
            });
        }

        if (btnResume) {
            btnResume.addEventListener("click", async () => {
                await initAndResume();
            });
        }

        if (btnSuspend) {
            btnSuspend.addEventListener("click", async () => {
                await suspend();
            });
        }

        if (btnClose) {
            btnClose.addEventListener("click", async () => {
                await close();
            });
        }

        updateUI();

        return {
            get ctx() {
                return ctx;
            },
            log,
            updateUI,
            initContext,
            resume,
            initAndResume,
            suspend,
            close,
        };
    }

    /**
     * Wires an <input type=file> to a <audio>/<video> element via object URLs.
     * Manages URL.revokeObjectURL for you.
     *
     * @param {{
     *   fileInput: HTMLInputElement,
     *   mediaElement: HTMLMediaElement,
     *   log?: ((line: string) => void),
     *   onSelectionChanged?: ((hasFile: boolean) => void),
     *   onFileSelected?: ((file: File) => void),
     * } } options
     */
    function setupMediaElementFilePicker(options) {
        const log = options.log || createDefaultLogger();

        /** @type {string|null} */
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

    /**
     * Updates a text element on a fixed interval.
     *
     * @param {{
     *   intervalMs?: number,
     *   target: HTMLElement,
     *   text: (() => string),
     * }} options
     */
    function setupStatusTextLoop(options) {
        const intervalMs = options.intervalMs || 250;
        /** @type {number} */
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

    /**
     * Implements the common "Start/Stop" playback controls for an HTMLMediaElement.
     * Also keeps visualizers in sync with play/pause/ended.
     *
     * @param {{
     *   audioControls: ReturnType<typeof setupAudioContextControls>,
     *   mediaElement: HTMLMediaElement,
     *   startButton: HTMLButtonElement,
     *   stopButton: HTMLButtonElement,
     *   log?: ((line: string) => void),
     *   canStart?: (() => boolean),
     *   ensureGraph?: (() => void),
     *   startVisualizers?: (() => void),
     *   stopVisualizers?: (() => void),
     *   applyParams?: (() => void),
     *   stopResetsTime?: boolean,
     * }} options
     */
    function setupMediaElementStartStopControls(options) {
        const log = options.log || createDefaultLogger();
        const stopResetsTime = options.stopResetsTime !== undefined ? options.stopResetsTime : true;
        const canStart = options.canStart || (() => true);

        function isPlaying() {
            return !options.mediaElement.paused && !options.mediaElement.ended;
        }

        async function doStart() {
            try {
                await options.audioControls.initAndResume();

                if (options.ensureGraph) options.ensureGraph();
                if (options.startVisualizers) options.startVisualizers();

                await options.mediaElement.play();

                if (options.applyParams) options.applyParams();

                log("media.play() ok");
            } catch (e) {
                log(`media.play() failed: ${e}`);
            }
            updateButtons();
        }

        function doStop() {
            try {
                options.mediaElement.pause();
                if (stopResetsTime) {
                    try {
                        options.mediaElement.currentTime = 0;
                    } catch (_) {}
                }

                if (options.stopVisualizers) options.stopVisualizers();

                log("media.stop() ok");
            } catch (e) {
                log(`media.stop() failed: ${e}`);
            }
            updateButtons();
        }

        function updateButtons() {
            const hasCtx = !!options.audioControls.ctx;
            const playing = isPlaying();
            const startAllowed = canStart();

            options.startButton.disabled = !hasCtx || !startAllowed || playing;
            options.stopButton.disabled = !hasCtx || !playing;
        }

        const onPlay = () => {
            try {
                if (options.ensureGraph) options.ensureGraph();
            } catch (_) {}
            if (options.startVisualizers) options.startVisualizers();
            updateButtons();
        };

        const onPause = () => {
            if (options.stopVisualizers) options.stopVisualizers();
            updateButtons();
        };

        const onEnded = () => {
            if (options.stopVisualizers) options.stopVisualizers();
            updateButtons();
        };

        options.startButton.addEventListener("click", doStart);
        options.stopButton.addEventListener("click", doStop);
        options.mediaElement.addEventListener("play", onPlay);
        options.mediaElement.addEventListener("pause", onPause);
        options.mediaElement.addEventListener("ended", onEnded);

        updateButtons();

        return {
            updateButtons,
            teardown() {
                options.startButton.removeEventListener("click", doStart);
                options.stopButton.removeEventListener("click", doStop);
                options.mediaElement.removeEventListener("play", onPlay);
                options.mediaElement.removeEventListener("pause", onPause);
                options.mediaElement.removeEventListener("ended", onEnded);
            },
        };
    }

    // Expose as a single global to avoid polluting the window.
    // @ts-ignore
    window.WebAudio = {
        $,
        createTimestampedPreLogger,
        createDefaultLogger,
        createPillController,
        parseNumber,
        clampNumber,
        syncRangeAndNumber,
        createTimeDomainOscilloscope,
        createFrequencyDomainSpectrum,
        setupAudioContextControls,
        setupMediaElementFilePicker,
        setupStatusTextLoop,
        setupMediaElementStartStopControls,
    };
})();
