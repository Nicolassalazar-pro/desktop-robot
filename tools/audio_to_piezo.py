"""audio_to_piezo.py - convert MIDI / MP3 / WAV / M4A / MP4 -> mono frequency
sequence for the desktop-robot piezo (GPIO42).

Target piezo addressable range: 200 Hz -> 3322.44 Hz (G#7).
Loud zone: 2-4 kHz (use --octave to shift the whole song into it).

Outputs:
  --out  path.json     write JSON sequence
  --header path.h      write firmware C header (PROGMEM Note[])
  --stream COM6        live-stream over serial via `tone <hz>` commands
  (none)               JSON to stdout

Reducer strategy (MIDI only; audio is already monophonic via pyin):
  --strategy loudest   highest-velocity active note (default)
  --strategy highest   topmost active note (good for melodies)
  --strategy lowest    bassline
  --strategy average   median MIDI note (rarely musical, included for fun)

Usage:
  py -3.12 tools/audio_to_piezo.py tools/samples/walle_name.wav --octave 2 --out walle.json
  py -3.12 tools/audio_to_piezo.py song.mid --strategy loudest --header firmware_song.h --name kSongFoo
  py -3.12 tools/audio_to_piezo.py clip.wav --stream COM6
"""
from __future__ import annotations

import argparse
import inspect
import json
import math
import statistics
import sys
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable, Optional


# ---------- Note chart (equal-tempered, A4 = 440 Hz) -------------------------

A4_MIDI = 69
A4_HZ = 440.0
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Locked target range for the onboard piezo (HARDWARE.md §4)
FIRMWARE_TONE_MIN_HZ = 200
FIRMWARE_TONE_MAX_HZ = 6000
PIEZO_LO_HZ = float(FIRMWARE_TONE_MIN_HZ)
PIEZO_HI_HZ = 3322.44    # G#7 (MIDI 104)
PIEZO_LO_MIDI = 56       # G#3 (207.65 Hz), first semitone accepted by firmware
PIEZO_HI_MIDI = 104
BRIDGE_HEARTBEAT_SECONDS = 0.250
TONE_REFRESH_SECONDS = BRIDGE_HEARTBEAT_SECONDS - 0.010


def midi_to_hz(m: float) -> float:
    return A4_HZ * (2.0 ** ((m - A4_MIDI) / 12.0))


def hz_to_midi(hz: float) -> float:
    return A4_MIDI + 12.0 * math.log2(hz / A4_HZ)


def note_name(midi_int: int) -> str:
    return f"{NOTE_NAMES[midi_int % 12]}{(midi_int // 12) - 1}"


def quantize_hz(hz: float) -> tuple[int, float]:
    """Snap arbitrary Hz to nearest equal-tempered semitone.
    Returns (midi_int, snapped_hz)."""
    m = round(hz_to_midi(hz))
    return m, midi_to_hz(m)


def clamp_to_piezo(midi_int: int) -> Optional[int]:
    """Return midi_int if in piezo range, else None (caller should drop)."""
    if PIEZO_LO_MIDI <= midi_int <= PIEZO_HI_MIDI:
        return midi_int
    return None


# ---------- Output unit ------------------------------------------------------

@dataclass
class Note:
    hz: int         # 0 = rest/silence
    ms: int
    name: str = ""  # "A5" / "rest" — informational

    def to_jsonable(self) -> dict:
        return {"hz": self.hz, "ms": self.ms, "name": self.name}


def note_from_midi(midi_int: int, ms: int) -> Note:
    """Convert an in-range MIDI pitch, preserving out-of-range time as a rest."""
    midi_clamped = clamp_to_piezo(midi_int)
    if midi_clamped is None:
        return Note(hz=0, ms=ms, name="rest")
    return Note(
        hz=int(round(midi_to_hz(midi_clamped))),
        ms=ms,
        name=note_name(midi_clamped),
    )


