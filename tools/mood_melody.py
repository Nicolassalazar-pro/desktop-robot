#!/usr/bin/env python3
"""Generate and safely play short mood melodies on the desktop robot."""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Any, Callable, Iterable, Iterator, TextIO

try:
    from melody_player import (
        BOARD_TARGETS,
        MelodyPlaybackError,
        SerialToneTransport,
        play_notes,
        validate_note,
    )
except ModuleNotFoundError:
    from tools.melody_player import (
        BOARD_TARGETS,
        MelodyPlaybackError,
        SerialToneTransport,
        play_notes,
        validate_note,
    )


MOODS = ("happy", "sad", "confused")

# Silent break between moods in --demo. Played back to back the three phrases
# run together and cannot be told apart.
DEMO_GAP_SECONDS = 1.5
DEMO_MOODS = (
    "default",
    "bored",
    "annoyed",
    "tired",
    "angry",
    "happy",
    "sad",
    "error",
    "surprised",
    "scared",
    "wink",
    "confused",
    "sleeping",
)
SEED_BITS = 128
WATCH_DEBOUNCE_SECONDS = 0.750
_MOOD_LINE = re.compile(
    r"^\s*demo\s+step\s+\d+/\d+\s+mood\s+([a-z]+)\s*$",
    re.IGNORECASE,
)


def _load_generator() -> ModuleType:
    """Load pure generation by path only when a phrase is actually needed."""

    path = Path(__file__).with_name("melody_generate.py")
    spec = importlib.util.spec_from_file_location(
        "_desktop_robot_melody_generate", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load melody generator from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(spec.name, None)
        raise
    return module


def parse_seed(text: str) -> int:
    """Parse the generator's fixed-width-compatible 128-bit hex seed."""

    cleaned = text.strip().lower()
    if cleaned.startswith("0x"):
        cleaned = cleaned[2:]
    if not cleaned or any(character not in "0123456789abcdef" for character in cleaned):
        raise argparse.ArgumentTypeError(f"seed must be hexadecimal, got {text!r}")
    value = int(cleaned, 16)
    if value.bit_length() > SEED_BITS:
        raise argparse.ArgumentTypeError(f"seed must fit in {SEED_BITS} bits")
    return value


def format_seed(seed: int) -> str:
    if isinstance(seed, bool) or not isinstance(seed, int):
        raise TypeError("phrase seed must be an integer")
    if seed < 0 or seed.bit_length() > SEED_BITS:
        raise ValueError(f"phrase seed must fit in {SEED_BITS} bits")
    return f"{seed:0{SEED_BITS // 4}x}"


def _positive_count(text: str) -> int:
    try:
        count = int(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("count must be an integer") from exc
    if count < 1:
        raise argparse.ArgumentTypeError("count must be at least 1")
    return count


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate and play seeded piezo phrases for robot moods"
    )
    parser.add_argument("mood", nargs="?", choices=MOODS)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--demo",
        action="store_true",
        help="play labelled examples of happy, sad, and confused",
    )
    mode.add_argument(
        "--watch",
        action="store_true",
        help="follow demo mood lines and play supported mood changes",
    )
    parser.add_argument(
        "--seed",
        type=parse_seed,
        help="128-bit hexadecimal seed; repeats the exact generated phrase",
    )
    parser.add_argument(
        "--count",
        type=_positive_count,
        default=1,
        help="phrases to play for the selected mood or each demo mood",
    )
    parser.add_argument(
        "--gap",
        type=float,
        default=1.5,
        metavar="SECONDS",
        help="silence between consecutive phrases (default: 1.5)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print notes and duration without importing pyserial or opening a port",
    )
    parser.add_argument("--port", metavar="COMx", help="explicit robot serial port")
    parser.add_argument(
        "--target",
        choices=BOARD_TARGETS,
        default="production",
        help="known USB board identity to select (default: production)",
    )
    return parser


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.mood is None and not args.demo and not args.watch:
        parser.error("provide a mood, --demo, or --watch")
    if args.mood is not None and (args.demo or args.watch):
        parser.error("a mood cannot be combined with --demo or --watch")
    if args.watch and args.dry_run:
        parser.error("--watch needs a serial input and cannot be a dry run")
    return args


def parse_mood_line(line: str, *, handled_only: bool = True) -> str | None:
    """Extract a firmware demo mood, optionally filtering unsupported moods."""

    match = _MOOD_LINE.fullmatch(line)
    if match is None:
        return None
    mood = match.group(1).lower()
    allowed = MOODS if handled_only else DEMO_MOODS
    return mood if mood in allowed else None


