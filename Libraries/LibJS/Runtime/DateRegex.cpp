#include "DateRegex.h"

DateRegexGenerator::DatePatterns DateRegexGenerator::create_date_patterns()
{
    //  Base non-terminals
    DateRegex const COLON { ":"_string };
    DateRegex const COMMA { ","_string };
    DateRegex const DASH { "-"_string };
    DateRegex const DOT { "[.]"_string };
    DateRegex const LPAREN { "[(]"_string };
    DateRegex const PLUS { "+"_string };
    DateRegex const PUNCTSPACE { "[+-./, ]"_string };
    DateRegex const SLASH { "/"_string };
    DateRegex const SPACE { "[ ]"_string };

    DateRegex const T { "T"_string };
    DateRegex const Z { "Z"_string };
    DateRegex const GMT { "GMT"_string, Concatentation };
    DateRegex const UTC { "UTC"_string, Concatentation };

    DateRegex const ALPHASPACE { "[a-z ]+"_string };
    DateRegex const MAYBE_DIGITS { "[0-9]*"_string };
    DateRegex const MAYBE_SPACES { "[ ]*"_string };
    DateRegex const MAYBE_PUNCTSPACE { "[+-./, ]*"_string };
    DateRegex const NOT_RPAREN { "[^)]"_string };
    DateRegex const PLUS_OR_MINUS { "[+\\-]"_string };
    DateRegex const RPAREN { "[)]"_string };
    DateRegex const SOME_JUNK { "[A-Za-z][A-Za-z+-./,:]*"_string, Concatentation }; // Firefox garbage
    DateRegex const SOME_PUNCT { "[+-./,:]+"_string };                              // Firefox punctation
    DateRegex const SOME_SPACE { "[ ]+"_string };

    DateRegex const T024 { "[01][0-9]|2[0-4]"_string, Disjunction };      // 2-digit 24-hour
    DateRegex const T24 { "[01][0-9]|2[0-4]|[0-9]"_string, Disjunction }; // any number that can be an hour
    DateRegex const T012 { "0[0-9]|1[0-2]"_string, Disjunction };         // 2-digit month
    DateRegex const T12 { "[01][0-2]|[1-9]"_string, Disjunction };        // any number that can be a month
    DateRegex const T059 { "[0-5][0-9]"_string };                         // minutes or seconds; single-digit minutes or seconds are not supported
    DateRegex const T31 { "[12][0-9]|3[01]|1-9"_string, Disjunction };    // any number that can be a day
    DateRegex const T031 { "[0-2][0-9]|3[01]"_string, Disjunction };      // 2-digit day

    DateRegex const N03 { "[0-9]{3}"_string };  // 3-digit number (use milliseconds)
    DateRegex const N3 { "[0-9]{1,3}"_string }; // Number up to 3-digits
    DateRegex const N04 { "[0-9]{4}"_string };  // 4-digit number
    DateRegex const N4 { "[0-9]{1,4}"_string }; // Number up to 4-digits
    DateRegex const N06 { "[0-9]{6}"_string };  // 6-digit number (use for signed 6-digit iso8601 year)
    DateRegex const N6 { "[0-9]{1,6}"_string }; // Number up to 6-digits (use to catch free numbers)
    DateRegex const N9 { "[0-9]{1,9}"_string }; // Number up to 9-digits (use for nanoseconds)

    // 3-letter month name abbreviations. Strict matching in date_tostring and date_toutcstring.
    DateRegex const jan3 { DateRegex { "Jan"_string }.group(JAN) };
    DateRegex const feb3 { DateRegex { "Feb"_string }.group(FEB) };
    DateRegex const mar3 { DateRegex { "Mar"_string }.group(MAR) };
    DateRegex const apr3 { DateRegex { "Apr"_string }.group(APR) };
    DateRegex const may3 { DateRegex { "May"_string }.group(MAY) };
    DateRegex const jun3 { DateRegex { "Jun"_string }.group(JUN) };
    DateRegex const jul3 { DateRegex { "Jul"_string }.group(JUL) };
    DateRegex const aug3 { DateRegex { "Aug"_string }.group(AUG) };
    DateRegex const sep3 { DateRegex { "Sep"_string }.group(SEP) };
    DateRegex const oct3 { DateRegex { "Oct"_string }.group(OCT) };
    DateRegex const nov3 { DateRegex { "Nov"_string }.group(NOV) };
    DateRegex const dec3 { DateRegex { "Dec"_string }.group(DEC) };

    // Permissive month names. Firefox and Chrome read the first three characters then anything goes. We do the same.
    DateRegex const jan { DateRegex { "jan[a-z]{0,8}"_string }.group(JAN) };
    DateRegex const feb { DateRegex { "feb[a-z]{0,8}"_string }.group(FEB) };
    DateRegex const mar { DateRegex { "mar[a-z]{0,8}"_string }.group(MAR) };
    DateRegex const apr { DateRegex { "apr[a-z]{0,8}"_string }.group(APR) };
    DateRegex const may { DateRegex { "may[a-z]{0,8}"_string }.group(MAY) };
    DateRegex const jun { DateRegex { "jun[a-z]{0,8}"_string }.group(JUN) };
    DateRegex const jul { DateRegex { "jul[a-z]{0,8}"_string }.group(JUL) };
    DateRegex const aug { DateRegex { "aug[a-z]{0,8}"_string }.group(AUG) };
    DateRegex const sep { DateRegex { "sep[a-z]{0,8}"_string }.group(SEP) };
    DateRegex const oct { DateRegex { "oct[a-z]{0,8}"_string }.group(OCT) };
    DateRegex const nov { DateRegex { "nov[a-z]{0,8}"_string }.group(NOV) };
    DateRegex const dec { DateRegex { "dec[a-z]{0,8}"_string }.group(DEC) };

    // Only datetime_tostring and datetime_toutcstring confirm correct weekdays.
    // Firefox and Chrome simply discard weekdays -- you can write anything. We do the same.
    DateRegex const sun { "Sun"_string, Concatentation };
    DateRegex const mon { "Mon"_string, Concatentation };
    DateRegex const tue { "Tue"_string, Concatentation };
    DateRegex const wed { "Wed"_string, Concatentation };
    DateRegex const thu { "Thu"_string, Concatentation };
    DateRegex const fri { "Fri"_string, Concatentation };
    DateRegex const sat { "Sat"_string, Concatentation };

    // AM/PM
    DateRegex const am { DateRegex { "am"_string }.group(HHAM) };
    DateRegex const pm { DateRegex { "pm"_string }.group(HHPM) };

    // US mainland time zones.
    // Firefox and Chrome only catch these. We do the same.
    DateRegex const edt { DateRegex { "edt"_string }.group(EDT) };
    DateRegex const est { DateRegex { "est"_string }.group(EST) };
    DateRegex const cdt { DateRegex { "cdt"_string }.group(CDT) };
    DateRegex const cst { DateRegex { "cst"_string }.group(CST) };
    DateRegex const mdt { DateRegex { "mdt"_string }.group(MDT) };
    DateRegex const mst { DateRegex { "mst"_string }.group(MST) };
    DateRegex const pdt { DateRegex { "pdt"_string }.group(PDT) };
    DateRegex const pst { DateRegex { "pst"_string }.group(PST) };

    //  Date fragments
    DateRegex const signed_year = (PLUS_OR_MINUS + N06).group(YEAR); // Year with a sign and exactly six digits +123456. Use for iso8601 expanded years extension.
    DateRegex const year4 = N04.group(YEAR);                         // 4-digit year
    DateRegex const month = T012.group(MONTH);
    DateRegex const month_name = one_of(jan, feb, mar, apr, may, jun, jul, sep, oct, nov, dec);
    DateRegex const month_name3 = one_of(jan3, feb3, mar3, apr3, may3, jun3, jul3, sep3, oct3, nov3, dec3); // 3-letter month name. Use in date_tostring and date_toutcstring
    DateRegex const weekday = one_of(sun, mon, tue, wed, thu, fri, sat);
    DateRegex const day = T031.group(DAY);
    DateRegex const us_timezone = one_of(est, edt, cst, cdt, mst, mdt, pst, pdt);

    //  Time fragments
    DateRegex const hour = T024.group(HH);
    DateRegex const min = T059.group(MM);
    DateRegex const sec = T059.group(SS);
    DateRegex const ms = N03.group(MS);
    DateRegex const ms_relaxed = N3.group(MS_PERMISSIVE) + MAYBE_DIGITS;
    DateRegex const ampm = one_of(am, pm);

    //  Time zone fragments
    DateRegex const tzsign = PLUS_OR_MINUS.group(TZSIGN);
    DateRegex const tzhh = T024.group(TZHH);
    DateRegex const tzmin = T059.group(TZMM);
    DateRegex const tzsec = T059.group(TZSS);
    DateRegex const tzns = N9.group(TZNS);
    DateRegex const tzhhmin = N04.group(TZHHMM);

    // Fragments that support specific terminals.
    DateRegex const year_iso8601 = one_of(year4, signed_year); // https://tc39.es/ecma262/#sec-expanded-years
    // "The representation of the year 0 as -000000 is invalid." https://tc39.es/ecma262/#sec-expanded-years
    // Firefox interprets "-000000" as "Jan 1, 2000".
    // We do the same: stop parsing the input as ISO8601 format; mark the match as a number; the number is guessed later to be a year.
    DateRegex const invalid_minus_zero_year_iso8601 = DASH + DateRegex { "000000"_string }.group(NUMBER);

    // https://tc39.es/ecma262/#sec-time-zone-offset-strings
    // NOTE: At this time, neither Firefox nor Chrome support nanoseconds in the timezone offset. We do.
    DateRegex const tz_iso8601 = one_of(Z.group(TZUTC), one_of(tzsign + tzhh + COLON + tzmin + maybe(COLON + tzsec + maybe((DOT + tzns))), // extended format
                                                            tzsign + tzhhmin,                                                              // military timezone offset
                                                            // Both Chrome and Firefox fail when parsing "1980-05-30T13:40+[ ]1[2[3]]", but succeed on "1980-05-30 13:40+1"
                                                            // Explanation: based on what they've seen so far, they assume ISO8601 format (does not allow "permissive" military offset = anything less than 4 digits)
                                                            // and fail abruptly, without matching against other patterns.
                                                            // For "1980-05-30 13:40+1", they discard this as an ISO8601 format and match permissively.
                                                            // We do the same.
                                                            (MAYBE_SPACES + PLUS_OR_MINUS + N3).group(FAIL)));

    DateRegex const tzname_date_tostring = LPAREN + ALPHASPACE + RPAREN; // Stuff in brackets. Use in date_tostring

    // Permissive fragments, used for tokenizing the input string.
    DateRegex const gmt = one_of(GMT, UTC, Z).group(TZUTC);
    DateRegex const tzname_permissive = (LPAREN + NOT_RPAREN.plus() + maybe(RPAREN)).last();       // Permissive time zone name: almost anything after left parenthesis.
    DateRegex const number = N6.group(NUMBER);                                                     // A number with role (year/month/day) that will be guessed after matching.
    DateRegex const number4_with_sign = PLUS_OR_MINUS.group(SIGN) + N4.group(SIGNED_NUMBER);       // Either a plain number after a dash '-' or a military timezone offset.
    DateRegex const one_number = full(MAYBE_PUNCTSPACE + N6.group(ONE_NUMBER) + MAYBE_PUNCTSPACE); // The whole string is just one number "2022" or "12".

    DateRegex const time_with_colon = // H[H]:MM[:SS[.MSs...]]
        maybe(T) + T24.group(HH) + COLON + MAYBE_SPACES + maybe(min + maybe(COLON + MAYBE_SPACES + sec + maybe(DOT + ms_relaxed))) + MAYBE_SPACES + maybe(ampm);
    DateRegex const tz_with_colon = // +H[H]:MM[:SS[.Ns]]
        maybe(gmt) + MAYBE_SPACES + PLUS_OR_MINUS.group(TZSIGN) + tzhh + COLON + tzmin + maybe(COLON + tzsec + maybe(DOT + tzns));
    DateRegex const colon_guard = (N03 + COLON).group(FAIL); // reject 123:

    DateRegex const gmt_military_offset = gmt + MAYBE_SPACES + PLUS_OR_MINUS.group(TZSIGN) + N4.group(TZHHMM) + maybe(N3.group(FAIL)); // GMT+1[2[3[4]]]

    // Terminals
    // https://tc39.es/ecma262/#sec-date-time-string-format
    DateRegex const ecma_script_datetime = year4.group(ISO8601) + maybe(DASH + month + maybe(DASH + day)) + maybe(T + hour + COLON + min + maybe(COLON + sec + maybe(DOT + ms)) + maybe(one_of(Z.group(TZUTC), (tzsign + tzhh + COLON + tzmin))));

    // ecma_script_date_time plus iso8601 extensions.
    // https://tc39.es/ecma262/#sec-date-time-string-format
    DateRegex const iso8601_simplified_date_time = one_of(invalid_minus_zero_year_iso8601, full(year_iso8601.group(ISO8601) + maybe(DASH + month + maybe(DASH + day)) + maybe(T + hour + COLON + min + maybe(COLON + sec + maybe(DOT + ms)) + maybe(tz_iso8601))));

    DateRegex const date_tostring = // Wed Dec 08 1999 00:00:00 GMT-0800 (Pacific Standard Time) https://tc39.es/ecma262/#sec-todatestring
        weekday + SPACE + month_name3 + SPACE + day + SPACE + year4 + SPACE + hour + COLON + min + COLON + sec + SPACE + GMT.group(TZUTC) + maybe(tzsign + tzhhmin + maybe(SPACE + tzname_date_tostring));

    DateRegex const date_toutcstring = // Wed, 08 Dec 1999 08:00:00 GMT  https://tc39.es/ecma262/#sec-date.prototype.toutcstring
        weekday + COMMA + SPACE + day + SPACE + month_name3 + SPACE + year4 + SPACE + hour + COLON + min + COLON + sec + SPACE + GMT.group(TZUTC);

    // Tokenizer, based on a collection of permissive fragments. Order matters; first match, greedy.
    DateRegex const permissive = one_of(
        iso8601_simplified_date_time, // Attempt to match iso8601 format. If that fails, tokenize the string.

        colon_guard, // Must come before time_with_colon, otherwise matches 1{23:12:12}

        time_with_colon,
        tz_with_colon,
        gmt_military_offset, // Must come after tz_with_colon, otherwise matches {GMT+12}:00

        one_number,
        signed_year,
        number4_with_sign,
        number,

        month_name,
        us_timezone,
        gmt,
        tzname_permissive,

        SOME_PUNCT.group(PUNCT), // Punctuation
        SOME_JUNK.group(JUNK),   // Junk. Ignore at the beginning, fail
        SOME_SPACE               // do nothing on spaces
    );

    return DateRegexGenerator::DatePatterns {
        .ecma_script_datetime = full(ecma_script_datetime),
        .iso8601_simplified_datetime = iso8601_simplified_date_time,
        .date_tostring = full(date_tostring),
        .date_toutcstring = full(date_toutcstring),

        .permissive = permissive, // permissive
    };
}
