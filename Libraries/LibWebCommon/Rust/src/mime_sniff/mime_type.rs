/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://mimesniff.spec.whatwg.org/#javascript-mime-type
// A JavaScript MIME type is any MIME type whose essence is one of the following:
const JAVASCRIPT_MIME_TYPE_ESSENCE_STRINGS: [&str; 16] = [
    "application/ecmascript",
    "application/javascript",
    "application/x-ecmascript",
    "application/x-javascript",
    "text/ecmascript",
    "text/javascript",
    "text/javascript1.0",
    "text/javascript1.1",
    "text/javascript1.2",
    "text/javascript1.3",
    "text/javascript1.4",
    "text/javascript1.5",
    "text/jscript",
    "text/livescript",
    "text/x-ecmascript",
    "text/x-javascript",
];

// https://fetch.spec.whatwg.org/#http-whitespace
// HTTP whitespace is U+000A LF, U+000D CR, or an HTTP tab or space.
fn is_http_whitespace(code_point: char) -> bool {
    matches!(code_point, '\n' | '\r' | '\t' | ' ')
}

// https://mimesniff.spec.whatwg.org/#http-token-code-point
// An HTTP token code point is U+0021 (!), U+0023 (#), U+0024 ($), U+0025 (%), U+0026 (&), U+0027 ('), U+002A (*),
// U+002B (+), U+002D (-), U+002E (.), U+005E (^), U+005F (_), U+0060 (`), U+007C (|), U+007E (~), or an ASCII alphanumeric.
fn is_http_token_code_point(code_point: char) -> bool {
    matches!(
        code_point,
        '!' | '#' | '$' | '%' | '&' | '\'' | '*' | '+' | '-' | '.' | '^' | '_' | '`' | '|' | '~'
    ) || code_point.is_ascii_alphanumeric()
}

// https://mimesniff.spec.whatwg.org/#http-quoted-string-token-code-point
// An HTTP quoted-string token code point is U+0009 TAB, a code point in the range U+0020 SPACE to U+007E (~), inclusive,
// or a code point in the range U+0080 through U+00FF (ÿ), inclusive.
fn is_http_quoted_string_token_code_point(code_point: char) -> bool {
    matches!(code_point, '\t' | '\u{20}'..='\u{7E}' | '\u{80}'..='\u{FF}')
}

fn solely_contains_http_token_code_points(string: &str) -> bool {
    string.chars().all(is_http_token_code_point)
}

fn trim_trailing_http_whitespace(code_points: &[char]) -> &[char] {
    let end = code_points
        .iter()
        .rposition(|&code_point| !is_http_whitespace(code_point))
        .map_or(0, |index| index + 1);
    &code_points[..end]
}

// https://infra.spec.whatwg.org/#collect-a-sequence-of-code-points
fn collect_a_sequence_of_code_points<'a>(
    input: &'a [char],
    position: &mut usize,
    condition: impl Fn(char) -> bool,
) -> &'a [char] {
    let start = *position;
    while *position < input.len() && condition(input[*position]) {
        *position += 1;
    }
    &input[start..*position]
}

// https://fetch.spec.whatwg.org/#collect-an-http-quoted-string
fn collect_an_http_quoted_string_extracting_the_value(input: &[char], position: &mut usize) -> String {
    // 1. Let positionStart be position.
    // NB: Skipped, as MIME type parsing uses the extract-value flag.

    // 2. Let value be the empty string.
    let mut value = String::new();

    // 3. Assert: the code point at position within input is U+0022 (").
    assert_eq!(input[*position], '"');

    // 4. Advance position by 1.
    *position += 1;

    // 5. While true:
    loop {
        // 1. Append the result of collecting a sequence of code points that are not U+0022 (") or U+005C (\) from
        //    input, given position, to value.
        value.extend(collect_a_sequence_of_code_points(input, position, |code_point| {
            code_point != '"' && code_point != '\\'
        }));

        // 2. If position is past the end of input, then break.
        if *position >= input.len() {
            break;
        }

        // 3. Let quoteOrBackslash be the code point at position within input.
        let quote_or_backslash = input[*position];

        // 4. Advance position by 1.
        *position += 1;

        // 5. If quoteOrBackslash is U+005C (\), then:
        if quote_or_backslash == '\\' {
            // 1. If position is past the end of input, then append U+005C (\) to value and break.
            if *position >= input.len() {
                value.push('\\');
                break;
            }

            // 2. Append the code point at position within input to value.
            value.push(input[*position]);

            // 3. Advance position by 1.
            *position += 1;
        }
        // 6. Otherwise:
        else {
            // 1. Assert: quoteOrBackslash is U+0022 (").
            assert_eq!(quote_or_backslash, '"');

            // 2. Break.
            break;
        }
    }

    // 6. If the extract-value flag is set, then return value.
    value
}

