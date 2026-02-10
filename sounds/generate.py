#!/usr/bin/env python3
"""
Generate UI sound effects for Parties using MIDI + FluidSynth.

Requirements:
    pip install mido numpy
    FluidSynth CLI must be installed and in PATH
    (on Windows: choco install fluidsynth, or download from https://github.com/FluidSynth/fluidsynth/releases)

Usage:
    python generate.py              # auto-downloads FluidR3_GM soundfont
    python generate.py my_font.sf2  # use a specific soundfont
"""

import mido
import subprocess
import wave
import numpy as np
import os
import sys
import tempfile
import urllib.request

SAMPLE_RATE = 48000
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SF2 = os.path.join(SCRIPT_DIR, "FluidR3_GM.sf2")
SF2_URL = "https://keymusician01.s3.amazonaws.com/FluidR3_GM.zip"

# ── MIDI note numbers ──────────────────────────────────────────────
C4, Eb4, E4, F4, G4, A4, Bb4 = 60, 63, 64, 65, 67, 69, 70
C5, D5, E5, F5, G5, A5 = 72, 74, 76, 77, 79, 81

# ── GM program numbers ─────────────────────────────────────────────
CELESTA      = 8
GLOCKENSPIEL = 9
VIBRAPHONE   = 11
MARIMBA      = 12

# ── Timing config ──────────────────────────────────────────────────
TICKS_PER_BEAT = 480
TEMPO = 500000  # 120 BPM, 1 tick ~ 1.04ms


def ms_to_ticks(ms):
    return int(ms * TICKS_PER_BEAT / (TEMPO / 1000))


def create_midi_file(notes, program):
    """Create a temporary MIDI file from note events.
    notes: list of (start_ms, midi_note, velocity, duration_ms)
    """
    mid = mido.MidiFile(ticks_per_beat=TICKS_PER_BEAT)
    track = mido.MidiTrack()
    mid.tracks.append(track)

    track.append(mido.MetaMessage('set_tempo', tempo=TEMPO))
    track.append(mido.Message('program_change', program=program, channel=0, time=0))

    # Build event list
    events = []
    for start_ms, note, vel, dur_ms in notes:
        t_on  = ms_to_ticks(start_ms)
        t_off = ms_to_ticks(start_ms + dur_ms)
        events.append((t_on,  'note_on',  note, vel))
        events.append((t_off, 'note_off', note, 0))
    events.sort(key=lambda x: (x[0], x[1] != 'note_off'))

    # Convert to delta times
    prev = 0
    for tick, msg_type, note, vel in events:
        delta = max(0, tick - prev)
        track.append(mido.Message(msg_type, note=note, velocity=vel,
                                  channel=0, time=delta))
        prev = tick

    tmp = tempfile.NamedTemporaryFile(suffix='.mid', delete=False)
    mid.save(tmp.name)
    tmp.close()
    return tmp.name


def render_with_fluidsynth(midi_path, wav_path, soundfont):
    """Render MIDI to WAV using FluidSynth CLI."""
    cmd = [
        'fluidsynth', '-ni',
        '-r', str(SAMPLE_RATE),
        '-g', '1.0',
        '-F', wav_path,
        soundfont, midi_path,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"\n  FluidSynth error: {result.stderr.strip()}")
        raise RuntimeError("FluidSynth failed")


def postprocess(wav_path, target_amp=0.1):
    """Trim silence, convert to mono 16-bit 48kHz, normalize."""
    with wave.open(wav_path, 'r') as f:
        nch = f.getnchannels()
        sw  = f.getsampwidth()
        sr  = f.getframerate()
        raw = f.readframes(f.getnframes())

    # Decode to float
    if sw == 2:
        samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    elif sw == 4:
        samples = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"Unsupported sample width: {sw}")

    # Stereo -> mono
    if nch == 2:
        samples = samples.reshape(-1, 2).mean(axis=1)

    # Resample if needed
    if sr != SAMPLE_RATE:
        new_len = int(len(samples) * SAMPLE_RATE / sr)
        x_old = np.linspace(0, 1, len(samples))
        x_new = np.linspace(0, 1, new_len)
        samples = np.interp(x_new, x_old, samples)

    # Trim trailing silence (below -60dB)
    threshold = 0.001
    last = len(samples) - 1
    while last > 0 and abs(samples[last]) < threshold:
        last -= 1
    tail_samples = int(SAMPLE_RATE * 0.05)  # keep 50ms tail
    samples = samples[:min(last + tail_samples, len(samples))]

    # Normalize
    peak = np.max(np.abs(samples))
    if peak > 0:
        samples *= target_amp / peak

    # Write 16-bit mono WAV
    out = np.clip(samples * 32767, -32768, 32767).astype(np.int16)
    with wave.open(wav_path, 'w') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SAMPLE_RATE)
        f.writeframes(out.tobytes())


