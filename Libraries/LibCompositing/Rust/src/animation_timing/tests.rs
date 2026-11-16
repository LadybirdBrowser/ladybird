/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::easing::{Easing, STEP_POSITION_JUMP_START};

fn ms(value: f64) -> TimeValue {
    TimeValue::milliseconds(value)
}

fn percent(value: f64) -> TimeValue {
    TimeValue::percentage(value)
}

/// A one-second effect on a document timeline.
fn one_second() -> AnimationEffectTiming {
    normalize_specified_timing(
        &SpecifiedEffectTiming {
            iteration_duration: SpecifiedIterationDuration::milliseconds(1000.0),
            ..SpecifiedEffectTiming::default()
        },
        None,
        TimeValueType::Milliseconds,
    )
}

/// An animation that started at zero and plays at `playback_rate`.
fn playing(playback_rate: f64) -> AnimationPlayback {
    AnimationPlayback {
        start_time: Some(ms(0.0)).into(),
        playback_rate,
        ..AnimationPlayback::default()
    }
}

fn document_timeline(time: f64) -> TimelineSample {
    TimelineSample {
        current_time: Some(ms(time)).into(),
        is_monotonically_increasing: true,
        ..TimelineSample::default()
    }
}

fn scroll_timeline(progress: f64) -> TimelineSample {
    TimelineSample {
        current_time: Some(percent(progress)).into(),
        minimum_timeline_time: Some(percent(0.0)).into(),
        maximum_timeline_time: Some(percent(100.0)).into(),
        is_monotonically_increasing: false,
    }
}

/// The resolved timing of `timing` played by `animation` with its timeline at `timeline`.
fn resolved_at(
    timing: &AnimationEffectTiming,
    animation: &AnimationPlayback,
    timeline: &TimelineSample,
) -> ResolvedEffectTiming {
    let local_time = current_time_at(animation, timeline.current_time.get());
    resolve_timing(timing, local_time, Some(animation), Some(timeline))
}

/// The transformed progress of `timing` played at rate one from zero, at `time`.
fn progress_at(timing: &AnimationEffectTiming, time: f64) -> Option<f64> {
    let resolved = resolved_at(timing, &playing(1.0), &document_timeline(time));
    transformed_progress(timing, &resolved, &Easing::default())
}

#[test]
fn normalizing_without_a_timeline_duration_keeps_the_specified_times() {
    let specified = SpecifiedEffectTiming {
        start_delay: 100.0,
        end_delay: 50.0,
        iteration_duration: SpecifiedIterationDuration::milliseconds(1000.0),
        iteration_count: 2.0,
        ..SpecifiedEffectTiming::default()
    };
    let timing = normalize_specified_timing(&specified, None, TimeValueType::Milliseconds);
    assert_eq!(timing.start_delay, ms(100.0));
    assert_eq!(timing.end_delay, ms(50.0));
    assert_eq!(timing.iteration_duration, ms(1000.0));
    assert_eq!(active_duration(&timing), ms(2000.0));
    assert_eq!(end_time(&timing), ms(2150.0));

    let auto = SpecifiedEffectTiming {
        iteration_duration: SpecifiedIterationDuration::AUTO,
        ..specified
    };
    let timing = normalize_specified_timing(&auto, None, TimeValueType::Milliseconds);
    assert_eq!(
        timing.iteration_duration,
        ms(0.0),
        "auto is the intrinsic iteration duration"
    );
}