/// The type and subtype of a MIME type, which say which groups of MIME types it belongs to.
#[derive(Clone, Copy)]
pub struct Essence<'a> {
    pub type_: &'a str,
    pub subtype: &'a str,
}

impl Essence<'_> {
    fn is(self, essence: &str) -> bool {
        essence.split_once('/') == Some((self.type_, self.subtype))
    }

    // https://mimesniff.spec.whatwg.org/#image-mime-type
    pub fn is_image(self) -> bool {
        // An image MIME type is a MIME type whose type is "image".
        self.type_ == "image"
    }

    // https://mimesniff.spec.whatwg.org/#audio-or-video-mime-type
    pub fn is_audio_or_video(self) -> bool {
        // An audio or video MIME type is any MIME type whose type is "audio" or "video", or whose essence is "application/ogg".
        matches!(self.type_, "audio" | "video") || self.is("application/ogg")
    }

    // https://mimesniff.spec.whatwg.org/#font-mime-type
    pub fn is_font(self) -> bool {
        // A font MIME type is any MIME type whose type is "font", or whose essence is one of the following:
        //    - application/font-cff
        //    - application/font-otf
        //    - application/font-sfnt
        //    - application/font-ttf
        //    - application/font-woff
        //    - application/vnd.ms-fontobject
        //    - application/vnd.ms-opentype
        self.type_ == "font"
            || (self.type_ == "application"
                && matches!(
                    self.subtype,
                    "font-cff"
                        | "font-otf"
                        | "font-sfnt"
                        | "font-ttf"
                        | "font-woff"
                        | "vnd.ms-fontobject"
                        | "vnd.ms-opentype"
                ))
    }

    // https://mimesniff.spec.whatwg.org/#xml-mime-type
    pub fn is_xml(self) -> bool {
        // An XML MIME type is any MIME type whose subtype ends in "+xml" or whose essence is "text/xml" or "application/xml". [RFC7303]
        self.subtype.ends_with("+xml") || self.is("text/xml") || self.is("application/xml")
    }

    // https://mimesniff.spec.whatwg.org/#html-mime-type
    pub fn is_html(self) -> bool {
        // An HTML MIME type is any MIME type whose essence is "text/html".
        self.is("text/html")
    }

    // https://mimesniff.spec.whatwg.org/#javascript-mime-type
    pub fn is_javascript(self) -> bool {
        JAVASCRIPT_MIME_TYPE_ESSENCE_STRINGS
            .iter()
            .any(|&essence| self.is(essence))
    }

    // https://mimesniff.spec.whatwg.org/#json-mime-type
    pub fn is_json(self) -> bool {
        // A JSON MIME type is any MIME type whose subtype ends in "+json" or whose essence is "application/json" or "text/json".
        self.subtype.ends_with("+json") || self.is("application/json") || self.is("text/json")
    }

    // https://mimesniff.spec.whatwg.org/#minimize-a-supported-mime-type
    pub fn minimize_a_supported_mime_type(self) -> &'static str {
        // 1. If mimeType is a JavaScript MIME type, then return "text/javascript".
        if self.is_javascript() {
            return "text/javascript";
        }

        // 2. If mimeType is a JSON MIME type, then return "application/json".
        if self.is_json() {
            return "application/json";
        }

        // 3. If mimeType’s essence is "image/svg+xml", then return "image/svg+xml".
        if self.is("image/svg+xml") {
            return "image/svg+xml";
        }

        // 4. If mimeType is an XML MIME type, then return "application/xml".
        if self.is_xml() {
            return "application/xml";
        }

        // FIXME: 5. If mimeType is supported by the user agent, then return mimeType’s essence.

        // 6. Return the empty string.
        ""
    }
}

// https://mimesniff.spec.whatwg.org/#javascript-mime-type-essence-match
pub fn is_javascript_mime_type_essence_match(string: &[char]) -> bool {
    // A string is a JavaScript MIME type essence match if it is an ASCII case-insensitive match for one of the JavaScript MIME type essence strings.
    JAVASCRIPT_MIME_TYPE_ESSENCE_STRINGS.iter().any(|essence| {
        essence.len() == string.len()
            && essence
                .chars()
                .zip(string)
                .all(|(essence_code_point, &code_point)| essence_code_point.eq_ignore_ascii_case(&code_point))
    })
}

