/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Options for the optimizing JIT, from the LIBJS_JIT environment variable, which the runtime reads when it creates a
//! VM. It holds comma-separated options, each one of `DEFINITIONS`; "LIBJS_JIT=help" lists them with the values in
//! effect. The Configuration section of Libraries/LibJS/JIT/ARCHITECTURE.md describes them.

/// How many checks JIT code runs at most between two exits with a bare "stress-exits".
const DEFAULT_STRESS_EXIT_PERIOD: u32 = 20;

/// How many tier-up checks happen at most between two invalidations with a bare "stress-invalidate".
const DEFAULT_STRESS_INVALIDATION_PERIOD: u32 = 10;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Options {
    pub enabled: bool,

    /// How hot an executable must get before it tiers up, in function invocations. Loop iterations count as a
    /// fraction of an invocation.
    pub threshold: u32,

    /// How warm an executable must get before the interpreter collects feedback for it, measured like the threshold.
    /// The threshold counts from there.
    pub warmup: u32,

    /// Compile synchronously when an executable tiers up.
    pub sync: bool,

    /// List the options and the values in effect on stderr.
    pub help: bool,

    pub dump_ir: bool,
    /// Dump the IR after building it and after every pass.
    pub dump_passes: bool,
    pub dump_asm: bool,
    /// Check the invariants of the IR after building and after every optimization pass, also in release builds.
    pub verify_ir: bool,

    /// Print the interpreter feedback of each executable to stderr the first time it runs out of tier-up budget.
    pub dump_feedback: bool,

    /// Seeds the pseudo-random choices of the stress options, so that a run with the same options and the same seed
    /// makes the same choices (with "sync", whose compiles happen at the same points of every run).
    pub seed: u64,

    /// Exit from JIT code at every Nth check that can exit it runs, where N is chosen at random between 1 and this
    /// after each such exit, or never if 0. These exits teach the executable nothing: they neither record exit sites
    /// nor count towards discarding its code.
    pub stress_exits: u32,

    /// Give compiled code an on-stack replacement entry at every loop back edge that ran, not only at the one whose
    /// tier-up budget ran out.
    pub stress_osr: bool,

    /// Let the register allocator assign only the registers calls take their arguments and return their results in.
    pub stress_registers: bool,

    /// Give each executable a random tier-up budget between none and the one "threshold" and "warmup" give it.
    pub random_thresholds: bool,

    /// Invalidate the code depending on a random dependency at every Nth tier-up check, where N is chosen at random
    /// between 1 and this after each invalidation, or never if 0, as if the dependency stopped holding.
    pub stress_invalidate: u32,

    /// Install the code of each asynchronous compile only after a random number of later tier-up checks, from 0 to
    /// 16, instead of at the first one after it finished.
    pub stress_install: bool,

    /// Print every exit from JIT code to the interpreter, and every discard of JIT code, to stderr.
    pub log_exits: bool,

    /// Describe installed code in /tmp/perf-<pid>.map for perf.
    pub perf_map: bool,

    /// How many bytecode instructions the inlining candidates of one compile job that are not always inlined may have
    /// together.
    pub inline_budget: u32,

    /// How many bytecode instructions one inlining candidate may have.
    pub inline_max_size: u32,

    /// How deeply inlined calls may nest. 0 turns inlining off.
    pub inline_depth: u32,

    /// Where to write what compiled code contained and what it did, as one JSON file per VM.
    pub coverage: Option<String>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            enabled: true,
            threshold: 400,
            warmup: 8,
            sync: false,
            help: false,
            dump_ir: false,
            dump_passes: false,
            dump_asm: false,
            verify_ir: false,
            dump_feedback: false,
            seed: 1,
            stress_exits: 0,
            stress_osr: false,
            stress_registers: false,
            random_thresholds: false,
            stress_install: false,
            stress_invalidate: 0,
            log_exits: false,
            perf_map: false,
            inline_budget: 250,
            inline_max_size: 30,
            inline_depth: 5,
            coverage: None,
        }
    }
}

