'use strict';

// Dependency Notice: This helper relies on 'mathml-fragments.js'
// to be loaded prior to this script in the test HTML file.

const APPLIED_COLOR_RGB = 'rgb(0, 128, 0)';
const TEST_CSS_TEXT = `color: ${APPLIED_COLOR_RGB}`;
const CSS_TEXT_HASH = 'sha256-cgrWcJeXGgvsbzqeXr9ARPkciN38f5Rd6u0ahueqhkU=';

const ELEM_CSS_TEXT = `math { ${TEST_CSS_TEXT}; }`;
const ELEM_CSS_HASH = 'sha256-/n1LY0Mb4Hs1ejsIuf7bVyNgdFVUEPsG/esgnIQgikY=';

/**
 * Executes a single CSP style test on a MathML element inside an isolated
 * iframe.
 *
 * @param {Object} t - WPT test instance (provides add_cleanup).
 * @param {Object} options - Test execution configuration.
 * @param {string} options.tag - MathML element tag name present in
 *     MathMLFragments.
 * @param {string} options.policy - CSP directive header (e.g.,
 *     "style-src-attr 'unsafe-inline'").
 * @param {boolean} options.shouldApply - True if the style is expected to be
 *     allowed & applied; false if blocked.
 * @param {'styleAttribute' | 'styleElement'} [options.styleApplication] -
 *     Manner in which style is applied:
 *   - 'styleAttribute': Tests inline `style` attribute (governed by
 * style-src-attr / style-src).
 *   - 'styleElement': Tests `<style>` element inside the document (governed by
 * style-src-elem / style-src).
 * @example
 * // Testing inline style attribute with a policy
 * runMathMLStyleCSPTest(t, {
 *   tag: 'mspace',
 *   policy: "style-src-attr 'none'",
 *   shouldApply: false,
 *   styleApplication: 'styleAttribute'
 * });
 */
function runMathMLStyleCSPTest(t, {
  tag,
  policy,
  shouldApply,
  styleApplication,
}) {
  const iframe = document.createElement('iframe');
  document.body.appendChild(iframe);
  t.add_cleanup(() => iframe.remove());

  const win = iframe.contentWindow;
  const doc = iframe.contentDocument;
  const metaHtml = policy ?
      `<meta http-equiv="Content-Security-Policy" content="${policy}">` :
      '';

  doc.open();
  doc.write(`<!doctype html><html><head>${metaHtml}</head><body><math>${
      MathMLFragments[tag]}</math></body></html>`);
  doc.close();

  const mathEl = FragmentHelper.element(doc.body.firstElementChild);
  assert_true(!!mathEl, `MathML element <${tag}> should exist in iframe DOM.`);

  if (styleApplication === 'styleAttribute') {
    // Test inline style attribute
    mathEl.setAttribute('style', TEST_CSS_TEXT);
  } else if (styleApplication === 'styleElement') {
    // Test <style> element injected inside MathML fragment
    const styleEl = doc.createElement('style');
    styleEl.textContent = ELEM_CSS_TEXT;
    doc.head.appendChild(styleEl);
  } else {
    throw new Error(`[runMathMLStyleCSPTest] Invalid styleApplication: '${
        styleApplication}'. Expected 'styleAttribute' or 'styleElement'.`);
  }

  const computedColor = win.getComputedStyle(mathEl).color;
  const description = `Inline style (${styleApplication}) on <${
      tag}> with policy [${policy || 'none'}]`;

  if (shouldApply) {
    assert_equals(computedColor, APPLIED_COLOR_RGB,
                  `${description} should be APPLIED.`);
  } else {
    assert_not_equals(computedColor, APPLIED_COLOR_RGB,
                      `${description} should be BLOCKED.`);
  }
}