#[test]
fn normalizing_against_a_timeline_duration_makes_the_times_proportional() {
    let specified = SpecifiedEffectTiming {
        start_delay: 250.0,
        end_delay: 250.0,
        iteration_duration: SpecifiedIterationDuration::milliseconds(500.0),
        ..SpecifiedEffectTiming::default()
    };
    let timing = normalize_specified_timing(&specified, Some(percent(100.0)), TimeValueType::Percentage);
    assert_eq!(timing.start_delay, percent(25.0));
    assert_eq!(timing.iteration_duration, percent(50.0));
    assert_eq!(timing.end_delay, percent(25.0));

    let auto = SpecifiedEffectTiming {
        iteration_duration: SpecifiedIterationDuration::AUTO,
        iteration_count: 4.0,
        ..specified
    };
    let timing = normalize_specified_timing(&auto, Some(percent(100.0)), TimeValueType::Percentage);
    assert_eq!(timing.start_delay, percent(0.0), "auto drops the delays");
    assert_eq!(timing.iteration_duration, percent(25.0));

    let empty = SpecifiedEffectTiming {
        start_delay: 0.0,
        end_delay: 0.0,
        iteration_duration: SpecifiedIterationDuration::milliseconds(0.0),
        ..SpecifiedEffectTiming::default()
    };
    let timing = normalize_specified_timing(&empty, Some(percent(100.0)), TimeValueType::Percentage);
    assert_eq!(
        timing.iteration_duration,
        percent(0.0),
        "no division by a zero total time"
    );
    assert_eq!(timing.start_delay, percent(0.0));
}

#[test]
fn normalizing_infinite_factors_against_a_timeline_duration_stays_finite() {
    let zero_duration_repeating_forever = SpecifiedEffectTiming {
        iteration_duration: SpecifiedIterationDuration::milliseconds(0.0),
        iteration_count: f64::INFINITY,
        ..SpecifiedEffectTiming::default()
    };
    let timing = normalize_specified_timing(
        &zero_duration_repeating_forever,
        Some(percent(100.0)),
        TimeValueType::Percentage,
    );
    assert_eq!(timing.iteration_duration, percent(0.0));
    assert_eq!(end_time(&timing), percent(0.0));

    let infinite_duration = SpecifiedEffectTiming {
        start_delay: 100.0,
        iteration_duration: SpecifiedIterationDuration::milliseconds(f64::INFINITY),
        ..SpecifiedEffectTiming::default()
    };
    let timing = normalize_specified_timing(&infinite_duration, Some(percent(100.0)), TimeValueType::Percentage);
    assert_eq!(timing.start_delay, percent(0.0));
    assert_eq!(timing.iteration_duration, percent(100.0));
    let animation = AnimationPlayback {
        start_time: Some(percent(0.0)).into(),
        ..playing(1.0)
    };
    let resolved = resolved_at(&timing, &animation, &scroll_timeline(50.0));
    assert_eq!(transformed_progress(&timing, &resolved, &Easing::default()), Some(0.5));

    let repeating_forever = SpecifiedEffectTiming {
        iteration_duration: SpecifiedIterationDuration::milliseconds(1000.0),
        iteration_count: f64::INFINITY,
        ..SpecifiedEffectTiming::default()
    };
    let timing = normalize_specified_timing(&repeating_forever, Some(percent(100.0)), TimeValueType::Percentage);
    assert_eq!(timing.iteration_duration, percent(0.0));
}

#[test]
fn an_infinite_duration_never_iterated_has_no_active_duration() {
    let timing = AnimationEffectTiming {
        iteration_duration: ms(f64::INFINITY),
        iteration_count: 0.0,
        ..AnimationEffectTiming::default()
    };
    assert_eq!(active_duration(&timing), ms(0.0));
}