# ---------- MIDI ingest ------------------------------------------------------

def midi_to_events(path: Path, strategy: str) -> list[Note]:
    import mido
    mid = mido.MidiFile(str(path))

    # Build (abs_time_sec, type, note, velocity) stream across all tracks merged.
    # mido.MidiFile already merges + converts ticks to seconds when iterated.
    active: dict[tuple[int, int], list[int]] = {}
    notes: list[Note] = []
    absolute_time = 0.0
    segment_start = 0.0
    selected_pitch: Optional[int] = None

    def reduce(now: dict[tuple[int, int], list[int]]) -> Optional[int]:
        voices = [
            (note, velocity)
            for (_, note), velocities in now.items()
            for velocity in velocities
        ]
        if not voices:
            return None
        if strategy == "loudest":
            return max(voices, key=lambda voice: voice[1])[0]
        if strategy == "highest":
            return max(note for note, _ in voices)
        if strategy == "lowest":
            return min(note for note, _ in voices)
        if strategy == "average":
            return int(round(statistics.median(note for note, _ in voices)))
        raise ValueError(f"unknown strategy {strategy!r}")

    def flush(now_t: float) -> None:
        nonlocal segment_start
        dt_ms = int(round((now_t - segment_start) * 1000))
        if dt_ms <= 0:
            segment_start = now_t
            return
        if selected_pitch is None:
            notes.append(Note(hz=0, ms=dt_ms, name="rest"))
        else:
            notes.append(note_from_midi(selected_pitch, dt_ms))
        segment_start = now_t

    for msg in mid:
        # `msg.time` is delta seconds when iterating MidiFile directly.
        absolute_time += msg.time
        if msg.type in ("note_on", "note_off"):
            key = (getattr(msg, "channel", 0), msg.note)
            if msg.type == "note_on" and msg.velocity > 0:
                active.setdefault(key, []).append(msg.velocity)
            else:  # note_off OR note_on vel=0
                voices = active.get(key)
                if voices:
                    del voices[0]
                    if not voices:
                        del active[key]
            new_pitch = reduce(active)
            if new_pitch != selected_pitch:
                flush(absolute_time)
                selected_pitch = new_pitch

    flush(absolute_time)

    # Preserve the existing token tail when the MIDI ends with a note held.
    if selected_pitch is not None:
        notes.append(note_from_midi(selected_pitch, 50))

    return notes


# ---------- Audio ingest (mp3 / wav / m4a / mp4) -----------------------------