@dataclass
class MoodChangeDebouncer:
    """Turn serial lines into sparse supported-mood change events.

    Firmware emits each demo mood only once, so a conventional "wait for a
    second identical sample" debounce would suppress every real transition.
    Instead, changes are accepted immediately but rate-limited, with a pending
    mood allowed through if the same line is repeated after the quiet window.
    """

    debounce_seconds: float = WATCH_DEBOUNCE_SECONDS
    last_observed_mood: str | None = None
    last_triggered_at: float | None = None
    pending_mood: str | None = None

    def observe(self, line: str, timestamp: float) -> str | None:
        mood = parse_mood_line(line, handled_only=False)
        if mood is None:
            return None

        changed = mood != self.last_observed_mood
        self.last_observed_mood = mood
        if mood not in MOODS:
            self.pending_mood = None
            return None
        if changed:
            self.pending_mood = mood
        if self.pending_mood != mood:
            return None
        if (
            self.last_triggered_at is not None
            and timestamp - self.last_triggered_at < self.debounce_seconds
        ):
            return None

        self.pending_mood = None
        self.last_triggered_at = timestamp
        return mood


def watch_mood_changes(
    lines: Iterable[str],
    *,
    now: Callable[[], float] = time.monotonic,
    debounce_seconds: float = WATCH_DEBOUNCE_SECONDS,
) -> Iterator[str]:
    """Pure watch parser used by tests and by non-serial integrations."""

    debouncer = MoodChangeDebouncer(debounce_seconds=debounce_seconds)
    for line in lines:
        mood = debouncer.observe(line, now())
        if mood is not None:
            yield mood


def latest_mood_change(
    lines: Iterable[str],
    debouncer: MoodChangeDebouncer,
    *,
    now: Callable[[], float] = time.monotonic,
) -> str | None:
    """Coalesce a serial backlog so playback follows its newest demo mood."""

    recognized: list[tuple[str, str]] = []
    for line in lines:
        mood = parse_mood_line(line, handled_only=False)
        if mood is not None:
            recognized.append((mood, line))
    if not recognized:
        return None
    latest_mood, latest_line = recognized[-1]
    if (
        latest_mood == debouncer.last_observed_mood
        and any(mood != latest_mood for mood, _ in recognized[:-1])
    ):
        # The face left this mood and returned while playback blocked reads.
        debouncer.last_observed_mood = None
    # Intermediate buffered changes are stale and must not consume the rate
    # limit; only the final line represents the face after a long phrase.
    return debouncer.observe(latest_line, now())


def _generate_phrase(generator: Any, mood: str, seed: int | None) -> Any:
    generate = getattr(generator, "generate", None)
    if not callable(generate):
        raise RuntimeError("melody_generate.py does not define generate()")
    phrase = generate(mood, seed=seed)
    if not hasattr(phrase, "seed") or not hasattr(phrase, "notes"):
        raise RuntimeError("generator returned a phrase without seed or notes")
    return phrase


def _phrase_notes(phrase: Any) -> list[Any]:
    notes = list(phrase.notes)
    for note in notes:
        validate_note(note)
    return notes


def print_phrase(
    phrase: Any,
    *,
    output: TextIO | None = None,
    label: str | None = None,
) -> None:
    output = sys.stdout if output is None else output
    notes = [validate_note(note) for note in phrase.notes]
    mood = str(getattr(phrase, "mood", label or "phrase"))
    heading = label or mood
    # Show the closing gesture where the mood has more than one. It is the
    # main thing that varies audibly, so naming it makes listening feedback
    # actionable instead of guesswork.
    gesture = str(getattr(phrase, "terminal_function", "") or "")
    suffix = f"  [{gesture}]" if gesture else ""
    print(
        f"{heading}: seed=0x{format_seed(phrase.seed)}{suffix}", file=output
    )
    for note in notes:
        print(
            f"  {note.hz:4d} Hz  gate={note.gate_ms:3d} ms  "
            f"gap={note.gap_ms:3d} ms",
            file=output,
        )
    total_ms = sum(note.gate_ms + note.gap_ms for note in notes)
    print(f"  total={total_ms} ms", file=output)


def _phrases(
    generator: Any,
    mood: str,
    *,
    seed: int | None,
    count: int,
) -> Iterator[Any]:
    for _ in range(count):
        yield _generate_phrase(generator, mood, seed)


def _connect_transport(
    factory: Callable[..., SerialToneTransport],
    args: argparse.Namespace,
) -> SerialToneTransport:
    return factory(requested_port=args.port, target=args.target)