// https://mimesniff.spec.whatwg.org/#mime-type
#[derive(Clone)]
pub struct MimeType {
    // https://mimesniff.spec.whatwg.org/#type
    // A MIME type’s type is a non-empty ASCII string.
    type_: String,

    // https://mimesniff.spec.whatwg.org/#subtype
    // A MIME type’s subtype is a non-empty ASCII string.
    subtype: String,

    // https://mimesniff.spec.whatwg.org/#parameters
    // A MIME type’s parameters is an ordered map whose keys are ASCII strings and values are strings limited to HTTP quoted-string token code points. It is initially empty.
    parameters: Vec<(String, String)>,
}

impl MimeType {
    pub fn new(type_: String, subtype: String) -> Self {
        assert!(!type_.is_empty() && solely_contains_http_token_code_points(&type_));
        assert!(!subtype.is_empty() && solely_contains_http_token_code_points(&subtype));
        Self {
            type_,
            subtype,
            parameters: Vec::new(),
        }
    }

    pub fn type_(&self) -> &str {
        &self.type_
    }

    pub fn subtype(&self) -> &str {
        &self.subtype
    }

    pub fn parameters(&self) -> impl Iterator<Item = (&str, &str)> {
        self.parameters
            .iter()
            .map(|(name, value)| (name.as_str(), value.as_str()))
    }

    pub fn set_parameter(&mut self, name: String, value: String) {
        assert!(name.chars().all(is_http_quoted_string_token_code_point));
        assert!(value.chars().all(is_http_quoted_string_token_code_point));
        match self
            .parameters
            .iter_mut()
            .find(|(existing_name, _)| *existing_name == name)
        {
            Some((_, existing_value)) => *existing_value = value,
            None => self.parameters.push((name, value)),
        }
    }