#[test]
fn the_phase_follows_the_boundary_times_and_the_direction() {
    let timing = AnimationEffectTiming {
        start_delay: ms(100.0),
        ..one_second()
    };
    assert_eq!(before_active_boundary_time(&timing), ms(100.0));
    assert_eq!(after_active_boundary_time(&timing), ms(1100.0));
    let phase_at = |time: f64, direction| {
        phase(
            &timing,
            Some(ms(time)),
            direction,
            false,
            EndpointInclusiveActiveInterval::No,
        )
    };
    assert_eq!(
        phase(
            &timing,
            None,
            AnimationDirection::Forwards,
            false,
            EndpointInclusiveActiveInterval::No
        ),
        AnimationEffectPhase::Idle
    );
    assert_eq!(
        phase_at(50.0, AnimationDirection::Forwards),
        AnimationEffectPhase::Before
    );
    assert_eq!(
        phase_at(100.0, AnimationDirection::Forwards),
        AnimationEffectPhase::Active
    );
    assert_eq!(
        phase_at(100.0, AnimationDirection::Backwards),
        AnimationEffectPhase::Before
    );
    assert_eq!(
        phase_at(1100.0, AnimationDirection::Forwards),
        AnimationEffectPhase::After
    );
    assert_eq!(
        phase_at(1100.0, AnimationDirection::Backwards),
        AnimationEffectPhase::Active
    );
    assert_eq!(
        phase(
            &timing,
            Some(ms(1100.0)),
            AnimationDirection::Forwards,
            true,
            EndpointInclusiveActiveInterval::No
        ),
        AnimationEffectPhase::Active,
        "an effect at the timeline boundary stays active"
    );
    for (time, direction) in [
        (100.0, AnimationDirection::Backwards),
        (1100.0, AnimationDirection::Forwards),
    ] {
        assert_eq!(
            phase(
                &timing,
                Some(ms(time)),
                direction,
                false,
                EndpointInclusiveActiveInterval::Yes
            ),
            AnimationEffectPhase::Active,
            "an endpoint-inclusive active interval includes its endpoints"
        );
    }
}

#[test]
fn a_progress_based_timeline_is_at_its_boundary_within_tolerance() {
    assert!(is_at_timeline_boundary(Some(&scroll_timeline(100.0 - 1e-7))));
    assert!(is_at_timeline_boundary(Some(&scroll_timeline(0.0))));
    assert!(!is_at_timeline_boundary(Some(&scroll_timeline(50.0))));
    assert!(!is_at_timeline_boundary(Some(&document_timeline(0.0))));
    assert!(!is_at_timeline_boundary(None));

    let timing = normalize_specified_timing(
        &SpecifiedEffectTiming {
            fill_mode: FillMode::None,
            ..SpecifiedEffectTiming::default()
        },
        Some(percent(100.0)),
        TimeValueType::Percentage,
    );
    let animation = AnimationPlayback {
        start_time: Some(percent(0.0)).into(),
        ..playing(1.0)
    };
    let resolved = resolved_at(&timing, &animation, &scroll_timeline(100.0));
    assert_eq!(resolved.phase, AnimationEffectPhase::Active);
    assert_eq!(transformed_progress(&timing, &resolved, &Easing::default()), Some(1.0));
}

#[test]
fn the_active_time_fills_as_the_fill_mode_says() {
    let timing = AnimationEffectTiming {
        start_delay: ms(100.0),
        ..one_second()
    };
    let at = |time: f64, fill_mode| {
        let local_time = Some(ms(time));
        let phase = phase(
            &timing,
            local_time,
            AnimationDirection::Forwards,
            false,
            EndpointInclusiveActiveInterval::No,
        );
        active_time(&timing, phase, local_time, fill_mode)
    };
    assert_eq!(at(50.0, FillMode::None), None);
    assert_eq!(at(50.0, FillMode::Auto), None);
    assert_eq!(at(50.0, FillMode::Forwards), None);
    assert_eq!(at(50.0, FillMode::Backwards), Some(ms(0.0)));
    assert_eq!(at(600.0, FillMode::None), Some(ms(500.0)));
    assert_eq!(at(1500.0, FillMode::Backwards), None);
    assert_eq!(at(1500.0, FillMode::Both), Some(ms(1000.0)));
}

#[test]
fn progress_follows_the_timeline_through_the_active_phase() {
    let timing = one_second();
    assert_eq!(progress_at(&timing, 250.0), Some(0.25));
    assert_eq!(progress_at(&timing, 0.0), Some(0.0));
    assert_eq!(progress_at(&timing, 1500.0), None, "no fill after the end");
    assert_eq!(progress_at(&timing, -1.0), None, "no fill before the start");
}