def audio_to_events(path: Path) -> list[Note]:
    import numpy as np
    import librosa

    # If non-wav and ffmpeg available via imageio_ffmpeg, transcode to a temp wav so
    # librosa's audioread backend never has to find ffmpeg on PATH.
    src = path
    tmp_wav: Optional[Path] = None
    if path.suffix.lower() != ".wav":
        try:
            import imageio_ffmpeg
            import os as _os
            import subprocess, tempfile
            ff = imageio_ffmpeg.get_ffmpeg_exe()
            fd, tmp_path = tempfile.mkstemp(suffix=".wav")
            _os.close(fd)  # close the handle so ffmpeg/librosa can use the path
            tmp_wav = Path(tmp_path)
            subprocess.run(
                [ff, "-y", "-i", str(path), "-ar", "22050", "-ac", "1", "-vn", str(tmp_wav)],
                check=True, capture_output=True,
            )
            src = tmp_wav
        except Exception as e:
            print(f"[warn] ffmpeg transcode failed ({e}); trying librosa direct load", file=sys.stderr)

    y, sr = librosa.load(str(src), sr=22050, mono=True)
    if tmp_wav and tmp_wav.exists():
        tmp_wav.unlink()

    # pyin = probabilistic YIN, robust monophonic pitch tracker.
    # Frame at hop_length=512 (~23.2 ms/frame at 22050 Hz).
    f0, voiced_flag, voiced_prob = librosa.pyin(
        y,
        fmin=PIEZO_LO_HZ,
        fmax=PIEZO_HI_HZ,
        sr=sr,
        frame_length=2048,
        hop_length=512,
    )

    frame_ms = (512 / sr) * 1000.0   # ≈ 23.2 ms

    # Quantize each voiced frame to nearest semitone (or None for silence).
    quantized: list[Optional[int]] = []
    for hz, v in zip(f0, voiced_flag):
        if v and not (hz is None or math.isnan(hz)):
            m, _snap = quantize_hz(float(hz))
            quantized.append(m if clamp_to_piezo(m) is not None else None)
        else:
            quantized.append(None)

    # Run-length encode consecutive identical values into Notes.
    notes: list[Note] = []
    if not quantized:
        return notes
    run_pitch = quantized[0]
    run_len = 1
    for p in quantized[1:]:
        if p == run_pitch:
            run_len += 1
        else:
            ms = int(round(run_len * frame_ms))
            if run_pitch is None:
                notes.append(Note(hz=0, ms=ms, name="rest"))
            else:
                notes.append(Note(
                    hz=int(round(midi_to_hz(run_pitch))),
                    ms=ms,
                    name=note_name(run_pitch),
                ))
            run_pitch = p
            run_len = 1
    # tail
    ms = int(round(run_len * frame_ms))
    if run_pitch is None:
        notes.append(Note(hz=0, ms=ms, name="rest"))
    else:
        notes.append(Note(
            hz=int(round(midi_to_hz(run_pitch))),
            ms=ms,
            name=note_name(run_pitch),
        ))
    return notes


# ---------- Post-processing --------------------------------------------------

def transpose(notes: list[Note], octave_shift: int) -> list[Note]:
    if octave_shift == 0:
        return notes
    out: list[Note] = []
    for n in notes:
        if n.hz == 0:
            out.append(n)
            continue
        m = int(round(hz_to_midi(n.hz))) + 12 * octave_shift
        out.append(note_from_midi(m, n.ms))
    return out


def merge_adjacent_same(notes: list[Note], min_ms: int = 30) -> list[Note]:
    """Glue consecutive identical-Hz notes (post-transpose). Drop sub-min_ms
    rests by merging into the next note."""
    if not notes:
        return notes
    out: list[Note] = []
    for n in notes:
        if out and out[-1].hz == n.hz:
            out[-1] = Note(hz=n.hz, ms=out[-1].ms + n.ms, name=n.name)
        else:
            out.append(n)
    # Drop very short flickers (<min_ms) entirely - more aggressive than just
    # rests, removes any sub-perceptual blip.
    cleaned: list[Note] = []
    for n in out:
        if n.ms < min_ms and cleaned:
            cleaned[-1] = Note(hz=cleaned[-1].hz, ms=cleaned[-1].ms + n.ms, name=cleaned[-1].name)
        else:
            cleaned.append(n)
    return cleaned


# ---------- Emitters ---------------------------------------------------------

def emit_json(notes: list[Note], path: Optional[Path]) -> None:
    payload = {
        "count": len(notes),
        "total_ms": sum(n.ms for n in notes),
        "notes": [n.to_jsonable() for n in notes],
    }
    text = json.dumps(payload, indent=2)
    if path:
        path.write_text(text, encoding="utf-8")
        print(f"[json] wrote {path} ({len(notes)} notes, {payload['total_ms']} ms)", file=sys.stderr)
    else:
        print(text)


def emit_header(notes: list[Note], path: Path, name: str) -> None:
    lines = [
        "// Auto-generated by tools/audio_to_piezo.py — DO NOT EDIT by hand.",
        f"// {len(notes)} notes, {sum(n.ms for n in notes)} ms total.",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "struct PiezoNote { uint16_t hz; uint16_t ms; };",
        "",
        f"static const PiezoNote {name}[] PROGMEM = {{",
    ]
    for n in notes:
        comment = f"  // {n.name}" if n.name else ""
        lines.append(f"    {{ {n.hz:>5}, {n.ms:>5} }},{comment}")
    lines.append("};")
    lines.append(f"static const size_t {name}Len = sizeof({name}) / sizeof({name}[0]);")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"[header] wrote {path} ({len(notes)} notes)", file=sys.stderr)