/// What an option sets.
enum Setting {
    /// Whether the JIT runs.
    Enabled(bool),
    /// A switch that the option turns on.
    Switch(fn(&mut Options) -> &mut bool),
    /// A number, given as "name=N", or with a bare value, also as "name".
    Number {
        field: fn(&mut Options) -> &mut u32,
        bare: Option<u32>,
    },
    /// The seed, given as "seed=N".
    Seed,
    /// The coverage directory, given as "coverage=DIRECTORY".
    Directory,
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
        description: "Compile hot functions (the default).",
    },
    Definition {
        name: "off",
        setting: Setting::Enabled(false),
        description: "Run everything in the interpreter, which then collects no feedback.",
    },
    Definition {
        name: "help",
        setting: Setting::Switch(|options| &mut options.help),
        description: "List the options and the values in effect.",
    },
    Definition {
        name: "threshold",
        setting: Setting::Number {
            field: |options| &mut options.threshold,
            bare: None,
        },
        description: "How many invocations make a function hot, after its warm-up. Loop iterations count as a fraction of one.",
    },
    Definition {
        name: "warmup",
        setting: Setting::Number {
            field: |options| &mut options.warmup,
            bare: None,
        },
        description: "How many invocations a function runs before the interpreter collects feedback for it.",
    },
    Definition {
        name: "sync",
        setting: Setting::Switch(|options| &mut options.sync),
        description: "Compile on the main thread when a function gets hot, at the same points in every run.",
    },
    Definition {
        name: "inline-budget",
        setting: Setting::Number {
            field: |options| &mut options.inline_budget,
            bare: None,
        },
        description: "How many bytecode instructions the inlined callees of one compile may have together.",
    },
    Definition {
        name: "inline-max-size",
        setting: Setting::Number {
            field: |options| &mut options.inline_max_size,
            bare: None,
        },
        description: "How many bytecode instructions one inlined callee may have.",
    },
    Definition {
        name: "inline-depth",
        setting: Setting::Number {
            field: |options| &mut options.inline_depth,
            bare: None,
        },
        description: "How deeply inlined calls may nest; 0 turns inlining off.",
    },
    Definition {
        name: "dump-ir",
        setting: Setting::Switch(|options| &mut options.dump_ir),
        description: "Print the IR of every compile.",
    },
    Definition {
        name: "dump-passes",
        setting: Setting::Switch(|options| &mut options.dump_passes),
        description: "Print the IR after building it and after every pass.",
    },
    Definition {
        name: "dump-asm",
        setting: Setting::Switch(|options| &mut options.dump_asm),
        description: "Print the machine code of every compile.",
    },
    Definition {
        name: "dump-feedback",
        setting: Setting::Switch(|options| &mut options.dump_feedback),
        description: "Print the interpreter feedback of each function the first time it gets hot.",
    },
    Definition {
        name: "verify-ir",
        setting: Setting::Switch(|options| &mut options.verify_ir),
        description: "Check the invariants of the IR after building it and after every pass.",
    },
    Definition {
        name: "log-exits",
        setting: Setting::Switch(|options| &mut options.log_exits),
        description: "Print every exit from compiled code to the interpreter, and every discard of compiled code.",
    },
    Definition {
        name: "perf-map",
        setting: Setting::Switch(|options| &mut options.perf_map),
        description: "Describe compiled code in /tmp/perf-<pid>.map for perf.",
    },
    Definition {
        name: "coverage",
        setting: Setting::Directory,
        description: "Write what compiled code contained and did into DIRECTORY, one JSON file per VM.",
    },
    Definition {
        name: "seed",
        setting: Setting::Seed,
        description: "Seed the random choices of the stress options, which then repeat with \"sync\".",
    },
    Definition {
        name: "stress-exits",
        setting: Setting::Number {
            field: |options| &mut options.stress_exits,
            bare: Some(DEFAULT_STRESS_EXIT_PERIOD),
        },
        description: "Exit from compiled code at a random one of every N checks (bare: 20); 0 never does.",
    },
    Definition {
        name: "stress-osr",
        setting: Setting::Switch(|options| &mut options.stress_osr),
        description: "Give compiled code an on-stack replacement entry at every loop that ran.",
    },
    Definition {
        name: "stress-registers",
        setting: Setting::Switch(|options| &mut options.stress_registers),
        description: "Let the register allocator use only the registers of call arguments and results.",
    },
    Definition {
        name: "random-thresholds",
        setting: Setting::Switch(|options| &mut options.random_thresholds),
        description: "Give each function a random part of its warm-up and threshold.",
    },
    Definition {
        name: "stress-install",
        setting: Setting::Switch(|options| &mut options.stress_install),
        description: "Install the code of asynchronous compiles after a random number of later tier-up checks.",
    },
    Definition {
        name: "stress-invalidate",
        setting: Setting::Number {
            field: |options| &mut options.stress_invalidate,
            bare: Some(DEFAULT_STRESS_INVALIDATION_PERIOD),
        },
        description: "Invalidate code as if a random dependency stopped holding, at a random one of every N tier-up checks (bare: 10); 0 never does.",
    },
];

impl Definition {
    /// How the option is written.
    fn syntax(&self) -> String {
        match self.setting {
            Setting::Enabled(_) | Setting::Switch(_) => self.name.to_string(),
            Setting::Number { bare: None, .. } | Setting::Seed => format!("{}=N", self.name),
            Setting::Number { bare: Some(_), .. } => format!("{}[=N]", self.name),
            Setting::Directory => format!("{}=DIRECTORY", self.name),
        }
    }

