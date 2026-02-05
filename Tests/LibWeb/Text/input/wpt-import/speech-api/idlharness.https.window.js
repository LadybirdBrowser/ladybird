// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: timeout=long

'use strict';

// https://w3c.github.io/speech-api/#dom-speechsynthesis-getvoices can
// return an empty list and a voiceschanged event is fired if the list of
// voices is determined asynchronously.
function getVoices() {
  return new Promise(resolve => {
    const voices = speechSynthesis.getVoices();
    if (voices.length) {
        resolve(voices);
    } else {
        // wait for voiceschanged event
        speechSynthesis.addEventListener('voiceschanged', () => {
          resolve(speechSynthesis.getVoices());
        }, { once: true });
      }
  });
}

idl_test(
  ['speech-api'],
  ['dom', 'html'],
  (idl_array, t) => {
    idl_array.add_objects({
      SpeechGrammar: ['new SpeechGrammar()'],
      SpeechGrammarList: ['new SpeechGrammarList()'],
      SpeechRecognition: ['new SpeechRecognition()'],
      // TODO: SpeechRecognitionAlternative
      // TODO: SpeechRecognitionErrorEvent
      // TODO: SpeechRecognitionEvent
      // TODO: SpeechRecognitionResult
      // TODO: SpeechRecognitionResultList
      SpeechSynthesis: ['speechSynthesis'],
      // TODO: SpeechSynthesisErrorEvent
      // TODO: SpeechSynthesisEvent
      SpeechSynthesisUtterance: ['new SpeechSynthesisUtterance()'],
      SpeechSynthesisVoice: ['voice'],
      Window: ['self'],
    });

    const awaitVoice = getVoices().then(voices => self.voice = voices[0]);
    const timeout = new Promise((_, reject) => {
      t.step_timeout(() => reject('Timed out waiting for voice'), 3000);
    });
    return Promise.race([awaitVoice, timeout]);
  }
);