class FirmwareResponseError(RuntimeError):
    """An error response reported by the firmware."""


def _read_serial_input(serial_port, max_bytes: int, max_seconds: float) -> bytes:
    """Read already-buffered serial input within strict byte/time bounds."""
    deadline = time.perf_counter() + max_seconds
    data = bytearray()
    while len(data) < max_bytes and time.perf_counter() < deadline:
        waiting = serial_port.in_waiting
        if waiting <= 0:
            break
        chunk = serial_port.read(min(waiting, max_bytes - len(data), 512))
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def _drain_serial_input(serial_port, max_bytes: int, max_seconds: float) -> int:
    """Consume already-buffered serial input within strict byte/time bounds."""
    return len(_read_serial_input(serial_port, max_bytes, max_seconds))


def _drain_serial_replies(
    serial_port,
    pending: bytearray,
    max_bytes: int,
    max_seconds: float,
) -> int:
    """Consume bounded input, parse complete lines, and reject firmware errors."""
    data = _read_serial_input(serial_port, max_bytes, max_seconds)
    pending.extend(data)

    while True:
        newline = pending.find(b"\n")
        if newline < 0:
            break
        raw_line = bytes(pending[:newline])
        del pending[: newline + 1]
        line = raw_line.rstrip(b"\r").decode("utf-8", errors="replace")
        if line.startswith("ERR "):
            raise FirmwareResponseError(f"firmware response: {line}")
    if len(pending) > 8192:
        del pending[:-8192]
    return len(data)


def _firmware_safe_hz(hz: int) -> int:
    if hz == 0 or FIRMWARE_TONE_MIN_HZ <= hz <= FIRMWARE_TONE_MAX_HZ:
        return hz
    if 0 < hz < FIRMWARE_TONE_MIN_HZ:
        return 0
    raise ValueError(
        f"tone frequency must be 0 or {FIRMWARE_TONE_MIN_HZ}.."
        f"{FIRMWARE_TONE_MAX_HZ} Hz, got {hz}"
    )


def _write_tone(serial_port, hz: int) -> int:
    safe_hz = _firmware_safe_hz(hz)
    payload = f"tone {safe_hz}\n".encode("ascii")
    written = serial_port.write(payload)
    if written != len(payload):
        raise OSError(f"short serial write: wrote {written}/{len(payload)} bytes")
    return safe_hz