def _run_selected(
    args: argparse.Namespace,
    generator: Any,
    *,
    transport_factory: Callable[..., SerialToneTransport],
    output: TextIO,
    sleep: Callable[[float], None],
    now: Callable[[], float],
) -> None:
    phrases = list(
        _phrases(
            generator,
            args.mood,
            seed=args.seed,
            count=args.count,
        )
    )
    if args.dry_run:
        for index, phrase in enumerate(phrases, start=1):
            label = args.mood if args.count == 1 else f"{args.mood} {index}/{args.count}"
            print_phrase(phrase, output=output, label=label)
        return

    with _connect_transport(transport_factory, args) as transport:
        for index, phrase in enumerate(phrases, start=1):
            if index > 1:
                # Repeats of one mood need a clear break too, otherwise a run
                # of phrases fuses into a single long stream and cannot be
                # judged individually.
                sleep(args.gap)
            label = args.mood if args.count == 1 else f"{args.mood} {index}/{args.count}"
            print_phrase(phrase, output=output, label=label)
            output.flush()
            play_notes(
                _phrase_notes(phrase),
                transport,
                sleep=sleep,
                now=now,
            )


def _run_demo(
    args: argparse.Namespace,
    generator: Any,
    *,
    transport_factory: Callable[..., SerialToneTransport],
    output: TextIO,
    sleep: Callable[[float], None],
    now: Callable[[], float],
    pause: Callable[[str], None] | None = None,
) -> None:
    generated = [
        (mood, list(_phrases(generator, mood, seed=args.seed, count=args.count)))
        for mood in MOODS
    ]
    if args.dry_run:
        for mood, phrases in generated:
            print(f"=== {mood.upper()} ===", file=output)
            for phrase in phrases:
                print_phrase(phrase, output=output)
        return

    with _connect_transport(transport_factory, args) as transport:
        for index, (mood, phrases) in enumerate(generated):
            if index:
                # A clear silent break between moods. Played back to back they
                # run together and cannot be told apart, which defeats the
                # whole point of a labelled demo.
                sleep(DEMO_GAP_SECONDS)
            print(f"=== {mood.upper()} ===", file=output)
            output.flush()
            for phrase in phrases:
                print_phrase(phrase, output=output)
                output.flush()
                play_notes(
                    _phrase_notes(phrase),
                    transport,
                    sleep=sleep,
                    now=now,
                )
            if pause is not None:
                pause(mood)


def _run_watch(
    args: argparse.Namespace,
    *,
    generator_loader: Callable[[], Any],
    transport_factory: Callable[..., SerialToneTransport],
    output: TextIO,
    sleep: Callable[[float], None],
    now: Callable[[], float],
) -> None:
    debouncer = MoodChangeDebouncer()
    generator: Any | None = None
    with _connect_transport(transport_factory, args) as transport:
        print(f"watching {transport.device} for demo mood changes", file=output)
        while True:
            line = transport.readline()
            if not line:
                continue
            lines = [line]
            drain = getattr(transport, "read_available_lines", None)
            if callable(drain):
                lines.extend(drain())
            for received in lines:
                print(f"< {received}", file=output)
            mood = latest_mood_change(lines, debouncer, now=now)
            if mood is None:
                continue
            if generator is None:
                generator = generator_loader()
            for phrase in _phrases(
                generator,
                mood,
                seed=args.seed,
                count=args.count,
            ):
                print_phrase(phrase, output=output)
                play_notes(
                    _phrase_notes(phrase),
                    transport,
                    sleep=sleep,
                    now=now,
                )


def main(
    argv: list[str] | None = None,
    *,
    generator_loader: Callable[[], Any] | None = None,
    transport_factory: Callable[..., SerialToneTransport] | None = None,
    output: TextIO | None = None,
    error: TextIO | None = None,
    sleep: Callable[[float], None] = time.sleep,
    now: Callable[[], float] = time.monotonic,
) -> int:
    args = parse_args(argv)
    output = sys.stdout if output is None else output
    error = sys.stderr if error is None else error
    loader = _load_generator if generator_loader is None else generator_loader
    factory = (
        SerialToneTransport.connect
        if transport_factory is None
        else transport_factory
    )
    try:
        if args.watch:
            _run_watch(
                args,
                generator_loader=loader,
                transport_factory=factory,
                output=output,
                sleep=sleep,
                now=now,
            )
        elif args.demo:
            generator = loader()
            _run_demo(
                args,
                generator,
                transport_factory=factory,
                output=output,
                sleep=sleep,
                now=now,
            )
        else:
            generator = loader()
            _run_selected(
                args,
                generator,
                transport_factory=factory,
                output=output,
                sleep=sleep,
                now=now,
            )
    except KeyboardInterrupt:
        print("stopped", file=error)
        return 130
    except (
        ImportError,
        MelodyPlaybackError,
        OSError,
        RuntimeError,
        TypeError,
        ValueError,
    ) as exc:
        print(f"ERR {exc}", file=error)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
