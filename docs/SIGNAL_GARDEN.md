# SIGNAL GARDEN

An ear trainer for the M-VAVE FM-1, built into [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita. The keys are where you answer, and the screen is a terminal tuned to station 88.1.

When you power on normally, you get Felucca exactly as it was. SIGNAL GARDEN only starts when you ask for it, and it changes none of Felucca's sounds, projects or settings.

## Status: v0.1, first station

- **88.1 NOTE ID** works now. You hear a reference C and then a note, and you play that note on the keys. Each new note is shown and played to you (a Reveal) before it's quizzed. Every answer gets a "why" line and a "next trap" line.
- **The learning engine** works now. It schedules notes SM-2 style, keeps a confusion matrix and a weak-spot report, and moves misses later in the same session. It also measures your speed: a correct answer that's slower than your usual time counts as shaky and comes back sooner.
- **Not yet:**
  - Progress is lost at power-off, because it's only kept in RAM. See "Next" below.
  - The other stations: half/whole steps, intervals, scales and modes, chords.
  - The game layer beyond the Garden and the ranks.

## Install

The dev build is on this repository's Releases page, under the `dev` pre-release, as `signal-garden-dev.fwsc`. GitHub Actions rebuilds it on every push to `main`.

1. Back up first. Open the [Felucca web editor](https://hugelton.github.io/Felucca/webapp/editor/) in Chrome or Edge, connect the FM-1 by USB, and use its full backup.
2. Install the package. Either use the command line:

   ```
   pip3 install mido python-rtmidi
   python3 tools/fm1_install.py signal-garden-dev.fwsc
   ```

   or run Felucca's installer locally (it needs `localhost`, because Web MIDI only works on a secure origin):

   ```
   python3 web/make_site.py signal-garden-dev.fwsc dev /tmp/sg-site
   cd /tmp/sg-site && python3 -m http.server 8000
   # open http://localhost:8000/webapp/installer/ in Chrome or Edge
   ```

**To go back:** reinstall any Felucca release, or run M-VAVE's own updater to restore the official firmware (V15).

**If the FM-1 stays black after a failed install:** check whether your computer sees a USB device named `WL80UBOOT`. That's the chip's built-in boot mode, and the unit can be recovered with [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter). You install firmware at your own risk.

## Use

**Getting in:** hold **F3 and F#3**, the two lowest keys, while you switch the FM-1 on. Keep holding until the 88.1 screen shows.

**Getting out:** hold **HOME** for one second, or tap HOME on the station screen.

| | |
| --- | --- |
| Keys | Answer. Any octave counts. In a Reveal, the keys play freely. |
| PLAY | Start, go to the next prompt, or hear the prompt again. |
| KNOB 1 | Hear the prompt again. |
| KNOB 4 | On the summary: open the weak-spot report, and close it again. |
| OCT− / OCT+ | Move the keys' range. |
| HOME | Tap: back to the station screen. Hold: leave SIGNAL GARDEN. |

Notes play through the selected track's sound. A track using DRUM or SLICE has no pitch to learn from, so SIGNAL GARDEN picks the first track with a pitched sound, and tells you if there isn't one. The keys' LEDs light every C as the reference. During a Reveal or after an answer, they also light the right note, and the wrong note you played breathes.

### How a session runs

- **Length:** 10 prompts, plus any misses brought back. That's about three minutes.
- **Mix:**
  - About 60% is review of notes that are due, oldest first.
  - About 25% drills your worst confusion pairs, both sides of each pair.
  - One new note is added, but only once nothing due is left out.
  - Any leftover slots go to the notes with your lowest accuracy.
- **First session:** introduces C, G and E. They are the notes of a major chord and far apart, so they're easy to tell apart.
- **Order of new notes:** after C, G and E come F, D, A and B, then F#, A#, C#, D# and G#.
- **The clock is the session, not the calendar.** The FM-1 has no real-time clock, so a note that's "due in 3" comes back three sessions later.

## Code

| File | What |
| --- | --- |
| `firmware/src/sg_core.c` | The learning engine. It's plain C with no hardware dependencies. |
| `firmware/src/sg_ui.c` | Screens, input, LEDs and the note scheduler. It's included at the end of `ui_input.c`. |
| `firmware/src/sg_audio.c` | The ring of notes the UI asks for. The sequencer block plays them through `trk_note_on` and `trk_note_off`. It's included from `seq.c`. |
| `tests/sg_test.c` | Engine tests. |
| `tests/sg_ui_test.c` | The mode run on `ui_test.c`'s stubs. It plays a whole session and writes every screen to `build/sg/*.ppm`. |

Felucca itself was changed in only a few places, each marked SIGNAL GARDEN:

- `seq.c`: the include, plus one line next to STEP PREVIEW's audition.
- `ui_draw.c`: one early return in `ui_draw_page`.
- `ui_input.c`: one early return in `ui_input` and one in `ui_leds`.
- `tests/ui_test.c`: `UI_TEST_NO_MAIN`.
- `tests/run_tests.sh`: two new tests.

To build it, see `BUILDING.md`. You need the JieLi toolchain and three files from the AC79 SDK. `.github/workflows/build.yml` downloads both on a GitHub runner.

## Next

1. **Bench test on a real unit.** Confirm the boot gesture, the prompt timing, the LEDs and HOME. The host tests can't prove those.
2. **Save progress across power cycles.** The store is only 296 bytes. Writing it to flash needs a verified free sector pair, outside Felucca's map in `storage.c`, the OTA staging area, and the stock updater's margin. The alternative is joining the settings record with a migration.
3. **Station 88.3:** half and whole steps.
4. **Station 88.4:** intervals.
5. **Keep effects out of drills.** Turn off the selected track's FX sends while a prompt plays.

## Licence

Felucca and these additions are released under GPL-3.0-only. Felucca's engines are MIT and its hardware layer is MPL-2.0; see `LICENSING.md`.