def emit_stream(notes: list[Note], port: str, baud: int = 115200) -> None:
    """Stream notes using only the implemented `tone <hz>` / `tone 0` protocol."""
    import serial

    print(f"[stream] opening {port} @ {baud}", file=sys.stderr)
    s = None
    pending_replies = bytearray()
    primary_error: Optional[BaseException] = None
    primary_traceback = None
    cleanup_firmware_error: Optional[FirmwareResponseError] = None
    elapsed = 0.0
    try:
        serial_kwargs = {"port": None, "baudrate": baud, "timeout": 0}
        try:
            constructor_params = inspect.signature(serial.Serial).parameters
        except (TypeError, ValueError):
            constructor_params = {}
        if "dtr" in constructor_params:
            serial_kwargs["dtr"] = False
        if "rts" in constructor_params:
            serial_kwargs["rts"] = False

        s = serial.Serial(**serial_kwargs)
        s.dtr = False
        s.rts = False
        s.port = port
        s.open()
        # Keep both control lines low before allowing the device startup settle.
        s.dtr = False
        s.rts = False
        time.sleep(0.5)
        startup_bytes = _drain_serial_replies(
            s, pending_replies, max_bytes=8192, max_seconds=0.05
        )
        if startup_bytes:
            print(f"[stream] drained {startup_bytes} startup bytes", file=sys.stderr)

        start = time.perf_counter()
        target = start
        for i, n in enumerate(notes):
            # Consume the previous command reply before sending the next note.
            _drain_serial_replies(
                s, pending_replies, max_bytes=2048, max_seconds=0.001
            )
            last_write_started = time.perf_counter()
            sent_hz = _write_tone(s, n.hz)
            target += n.ms / 1000.0
            next_refresh = last_write_started + TONE_REFRESH_SECONDS
            # busy-precise wait
            while True:
                now = time.perf_counter()
                if now >= target:
                    break
                deadline = target
                if sent_hz != 0 and next_refresh < target:
                    deadline = min(deadline, next_refresh)
                slack = deadline - now
                if slack <= 0:
                    _drain_serial_replies(
                        s, pending_replies, max_bytes=2048, max_seconds=0.001
                    )
                    last_write_started = time.perf_counter()
                    _write_tone(s, sent_hz)
                    next_refresh = last_write_started + TONE_REFRESH_SECONDS
                    continue
                if slack > 0.005:
                    time.sleep(slack - 0.002)
            if i % 25 == 0:
                print(f"[stream] {i}/{len(notes)} note={n.name} hz={n.hz} ms={n.ms}", file=sys.stderr)
        _drain_serial_replies(
            s, pending_replies, max_bytes=8192, max_seconds=0.02
        )
        elapsed = time.perf_counter() - start
    except BaseException as error:
        primary_error = error
        primary_traceback = error.__traceback__
    finally:
        if s is not None:
            try:
                _write_tone(s, 0)
                s.flush()
            except Exception as e:
                print(f"[stream] cleanup tone 0 failed: {e}", file=sys.stderr)
            try:
                time.sleep(0.01)
                _drain_serial_replies(
                    s, pending_replies, max_bytes=2048, max_seconds=0.01
                )
            except FirmwareResponseError as e:
                cleanup_firmware_error = e
                print(f"[stream] cleanup reply drain failed: {e}", file=sys.stderr)
            except Exception as e:
                print(f"[stream] cleanup reply drain failed: {e}", file=sys.stderr)
            try:
                s.close()
            except Exception as e:
                print(f"[stream] serial close failed: {e}", file=sys.stderr)
    if primary_error is not None:
        raise primary_error.with_traceback(primary_traceback)
    if cleanup_firmware_error is not None:
        raise cleanup_firmware_error
    print(f"[stream] done in {elapsed:.2f} s", file=sys.stderr)


def emit_preview(notes: list[Note], path: Optional[Path] = None,
                 play: bool = True, sample_rate: int = 22050) -> Optional[Path]:
    """Synthesize the note sequence as a SQUARE WAVE WAV — identical waveform
    to what the piezo will produce. Writes WAV (path or temp), and by default
    plays it back through PC speakers so you can preview without flashing.
    """
    import math, struct, wave, os, tempfile
    if path is None:
        fd, tmp = tempfile.mkstemp(suffix="_piezo_preview.wav")
        os.close(fd)
        path = Path(tmp)

    amp = 24000  # int16 amplitude (square -amp / +amp); leaves a little headroom

    frames = bytearray()
    for n in notes:
        nsamp = int(round(sample_rate * n.ms / 1000.0))
        if n.hz == 0 or nsamp <= 0:
            # rest: silence
            frames.extend(b"\x00\x00" * max(nsamp, 0))
            continue
        # Square wave at n.hz, 50% duty.
        period_samp = sample_rate / float(n.hz)
        # Pre-build one cycle, then tile.
        half = max(1, int(round(period_samp / 2.0)))
        cycle = (struct.pack("<h", amp) * half) + (struct.pack("<h", -amp) * half)
        cycle_len = len(cycle) // 2
        # Repeat to fill nsamp samples.
        full_cycles = nsamp // cycle_len
        tail = nsamp - full_cycles * cycle_len
        frames.extend(cycle * full_cycles)
        if tail > 0:
            frames.extend(cycle[: tail * 2])

    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(bytes(frames))

    total_ms = sum(n.ms for n in notes)
    print(f"[preview] wrote {path} ({total_ms} ms, square-wave)", file=sys.stderr)

    if play:
        try:
            import winsound
            # SND_FILENAME | SND_NOWAIT bits — but PlaySync is fine since
            # preview clips are short.
            winsound.PlaySound(str(path), winsound.SND_FILENAME)
            print("[preview] playback done", file=sys.stderr)
        except Exception as e:
            print(f"[preview] could not play ({e}); WAV saved at {path}", file=sys.stderr)

    return path




