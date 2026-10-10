/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Options for the optimizing JIT, from the LIBJS_JIT environment variable, which the runtime reads when it creates a
//! VM. It holds comma-separated options, each one of `DEFINITIONS`; "LIBJS_JIT=help" lists them with the values in
//! effect.

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Options {
    /// Whether the interpreter's profiling tier runs, which collects feedback for the JIT.
    pub enabled: bool,

    /// How hot an executable must get before it tiers up, in function invocations. Loop iterations count as a
    /// fraction of an invocation.
    pub threshold: u32,

    /// How warm an executable must get before the interpreter collects feedback for it, measured like the threshold.
    /// The threshold counts from there.
    pub warmup: u32,

    /// List the options and the values in effect on stderr.
    pub help: bool,

    /// Print the interpreter feedback of each executable to stderr the first time it runs out of tier-up budget.
    pub dump_feedback: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            enabled: false,
            threshold: 400,
            warmup: 8,
            help: false,
            dump_feedback: false,
        }
    }
}

/// What an option sets.
enum Setting {
    /// Whether the profiling tier runs.
    Enabled(bool),
    /// A switch that the option turns on.
    Switch(fn(&mut Options) -> &mut bool),
    /// A number, given as "name=N".
    Number(fn(&mut Options) -> &mut u32),
}

/// An option LIBJS_JIT may hold.
struct Definition {
    name: &'static str,
    setting: Setting,
    description: &'static str,
}

const DEFINITIONS: &[Definition] = &[
    Definition {
        name: "on",
        setting: Setting::Enabled(true),
        description: "Run the profiling tier of the interpreter, which collects feedback.",
    },
    Definition {
        name: "off",
        setting: Setting::Enabled(false),
        description: "Run everything in the plain interpreter, which collects no feedback (the default).",
    },
    Definition {
        name: "help",
        setting: Setting::Switch(|options| &mut options.help),
        description: "List the options and the values in effect.",
    },
    Definition {
        name: "threshold",
        setting: Setting::Number(|options| &mut options.threshold),
        description: "How many invocations make a function hot, after its warm-up. Loop iterations count as a fraction of one.",
    },
    Definition {
        name: "dump-feedback",
        setting: Setting::Switch(|options| &mut options.dump_feedback),
        description: "Run the profiling tier, and print the feedback of each function the first time it gets hot.",
    },
    Definition {
        name: "warmup",
        setting: Setting::Number(|options| &mut options.warmup),
        description: "How many invocations a function runs before the interpreter collects feedback for it.",
    },
];

impl Definition {
    /// How the option is written.
    fn syntax(&self) -> String {
        match self.setting {
            Setting::Enabled(_) | Setting::Switch(_) => self.name.to_string(),
            Setting::Number(_) => format!("{}=N", self.name),
        }
    }

    /// Sets the option in `options`, with the argument after its "=", if it has one.
    fn apply(&self, options: &mut Options, argument: Option<&str>) -> Result<(), String> {
        let syntax = self.syntax();
        match (&self.setting, argument) {
            (Setting::Enabled(enabled), None) => options.enabled = *enabled,
            (Setting::Switch(field), None) => *field(options) = true,
            (Setting::Number(field), Some(text)) => {
                *field(options) = text
                    .parse()
                    .map_err(|_| format!("{syntax} needs a whole number from 0 to {}, not '{text}'", u32::MAX))?;
            }
            (Setting::Number(_), None) => return Err(format!("{syntax} needs a value")),
            (Setting::Enabled(_) | Setting::Switch(_), Some(_)) => {
                return Err(format!("{syntax} takes no value"));
            }
        }
        Ok(())
    }

    /// The option as it would be written to give `options` its value, if it is one that has a value.
    fn effective(&self, options: &Options) -> Option<String> {
        let mut options = options.clone();
        match self.setting {
            Setting::Enabled(enabled) => (options.enabled == enabled).then(|| self.name.to_string()),
            Setting::Switch(field) => (*field(&mut options)).then(|| self.name.to_string()),
            Setting::Number(field) => Some(format!("{}={}", self.name, *field(&mut options))),
        }
    }
}

impl Options {
    /// The options LIBJS_JIT holds, or the defaults if it is not set. A value with an option that is not one of
    /// these, or not written as one, is rejected: the process exits with a message that says why. With "help", the
    /// options are listed on stderr.
    pub fn from_environment() -> Self {
        let Ok(value) = std::env::var("LIBJS_JIT") else {
            return Self::default();
        };
        match Self::parse(&value) {
            Ok(options) => {
                if options.help {
                    eprint!("{}", options.help_text());
                }
                options
            }
            Err(error) => {
                eprintln!("LIBJS_JIT: {error} (LIBJS_JIT=help lists the options)");
                std::process::exit(1);
            }
        }
    }

    /// The options a LIBJS_JIT value holds, from the defaults on, left to right, or why it is not a valid value.
    pub fn parse(value: &str) -> Result<Self, String> {
        let mut options = Self::default();
        for option in value.split(',').map(str::trim).filter(|option| !option.is_empty()) {
            let (name, argument) = match option.split_once('=') {
                Some((name, argument)) => (name, Some(argument)),
                None => (option, None),
            };
            let definition = DEFINITIONS
                .iter()
                .find(|definition| definition.name == name)
                .ok_or_else(|| format!("Unknown option '{option}'"))?;
            definition.apply(&mut options, argument)?;
        }
        Ok(options)
    }

    /// The options as a LIBJS_JIT value: the ones that have values, and the switches that are on.
    pub fn effective_value(&self) -> String {
        DEFINITIONS
            .iter()
            .filter_map(|definition| definition.effective(self))
            .collect::<Vec<_>>()
            .join(",")
    }

    /// The list of options "help" prints, with the options in effect.
    pub fn help_text(&self) -> String {
        let mut help = String::from("LIBJS_JIT holds comma-separated options for the optimizing JIT:\n");
        for definition in DEFINITIONS {
            help.push_str(&format!("  {:<22} {}\n", definition.syntax(), definition.description));
        }
        help.push_str(&format!("In effect: LIBJS_JIT={}\n", self.effective_value()));
        help
    }

    /// Whether the interpreter collects feedback and counts down tier-up budgets. Only the JIT and the feedback dump
    /// need either.
    pub fn collects_feedback(&self) -> bool {
        self.enabled || self.dump_feedback
    }
}