#[test]
fn progress_fills_at_the_ends() {
    let timing = AnimationEffectTiming {
        fill_mode: FillMode::Both,
        ..one_second()
    };
    assert_eq!(progress_at(&timing, 1500.0), Some(1.0));
    assert_eq!(progress_at(&timing, -500.0), Some(0.0));

    let resolved = resolved_at(&timing, &playing(1.0), &document_timeline(1500.0));
    assert_eq!(simple_iteration_progress(&timing, &resolved), Some(1.0));
    assert_eq!(
        current_iteration(&timing, &resolved),
        Some(0.0),
        "the last iteration ends at 1.0"
    );
}

#[test]
fn a_hold_time_wins_over_the_timeline() {
    let animation = AnimationPlayback {
        hold_time: Some(ms(500.0)).into(),
        ..playing(1.0)
    };
    assert_eq!(current_time_at(&animation, Some(ms(1500.0))), Some(ms(500.0)));
    assert_eq!(current_time_at(&playing(2.0), Some(ms(1500.0))), Some(ms(3000.0)));
    assert_eq!(current_time_at(&playing(1.0), None), None);
    assert_eq!(current_time_at(&AnimationPlayback::default(), Some(ms(1500.0))), None);
}

#[test]
fn each_iteration_runs_in_its_direction() {
    let mut timing = AnimationEffectTiming {
        iteration_count: f64::INFINITY,
        ..one_second()
    };
    assert_eq!(progress_at(&timing, 1250.0), Some(0.25));
    timing.playback_direction = PlaybackDirection::Alternate;
    assert_eq!(
        progress_at(&timing, 1250.0),
        Some(0.75),
        "an alternate second iteration runs backwards"
    );
    timing.playback_direction = PlaybackDirection::AlternateReverse;
    assert_eq!(progress_at(&timing, 1250.0), Some(0.25));
    timing.playback_direction = PlaybackDirection::Reverse;
    assert_eq!(progress_at(&timing, 1250.0), Some(0.75));
}

#[test]
fn an_infinitely_iterating_effect_after_its_end_is_in_its_last_iteration() {
    let timing = AnimationEffectTiming {
        iteration_count: f64::INFINITY,
        fill_mode: FillMode::Forwards,
        playback_direction: PlaybackDirection::Alternate,
        ..one_second()
    };
    let resolved = resolve_timing(&timing, Some(ms(-1.0)), Some(&playing(-1.0)), None);
    assert_eq!(resolved.phase, AnimationEffectPhase::Before);
    let after = ResolvedEffectTiming {
        phase: AnimationEffectPhase::After,
        local_time: Some(ms(f64::INFINITY)),
        active_time: Some(ms(f64::INFINITY)),
        overall_progress: Some(f64::INFINITY),
    };
    assert_eq!(current_iteration(&timing, &after), Some(f64::INFINITY));
    assert_eq!(current_direction(&timing, &after), AnimationDirection::Forwards);
}

#[test]
fn progress_is_eased_by_the_timing_function() {
    let timing = one_second();
    let resolved = resolved_at(&timing, &playing(1.0), &document_timeline(500.0));
    let steps = Easing::Steps {
        interval_count: 2,
        position: STEP_POSITION_JUMP_START,
    };
    assert_eq!(transformed_progress(&timing, &resolved, &steps), Some(1.0));
}

#[test]
fn the_before_flag_is_set_going_forwards_before_the_active_phase() {
    let timing = AnimationEffectTiming {
        start_delay: ms(100.0),
        fill_mode: FillMode::Backwards,
        ..one_second()
    };
    let resolved = resolved_at(&timing, &playing(1.0), &document_timeline(50.0));
    let progress = transformed_progress_with(&timing, &resolved, |progress, before_flag| match before_flag {
        true => -1.0,
        false => progress,
    });
    assert_eq!(progress, Some(-1.0));
}