def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", type=Path, help="input file: .mid / .mp3 / .wav / .m4a / .mp4")
    p.add_argument("--strategy", choices=["loudest", "highest", "lowest", "average"],
                   default="loudest", help="MIDI polyphony reducer (audio is monophonic by design)")
    p.add_argument("--octave", type=int, default=0,
                   help="shift entire song by N octaves (positive=up). Use to land in 2-4 kHz loud zone.")
    p.add_argument("--min-ms", type=int, default=30,
                   help="merge/drop notes shorter than this (default 30 ms; piezo can't articulate faster)")
    p.add_argument("--out", type=Path, help="write JSON to file (else stdout)")
    p.add_argument("--header", type=Path, help="write firmware C header")
    p.add_argument("--name", default="kSong", help="C symbol name for --header (default kSong)")
    p.add_argument("--stream", help="live-stream to a serial port (e.g. COM6)")
    p.add_argument("--preview", action="store_true",
                   help="synthesize the converted note sequence as a SQUARE WAVE WAV and play it through PC speakers; exact piezo simulation, no device flash needed")
    p.add_argument("--preview-out", type=Path,
                   help="save the preview WAV to this path instead of a temp file (implies --preview)")
    p.add_argument("--no-play", action="store_true",
                   help="with --preview, just write the WAV (don't auto-play)")
    p.add_argument("--summary", action="store_true", help="print stats to stderr only (no JSON to stdout)")
    args = p.parse_args(argv)

    if not args.input.exists():
        print(f"input not found: {args.input}", file=sys.stderr)
        return 2

    ext = args.input.suffix.lower()
    print(f"[in] {args.input} ({ext})", file=sys.stderr)
    if ext in (".mid", ".midi"):
        notes = midi_to_events(args.input, args.strategy)
    elif ext in (".mp3", ".wav", ".m4a", ".mp4", ".flac", ".ogg", ".aac", ".webm"):
        notes = audio_to_events(args.input)
    else:
        print(f"unsupported extension: {ext}", file=sys.stderr)
        return 2

    print(f"[parse] {len(notes)} raw notes, {sum(n.ms for n in notes)} ms", file=sys.stderr)

    notes = transpose(notes, args.octave)
    notes = merge_adjacent_same(notes, min_ms=args.min_ms)
    print(f"[post]  {len(notes)} notes after transpose+merge", file=sys.stderr)

    # Range histogram (informational)
    pitched = [n for n in notes if n.hz > 0]
    if pitched:
        hzs = [n.hz for n in pitched]
        print(f"[range] pitched: {min(hzs)}-{max(hzs)} Hz, "
              f"median {int(statistics.median(hzs))} Hz, "
              f"rests: {len(notes) - len(pitched)}", file=sys.stderr)

    if args.header:
        emit_header(notes, args.header, args.name)
    if args.stream:
        emit_stream(notes, args.stream)
    if args.preview or args.preview_out:
        emit_preview(notes, path=args.preview_out, play=not args.no_play)
    if args.out or not (args.header or args.stream or args.preview or args.preview_out or args.summary):
        emit_json(notes, args.out)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