    /// Sets the option in `options`, with the argument after its "=", if it has one.
    fn apply(&self, options: &mut Options, argument: Option<&str>) -> Result<(), String> {
        let syntax = self.syntax();
        match (&self.setting, argument) {
            (Setting::Enabled(enabled), None) => options.enabled = *enabled,
            (Setting::Switch(field), None) => *field(options) = true,
            (Setting::Number { field, bare }, argument) => {
                *field(options) = match (argument, bare) {
                    (Some(text), _) => text
                        .parse()
                        .map_err(|_| format!("{syntax} needs a whole number from 0 to {}, not '{text}'", u32::MAX))?,
                    (None, Some(bare)) => *bare,
                    (None, None) => return Err(format!("{syntax} needs a value")),
                };
            }
            (Setting::Seed, Some(text)) => {
                options.seed = text
                    .parse()
                    .map_err(|_| format!("{syntax} needs a whole number from 0 to {}, not '{text}'", u64::MAX))?;
            }
            (Setting::Directory, Some(directory)) if !directory.is_empty() => {
                options.coverage = Some(directory.to_string());
            }
            (Setting::Seed | Setting::Directory, _) => return Err(format!("{syntax} needs a value")),
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
            Setting::Number { field, .. } => Some(format!("{}={}", self.name, *field(&mut options))),
            Setting::Seed => Some(format!("{}={}", self.name, options.seed)),
            Setting::Directory => options
                .coverage
                .as_ref()
                .map(|directory| format!("{}={directory}", self.name)),
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults() {
        assert_eq!(Options::parse(""), Ok(Options::default()));
        assert_eq!(Options::parse(" , ,"), Ok(Options::default()));
    }

    #[test]
    fn options_apply_left_to_right() {
        let options = Options::parse("off,threshold=5, warmup=0 ,sync,on,threshold=7").unwrap();
        assert!(options.enabled);
        assert!(options.sync);
        assert_eq!(options.threshold, 7);
        assert_eq!(options.warmup, 0);
        assert!(!Options::parse("on,off").unwrap().enabled);
    }

    #[test]
    fn every_option_parses() {
        let options = Options::parse(
            "threshold=1,warmup=2,sync,inline-budget=3,inline-max-size=4,inline-depth=0,dump-ir,dump-passes,dump-asm,\
             dump-feedback,verify-ir,log-exits,perf-map,coverage=/tmp/coverage,seed=18446744073709551615,\
             stress-exits=6,stress-osr,stress-registers,random-thresholds,stress-install,stress-invalidate=7,help",
        )
        .unwrap();
        let expected = Options {
            enabled: true,
            threshold: 1,
            warmup: 2,
            sync: true,
            help: true,
            dump_ir: true,
            dump_passes: true,
            dump_asm: true,
            verify_ir: true,
            dump_feedback: true,
            seed: u64::MAX,
            stress_exits: 6,
            stress_osr: true,
            stress_registers: true,
            random_thresholds: true,
            stress_install: true,
            stress_invalidate: 7,
            log_exits: true,
            perf_map: true,
            inline_budget: 3,
            inline_max_size: 4,
            inline_depth: 0,
            coverage: Some("/tmp/coverage".to_string()),
        };
        assert_eq!(options, expected);
        // The effective value gives the same options again.
        assert_eq!(Options::parse(&options.effective_value()), Ok(expected));
    }

    #[test]
    fn bare_stress_options_have_default_periods() {
        let options = Options::parse("stress-exits,stress-invalidate").unwrap();
        assert_eq!(options.stress_exits, DEFAULT_STRESS_EXIT_PERIOD);
        assert_eq!(options.stress_invalidate, DEFAULT_STRESS_INVALIDATION_PERIOD);
    }

    #[test]
    fn garbage_is_rejected() {
        for (value, error) in [
            ("bogus", "Unknown option 'bogus'"),
            (
                "threshold=1=2",
                "threshold=N needs a whole number from 0 to 4294967295, not '1=2'",
            ),
            ("threshold", "threshold=N needs a value"),
            (
                "threshold=",
                "threshold=N needs a whole number from 0 to 4294967295, not ''",
            ),
            (
                "threshold=-1",
                "threshold=N needs a whole number from 0 to 4294967295, not '-1'",
            ),
            (
                "threshold=4294967296",
                "threshold=N needs a whole number from 0 to 4294967295, not '4294967296'",
            ),
            (
                "warmup=1.5",
                "warmup=N needs a whole number from 0 to 4294967295, not '1.5'",
            ),
            (
                "stress-exits=x",
                "stress-exits[=N] needs a whole number from 0 to 4294967295, not 'x'",
            ),
            ("seed", "seed=N needs a value"),
            (
                "seed=x",
                "seed=N needs a whole number from 0 to 18446744073709551615, not 'x'",
            ),
            ("coverage=", "coverage=DIRECTORY needs a value"),
            ("sync=1", "sync takes no value"),
            ("on=1", "on takes no value"),
            ("ON", "Unknown option 'ON'"),
        ] {
            assert_eq!(Options::parse(value), Err(error.to_string()), "LIBJS_JIT={value}");
        }
    }

    #[test]
    fn help_lists_every_option_and_the_effective_value() {
        let help = Options::parse("threshold=50").unwrap().help_text();
        for definition in DEFINITIONS {
            assert!(help.contains(&definition.syntax()), "{}", definition.name);
        }
        assert!(help.ends_with(
            "In effect: LIBJS_JIT=on,threshold=50,warmup=8,inline-budget=250,inline-max-size=30,inline-depth=5,\
             seed=1,stress-exits=0,stress-invalidate=0\n"
        ));
    }
}