def download_soundfont(path):
    """Download FluidR3_GM soundfont if not present."""
    if os.path.exists(path):
        return
    zip_path = path + ".zip"
    print(f"Downloading FluidR3_GM soundfont...")
    urllib.request.urlretrieve(SF2_URL, zip_path)
    import zipfile
    with zipfile.ZipFile(zip_path) as zf:
        for name in zf.namelist():
            if name.endswith('.sf2'):
                print(f"  Extracting {name}...")
                with zf.open(name) as src, open(path, 'wb') as dst:
                    dst.write(src.read())
                break
    os.remove(zip_path)
    print(f"  Saved to {path}")


# ═══════════════════════════════════════════════════════════════════
# Sound Definitions
# Each: (start_ms, midi_note, velocity, duration_ms)
# ═══════════════════════════════════════════════════════════════════

SOUNDS = {
    # ── Unmute: Quick snappy chirp up (Celesta) ────────────────
    # E5 -> A5  — fast 2-note ascending, short toggle "pip"
    'parties-unmute': {
        'program': CELESTA,
        'notes': [
            (0,  E5, 80, 80),
            (45, A5, 95, 100),
        ],
    },

    # ── Mute: Quick snappy chirp down (Celesta) ─────────────────
    # A5 -> E5  — fast 2-note descending, clean "off" click
    'parties-mute': {
        'program': CELESTA,
        'notes': [
            (0,  A5, 85, 80),
            (45, E5, 65, 100),
        ],
    },

    # ── Undeafen: Wide warm ascending (Vibraphone) ──────────────
    # C4 -> G4 -> E5  — wide intervals, opening up, resonant
    'parties-undeafen': {
        'program': VIBRAPHONE,
        'notes': [
            (0,   C4, 70, 200),
            (95,  G4, 85, 200),
            (190, E5, 95, 250),
        ],
    },

    # ── Deafen: Wide warm descending (Vibraphone) ───────────────
    # E5 -> G4 -> C4  — closing down, definitive
    'parties-deafen': {
        'program': VIBRAPHONE,
        'notes': [
            (0,   E5, 85, 200),
            (95,  G4, 70, 200),
            (190, C4, 55, 250),
        ],
    },

    # ── Join Channel: Bright C-major arpeggio (Marimba) ─────────
    # C4 -> E4 -> G4 -> C5  — welcoming, percussive, clean
    'parties-join-self': {
        'program': MARIMBA,
        'notes': [
            (0,   C4, 70,  100),
            (55,  E4, 80,  100),
            (110, G4, 90,  100),
            (165, C5, 100, 150),
        ],
    },

    # ── Leave Channel: Descending farewell (Marimba) ────────────
    # C5 -> G4 -> E4 -> C4  — definitive departure
    'parties-leave-self': {
        'program': MARIMBA,
        'notes': [
            (0,   C5, 80, 100),
            (65,  G4, 70, 100),
            (130, E4, 60, 100),
            (195, C4, 50, 140),
        ],
    },

    # ── User Joined: Single soft ping (Celesta) ───────────────────
    # G5  — gentle single note, barely-there notification
    'parties-join-other': {
        'program': CELESTA,
        'notes': [
            (0, G5, 40, 100),
        ],
    },

    # ── User Left: Single soft low ping (Celesta) ───────────────
    # E5  — gentle single note, subtle farewell
    'parties-leave-other': {
        'program': CELESTA,
        'notes': [
            (0, E5, 35, 100),
        ],
    },
}


def main():
    soundfont = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SF2
    download_soundfont(soundfont)

    if not os.path.exists(soundfont):
        print(f"Error: SoundFont not found: {soundfont}")
        sys.exit(1)

    # Verify FluidSynth is available
    try:
        subprocess.run(['fluidsynth', '--version'], capture_output=True, check=True)
    except FileNotFoundError:
        print("Error: FluidSynth not found in PATH.")
        print("  Windows: choco install fluidsynth")
        print("  Linux:   apt install fluidsynth")
        print("  macOS:   brew install fluid-synth")
        sys.exit(1)

    print(f"Using soundfont: {soundfont}")
    print()

    for name, cfg in SOUNDS.items():
        wav_path = os.path.join(SCRIPT_DIR, f"{name}.wav")
        print(f"  {name}...", end=" ", flush=True)

        # Create MIDI
        midi_path = create_midi_file(cfg['notes'], cfg['program'])

        try:
            # Render to WAV
            render_with_fluidsynth(midi_path, wav_path, soundfont)
            # Post-process
            postprocess(wav_path)
            # Report size
            size = os.path.getsize(wav_path)
            with wave.open(wav_path, 'r') as f:
                dur_ms = int(f.getnframes() / f.getframerate() * 1000)
            print(f"OK ({dur_ms}ms, {size//1024}KB)")
        finally:
            os.unlink(midi_path)

    print()
    print("Done! WAV files written to:", SCRIPT_DIR)


if __name__ == '__main__':
    main()
