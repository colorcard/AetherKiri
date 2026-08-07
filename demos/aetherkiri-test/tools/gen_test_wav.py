#!/usr/bin/env python3
"""Generates the AetherKiri test-demo BGM as a 44.1kHz 16-bit mono WAV.

A short arpeggio loop (C major: C4-E4-G4-C5) with exponential decay per note,
4 seconds per pass, 3 passes. Kept dependency-free (wave + math + struct).
"""

import math
import struct
import sys
import wave

SAMPLE_RATE = 44100
NOTE_SECONDS = 0.45
PASS_SECONDS = 4.0
PASSES = 3
AMPLITUDE = 0.6

# C major arpeggio: frequency (Hz), pan-less mono
NOTES = [261.63, 329.63, 392.00, 523.25, 392.00, 329.63]


def main() -> int:
    output = sys.argv[1] if len(sys.argv) > 1 else "data/bgm/test_bgm.wav"
    total_samples = int(SAMPLE_RATE * PASS_SECONDS * PASSES)
    samples = []

    for idx in range(total_samples):
        t = idx / SAMPLE_RATE
        pass_t = t % PASS_SECONDS
        note_idx = int(pass_t / NOTE_SECONDS) % len(NOTES)
        note_t = pass_t - note_idx * NOTE_SECONDS
        freq = NOTES[note_idx]
        # Sine + quick exponential decay, 5ms attack to avoid clicks.
        env = math.exp(-note_t * 4.5)
        attack = min(1.0, note_t / 0.005)
        value = math.sin(2.0 * math.pi * freq * t) * env * attack
        # Soften the pass boundary with a fade at the end of each pass.
        pass_remain = PASS_SECONDS - pass_t
        fade = min(1.0, pass_remain / 0.2)
        value *= AMPLITUDE * fade
        samples.append(int(max(-1.0, min(1.0, value)) * 32767))

    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(struct.pack("<%dh" % len(samples), *samples))

    print(f"generated {output}: {total_samples} samples, "
          f"{total_samples / SAMPLE_RATE:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