#[test]
fn the_play_state_follows_the_playback() {
    let end = ms(1000.0);
    let idle = AnimationPlayback::default();
    assert_eq!(play_state_at(&idle, None, end), AnimationPlayState::Idle);
    let pending = AnimationPlayback {
        has_pending_play_task: true,
        ..idle
    };
    assert_eq!(play_state_at(&pending, None, end), AnimationPlayState::Running);
    let paused = AnimationPlayback {
        hold_time: Some(ms(500.0)).into(),
        ..idle
    };
    assert_eq!(play_state_at(&paused, Some(ms(500.0)), end), AnimationPlayState::Paused);
    assert_eq!(
        play_state_at(&playing(1.0), Some(ms(500.0)), end),
        AnimationPlayState::Running
    );
    assert_eq!(
        play_state_at(&playing(1.0), Some(ms(1000.0)), end),
        AnimationPlayState::Finished
    );
    assert_eq!(
        play_state_at(&playing(-1.0), Some(ms(0.0)), end),
        AnimationPlayState::Finished
    );
    let reversing = AnimationPlayback {
        pending_playback_rate: Some(-1.0).into(),
        ..playing(1.0)
    };
    assert_eq!(
        play_state_at(&reversing, Some(ms(1000.0)), end),
        AnimationPlayState::Running,
        "the effective playback rate decides"
    );
}

#[test]
fn an_effect_is_current_while_it_will_play() {
    let timing = AnimationEffectTiming {
        start_delay: ms(1000.0),
        ..one_second()
    };
    let current_at = |animation: &AnimationPlayback, timeline: &TimelineSample| {
        let resolved = resolved_at(&timing, animation, timeline);
        is_current(&timing, resolved.phase, Some(animation), Some(timeline))
    };
    assert!(
        current_at(&playing(1.0), &document_timeline(500.0)),
        "before, playing forwards"
    );
    assert!(current_at(&playing(1.0), &document_timeline(1500.0)), "in play");
    assert!(
        !current_at(&playing(1.0), &document_timeline(2500.0)),
        "after, playing forwards"
    );
    let finished = AnimationPlayback {
        is_finished: true,
        ..playing(1.0)
    };
    assert!(!is_in_play(AnimationEffectPhase::Active, Some(&finished)));
    assert!(!is_in_play(AnimationEffectPhase::Active, None));

    let scrolled = scroll_timeline(80.0);
    let on_scroll_timeline = normalize_specified_timing(
        &SpecifiedEffectTiming {
            iteration_duration: SpecifiedIterationDuration::milliseconds(1000.0),
            end_delay: 1000.0,
            ..SpecifiedEffectTiming::default()
        },
        Some(percent(100.0)),
        TimeValueType::Percentage,
    );
    let animation = AnimationPlayback {
        start_time: Some(percent(0.0)).into(),
        ..playing(1.0)
    };
    let resolved = resolved_at(&on_scroll_timeline, &animation, &scrolled);
    assert_eq!(resolved.phase, AnimationEffectPhase::After);
    assert!(
        is_current(&on_scroll_timeline, resolved.phase, Some(&animation), Some(&scrolled)),
        "a running animation on a timeline that is not monotonically increasing"
    );
}

#[test]
fn a_relevant_animation_has_a_current_or_in_effect_effect_and_is_not_removed() {
    let timing = one_second();
    let timeline = document_timeline(2000.0);
    let animation = playing(1.0);
    let local_time = current_time_at(&animation, timeline.current_time.get());
    assert!(!is_relevant(&animation, Some(&timeline), Some(&timing), local_time));
    let filling = AnimationEffectTiming {
        fill_mode: FillMode::Forwards,
        ..timing
    };
    assert!(is_relevant(&animation, Some(&timeline), Some(&filling), local_time));
    assert!(!is_relevant(&animation, Some(&timeline), None, local_time));
    let removed = AnimationPlayback {
        replace_state: AnimationReplaceState::Removed,
        ..animation
    };
    assert!(!is_relevant(&removed, Some(&timeline), Some(&filling), local_time));
}
