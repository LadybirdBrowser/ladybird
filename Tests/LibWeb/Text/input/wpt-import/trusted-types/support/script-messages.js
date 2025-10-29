'use strict'

const LOG_RUN_MESSAGE = `window.log_message("RUN")`;

function create_html_script_with_trusted_source_text(source_text) {
  let script = document.createElement("script");
  script.text = passthroughpolicy.createScript(source_text);
  return script;
}

function create_html_script_with_untrusted_source_text(source_text) {
  let script = document.createElement("script");
  // Setting script source via Node.appendChild() drops trustworthiness.
  script.appendChild(document.createTextNode(source_text));
  return script;
}

function create_svg_script_with_trusted_source_text(source_text) {
  // SVGScriptElement has no API to set its source while preserving its
  // trustworthiness. For now, we just expect a <script type="unknown"> tag
  // with the desired source to already be present in the page, so we can just
  // reuse it. See https://github.com/w3c/trusted-types/issues/512
  let script =
      Array.from(document.querySelectorAll("svg script[type='unknown']")).
      find(script => script.textContent === source_text);
  assert_true(!!script, `<script type="unknown">${source_text}</script> not found!`);
  script.remove();
  script.removeAttribute("type");
  return script;
}

function create_svg_script_with_untrusted_source_text(source_text) {
  let script = document.createElementNS(NSURI_SVG, "script")
  // Setting script source via Node.appendChild() drops trustworthiness.
  script.appendChild(document.createTextNode(source_text));
  return script;
}

// A generic helper that runs function fn and returns a promise resolving with
// an array of received messages. A script forcing a "done" message is appended
// after calling fn, to make sure that all the messages reported by fn have been
// delivered.
function script_messages_for(fn) {
  return new Promise(async (resolve, reject) => {
    // Listen for messages.
    let messages = [];
    let exception = null;
    window.log_message = message => {
      if (message === "DONE") {
        window.log_message = null;
        if (exception) {
          reject(exception);
        } else {
          resolve(messages);
        }
      } else {
        messages.push(message);
      }
    };

    // Execute the function.
    try {
      await fn();
    } catch(e) {
      exception = e;
    }

    // Indicate the last message.
    // This is done by appending an inline script to make sure it is executed
    // after processing any previously inserted inline script. Additionally, we
    // delay by a double requestAnimationFrame to work around incompatible
    // interop bugs:
    // - WebKit/Chromium seems to give lower priority to module, so it looks
    //   like the appended script should have type="module" here to work with
    //   tests for inline modules.
    // - but Firefox does not allow type="importmaps" after a type="module" so
    //   making the appended script a module would make importmap tests fail...
    //   See https://bugzilla.mozilla.org/show_bug.cgi?id=1916277#c4
    requestAnimationFrame(_ => requestAnimationFrame(_ => {
      let script = create_html_script_with_trusted_source_text(`window.log_message("DONE")`);
      script.setAttribute("nonce", "script-messages");
      document.body.appendChild(script);
    }));
  });
}

async function script_message_for(fn) {
  let messages = await script_messages_for(fn);
  assert_equals(messages.length, 1, `Number of messages (${messages})`);
  return messages[0];
}

async function no_script_message_for(fn) {
  let messages = await script_messages_for(fn);
  assert_equals(messages.length, 0, `Number of messages (${messages})`);
}

async function base64_hash_for_inline_script(source_text, algorithm) {
  const encoder = new TextEncoder();
  const data = encoder.encode(source_text);
  const hashBuffer = await window.crypto.subtle.digest(algorithm, data);
  const base64Array = (new Uint8Array(hashBuffer)).toBase64();
  return base64Array.toString();
}