    pub fn essence(&self) -> Essence<'_> {
        Essence {
            type_: &self.type_,
            subtype: &self.subtype,
        }
    }

    // https://mimesniff.spec.whatwg.org/#parse-a-mime-type
    pub fn parse(input: &[char]) -> Option<Self> {
        // 1. Remove any leading and trailing HTTP whitespace from input.
        let start = input
            .iter()
            .position(|&code_point| !is_http_whitespace(code_point))
            .unwrap_or(input.len());
        let input = trim_trailing_http_whitespace(&input[start..]);

        // 2. Let position be a position variable for input, initially pointing at the start of input.
        let mut position = 0;

        // 3. Let type be the result of collecting a sequence of code points that are not U+002F (/) from input, given position.
        let type_ = collect_a_sequence_of_code_points(input, &mut position, |code_point| code_point != '/');

        // 4. If type is the empty string or does not solely contain HTTP token code points, then return failure.
        if type_.is_empty() || !type_.iter().copied().all(is_http_token_code_point) {
            return None;
        }

        // 5. If position is past the end of input, then return failure.
        if position >= input.len() {
            return None;
        }

        // 6. Advance position by 1. (This skips past U+002F (/).)
        position += 1;

        // 7. Let subtype be the result of collecting a sequence of code points that are not U+003B (;) from input, given position.
        let subtype = collect_a_sequence_of_code_points(input, &mut position, |code_point| code_point != ';');

        // 8. Remove any trailing HTTP whitespace from subtype.
        let subtype = trim_trailing_http_whitespace(subtype);

        // 9. If subtype is the empty string or does not solely contain HTTP token code points, then return failure.
        if subtype.is_empty() || !subtype.iter().copied().all(is_http_token_code_point) {
            return None;
        }

        // 10. Let mimeType be a new MIME type record whose type is type, in ASCII lowercase, and subtype is subtype, in ASCII lowercase.
        let mut mime_type = Self {
            type_: type_.iter().map(char::to_ascii_lowercase).collect(),
            subtype: subtype.iter().map(char::to_ascii_lowercase).collect(),
            parameters: Vec::new(),
        };

        // 11. While position is not past the end of input:
        while position < input.len() {
            // 1. Advance position by 1. (This skips past U+003B (;).)
            position += 1;

            // 2. Collect a sequence of code points that are HTTP whitespace from input given position.
            collect_a_sequence_of_code_points(input, &mut position, is_http_whitespace);

            // 3. Let parameterName be the result of collecting a sequence of code points that are not U+003B (;) or U+003D (=) from input, given position.
            let parameter_name = collect_a_sequence_of_code_points(input, &mut position, |code_point| {
                code_point != ';' && code_point != '='
            });

            // 4. Set parameterName to parameterName, in ASCII lowercase.
            let parameter_name: String = parameter_name.iter().map(char::to_ascii_lowercase).collect();

            // 5. If position is not past the end of input, then:
            if position < input.len() {
                // 1. If the code point at position within input is U+003B (;), then continue.
                if input[position] == ';' {
                    continue;
                }

                // 2. Advance position by 1. (This skips past U+003D (=).)
                position += 1;
            }

            // 6. If position is past the end of input, then break.
            if position >= input.len() {
                break;
            }

            // 7. Let parameterValue be null.
            let parameter_value;

            // 8. If the code point at position within input is U+0022 ("), then:
            if input[position] == '"' {
                // 1. Set parameterValue to the result of collecting an HTTP quoted string from input, given position and the extract-value flag.
                parameter_value = collect_an_http_quoted_string_extracting_the_value(input, &mut position);

                // 2. Collect a sequence of code points that are not U+003B (;) from input, given position.
                collect_a_sequence_of_code_points(input, &mut position, |code_point| code_point != ';');
            }
            // 9. Otherwise:
            else {
                // 1. Set parameterValue to the result of collecting a sequence of code points that are not U+003B (;) from input, given position.
                let value = collect_a_sequence_of_code_points(input, &mut position, |code_point| code_point != ';');

                // 2. Remove any trailing HTTP whitespace from parameterValue.
                let value = trim_trailing_http_whitespace(value);

                // 3. If parameterValue is the empty string, then continue.
                if value.is_empty() {
                    continue;
                }
                parameter_value = value.iter().collect();
            }

            // 10. If all of the following are true
            //     - parameterName is not the empty string
            //     - parameterName solely contains HTTP token code points
            //     - parameterValue solely contains HTTP quoted-string token code points
            //     - mimeType’s parameters[parameterName] does not exist
            //     then set mimeType’s parameters[parameterName] to parameterValue.
            if !parameter_name.is_empty()
                && solely_contains_http_token_code_points(&parameter_name)
                && parameter_value.chars().all(is_http_quoted_string_token_code_point)
                && !mime_type.parameters.iter().any(|(name, _)| *name == parameter_name)
            {
                mime_type.parameters.push((parameter_name, parameter_value));
            }
        }

        // 12. Return mimeType.
        Some(mime_type)
    }

    // https://mimesniff.spec.whatwg.org/#serialize-a-mime-type
    pub fn serialize(&self) -> String {
        // 1. Let serialization be the concatenation of mimeType’s type, U+002F (/), and mimeType’s subtype.
        let mut serialization = format!("{}/{}", self.type_, self.subtype);

        // 2. For each name → value of mimeType’s parameters:
        for (name, value) in &self.parameters {
            // 1. Append U+003B (;) to serialization.
            serialization.push(';');

            // 2. Append name to serialization.
            serialization.push_str(name);

            // 3. Append U+003D (=) to serialization.
            serialization.push('=');

            // 4. If value does not solely contain HTTP token code points or value is the empty string, then:
            if !solely_contains_http_token_code_points(value) || value.is_empty() {
                // 1. Precede each occurrence of U+0022 (") or U+005C (\) in value with U+005C (\).
                // 2. Prepend U+0022 (") to value.
                // 3. Append U+0022 (") to value.
                serialization.push('"');
                for code_point in value.chars() {
                    if code_point == '"' || code_point == '\\' {
                        serialization.push('\\');
                    }
                    serialization.push(code_point);
                }
                serialization.push('"');
                continue;
            }

            // 5. Append value to serialization.
            serialization.push_str(value);
        }

        // 3. Return serialization.
        serialization
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(input: &str) -> Option<MimeType> {
        MimeType::parse(&input.chars().collect::<Vec<_>>())
    }

    #[test]
    fn parses_and_serializes() {
        let mime_type = parse(" Text/HTML ; Charset=\"utf-8\" ; charset=ignored;flag;x=\"a\\\"b\"").unwrap();
        assert_eq!(mime_type.type_(), "text");
        assert_eq!(mime_type.subtype(), "html");
        assert_eq!(mime_type.serialize(), "text/html;charset=utf-8;x=\"a\\\"b\"");
        assert!(mime_type.essence().is_html());
    }

    #[test]
    fn rejects_invalid_types() {
        assert!(parse("").is_none());
        assert!(parse("text").is_none());
        assert!(parse("text/").is_none());
        assert!(parse("te xt/html").is_none());
        assert!(parse("text/html\u{FFFD}").is_none());
    }

    #[test]
    fn drops_parameters_outside_the_quoted_string_token_code_points() {
        let mime_type = parse("text/plain;a=\u{100};b=\u{FF}").unwrap();
        assert_eq!(mime_type.serialize(), "text/plain;b=\"\u{FF}\"");
    }
}
