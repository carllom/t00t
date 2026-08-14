# Docs & source-comment refinement — tracking file

**Temporary.** Lives on the `docs` branch only. Delete this file as the last
step before merging `docs` into `main` (Phase 5).

Read this file first at the start of any session working on this effort —
it's the resume point. Update it before ending a session, even mid-phase.

## Why this exists

The module/history doc split (chip/fm/groovebox/speech/tracker/subtractive →
`module_X.md` + `history_X.md`, `engine.md` trimmed to cross-module
architecture) is done and committed. Before merge, five follow-on passes are
needed: fix references broken by the split (in docs and in source comments),
anonymize personal name references, make the six module docs structurally
consistent, validate documented claims against actual code, and clean up
source comments that narrate change history instead of describing current
behavior. This file tracks that multi-session effort so it survives context
loss between sessions/workstations.

Full phase rationale lives in the plan that produced this file (session
history, not reproduced here) — this file tracks *state*, the bullets below
are enough to act on without it.

## Phase checklist

- [x] **Phase 0** — this tracking file (commit `d589bfa`)
- [x] **Phase 1** — reference correctness (docs + source comments)
  - [x] 1a. Fix 3 doc-internal cross-reference issues (commit `50ab737`)
  - [x] 1b. Fix 463 source-comment references across 103 files, batched by
        module/directory (5 parallel agents: chip, fm, speech, tracker,
        groovebox/subtractive/shared — commit `69e98f7`). Actual count
        landed at 463 lines, not the original audit's 464 — one of the
        "hits" (`src/engines/fm/audio_engine.cpp:184`) was already correct
        and deliberately left untouched.
  - [x] 1c. Verification sweep — repo-wide grep for old filenames (bare,
        unprefixed) in `src/`+`tools/` returns 0 hits; grep for malformed
        `module_module_`/`history_history_`/`history_module_`/
        `module_history_` artifacts (one batch had a sed-ordering bug,
        self-caught and fixed before reporting) returns 0 hits; the one
        remaining `engine.md` hit is confirmed legitimate (Trigger/Gate
        Signaling genuinely still lives there). Also spot-checked the
        non-comment-marker-prefixed diff lines (continuation lines inside
        block comments/docstrings) to confirm no real code was touched —
        all clean.
- [x] **Phase 2** — section reordering across the six `module_*.md` docs
      (commit `96fa4ed`)
  - [x] Design canonical section order (proposed, approved — see below)
  - [x] Applied per doc via 5 parallel agents (chip/fm/groovebox/speech/
        tracker; subtractive untouched by design). **Physically reordered
        `##` blocks only — did NOT renumber the "N." prefixes**, per the
        decision below.
  - [x] Verified: sorted-line diff empty for all 5 files (pure permutation,
        zero content added/removed/altered) and line counts identical to
        the pre-reorder baseline (chip 1012, fm 1067, groovebox 562,
        speech 910, tracker 1020). No citation-fix commit was needed —
        `git diff --stat` for the reorder commit shows equal insertions/
        deletions per file (pure line-position churn), confirming zero
        citation text was touched.

  **Decision recorded:** chip has 151 external source citations + 190
  internal self-refs using `§N` numbers, fm has 67 + 140. Renumbering now
  would re-break everything Phase 1 just fixed. Since heading text
  (including its number) stayed exactly as-is and only physical position
  moved, `§5.2` still finds a section literally titled `### 5.2 ...`,
  wherever it now sits. **Numbers now read out of sequence within each
  reordered doc** (e.g. `module_chip.md` goes §1, §2, §3, §7, §4, §5, §6...)
  — deliberate, deferred to a later, separately-scoped renumbering pass
  (the user may want additional reordering/restructuring first, so
  renumbering happens once, at the end, not after every future reshuffle).
  No doc had a manual table-of-contents block that would also have needed
  updating.

### Canonical section order (approved)

Applies to `module_chip.md`, `module_fm.md`, `module_groovebox.md`,
`module_speech.md`, `module_tracker.md`. `module_subtractive.md` is left
untouched — it has no Scope/Reuse/Architecture/Decisions/Questions framing
at all (pure ADSR/LFO/Waveform/SVF reference content), and forcing empty
headers onto it would invent structure it doesn't have.

1. Prerequisites (if present)
2. Scope & phasing (or equivalent — "Goals" for tracker, "Scope" for speech)
3. Reuse inventory (if present)
4. Architecture integration (including any nested voice-allocation subsection)
5. DSP detail / New DSP components / Data structures — the technical core.
   Judge by actual content, not just title (e.g. fm's "§3 The performance
   question" is budget/cycle-cost analysis despite its name — see bucket 9,
   not this one). Keep multi-section blocks (chip's Voice model/Filter
   buses/Frame table VM) together, in original relative order.
6. MIDI / control mapping (only if it's already a standalone top-level
   section — don't extract one from embedded content)
7. Voice allocation (only if standalone — most docs already nest it in #4/#6)
8. Host tooling
9. CPU / memory / performance budget
10. Module-specific extras, kept together in original relative order —
    Testing, Backporting, Display, Feedback to Existing Engines, Optional
    Extensions, Sequencer (future), Code layout & integration strategy,
    Other chips (future), The other option (alternative approach) — whatever
    a given doc actually has, nothing invented
11. Settled decisions (chip/speech/tracker only)
12. Open questions
13. Recommended build order, then Summary — new-code list if both exist
14. Glossary (chip only, last — it's an appendix)
- [ ] **Phase 3** — anonymize "Carl" → "the author"
  - [ ] Folded into each module's Phase 4 pass (see per-module table below)
- [ ] **Phase 4** — per-module deep validation + comment cleanup (see table)
- [ ] **Phase 5** — wrap-up: final grep sweep, delete this file, hand off for
      merge review

## Phase 4 per-module status

Order: groovebox → subtractive → speech → tracker → chip → fm (smallest/
simplest first). Each module has two sub-deliverables, tracked separately.
"Carl→author" (Phase 3) is folded in here per module, not a separate sweep.

| Module | 4a. Doc validation | 4b. Comment cleanup | Carl→author |
|---|---|---|---|
| groovebox | done (commit `9720744`) | done (commit `9720744`) | n/a (0 hits, confirmed) |
| subtractive | done (commit `9720744`) | done (commit `9720744`) | n/a (0 hits, confirmed) |
| speech | done (commit `1d2d3b1`) | done (commit `1d2d3b1`) | done (commit `1d2d3b1`, 3/3 hits) |
| tracker | done (commit `5f743f1`) | done (commit `5f743f1`) | done (commit `5f743f1`, 2/2 hits in module_tracker.md; 1 hit remains in tracker_song_blob.h, out of scope, see session log) |
| chip | done (commit `420a762`) | done (commit `420a762`) | done (commit `420a762`, 9+3 doc hits + 6 newly-found source hits, all 18) |
| fm | not started | not started | not started (module_fm.md 4, history_fm.md 9) |

`CONTEXT.md`'s 2 "Carl" hits (hardware-ownership: "Carl's actual rig" /
"Carl runs the breadboard board") aren't module-doc content — handle in
Phase 5 wrap-up or opportunistically, not blocking any single module's pass.

## Baseline findings (from the investigation that produced this plan)

### Doc-to-doc cross-reference issues (3 total)

1. `module_fm.md` (3 occurrences, lines ~264-265, ~994-995, ~1043): cites
   `` `history_fm.md` §"FM P2 BLOCK Confirmation (#45)" `` — no such heading
   exists. Real heading: `### FM Engine — EnvDX + BLOCK Confirmation (#45)`
   in `history_fm.md`. (One correct instance of the citation already exists
   at `module_fm.md:185-186` — the wrong wording is inconsistent, not
   universal.)
2. `history_speech.md:192` references "`module_speech.md`'s Open Question 2
   (voice count)" — current Open Questions item 2 is "Display", not voice
   count. Predates the split, not caused by it, but still wrong.
3. `tools/fm_ref/README.md:98` — bare `§3.2` with no filename; content
   matches `history_fm.md` §3.2, not `module_fm.md` §3.2. Low priority,
   resolvable from context.

### Source-comment references to old doc filenames (464 lines / 103 files)

Per old filename:

| Old name | Files | Lines |
|---|---|---|
| `chip.md` | 41 | 175 |
| `fm.md` | 14 | 89 |
| `fm2.md` | 26 | 56 |
| `tracker.md` | 19 | 71 |
| `speech.md` | 17 | 64 |
| `engine.md` | 7 | 7 |
| `groovebox.md` | 2 | 2 |

Top hit-count files: `src/engines/chip/audio_engine.cpp` (28),
`src/engines/fm/op.h` (21), `src/engines/fm/rig.h` (16),
`src/engines/fm/patch.h` (16), `src/engines/tracker/player.h` (15),
`src/engines/tracker/mixer.h` (15), `tools/syx2patch.py` (14),
`tools/fit_6581_filter.py` (13), `src/chip/sid_osc.h` (11),
`src/engines/speech/engine.h` (10), `src/chip/sid_filter.h` (10),
`tools/sid_ref/make_streams.py` (9), `src/engines/chip/engine.h` (9),
`src/engines/fm/lfo.h` (8), `src/engines/fm/audio_engine.cpp` (8).

Reference shapes:
- **(A) bare `§N` cites, target section still lives in `module_X.md`** — the
  bulk of hits. Fix: just add the `module_` prefix.
- **(B) cites to content that moved to `history_X.md` during the split** —
  needs the history file, not just a prefix. Known cases:
  - chip's `§14a`–`§14f` (and "§14 item N" without a letter) — ~12 hits, e.g.
    `src/engines/chip/ay_osc.h:22` (`chip.md §14a.7`),
    `src/engines/chip/audio_engine.cpp:25` (`chip.md §9/§14a.9` — note §9
    itself stays in `module_chip.md`, only the *measured-number* citation
    needs `history_chip.md`; check each `§9/§14a.9`-style compound citation
    individually), `src/engines/chip/audio_engine.cpp:411` (`chip.md §14d.5`),
    `src/engines/chip/speaker_sim.h:59` (`chip.md §14d.5`),
    `tools/ins2chip.py:213` (`chip.md §14e.3`).
  - 6 of the 7 `engine.md` hits — content moved out during the 2240→305 line
    trim:
    - `src/engines/fm/op.h:40` — `engine.md "FM P2 BLOCK Confirmation (#45)"`
      → wrong file AND wrong title (same mismatch as doc issue #1 above) →
      `` history_fm.md §"FM Engine — EnvDX + BLOCK Confirmation (#45)" ``
    - `src/engines/speech/engine.h:9` — `engine.md "Speech Engine P2
      Profiling (#31)"` → `history_speech.md` (title itself is correct,
      heading exists there verbatim)
    - `src/engines/speech/audio_engine.cpp:67` — `engine.md "Tracker Engine
      -- 32-Voice Mixer (#15/#16)"` → `history_tracker.md` (title correct)
    - `src/engines/tracker/mixer.h:282` — `engine.md's tracker interpolation
      measurement` → `history_tracker.md`
    - `tools/syx2patch.py:12` — `engine.md's xm2t00t precedent` →
      `history_tracker.md` ("xm2t00t Host Converter (#14)" section)
    - `tools/xm2t00t/periods.py:16` — `engine.md and the plan for #14` →
      `history_tracker.md`
    - (the 7th, `src/engines/fm/audio_engine.cpp:184`, "Trigger/Gate
      Signaling, engine.md", is CORRECT as-is — that section survived the
      trim intact — leave it alone)
- **(C) prose-style, no `§`** — e.g. `src/engines/groovebox/audio_engine.cpp:20`
  "See groovebox.md." → `module_groovebox.md`. Both `groovebox.md` hits
  repo-wide are this simple form.
- **(D) `fm2.md` hits** — same underlying document as `fm.md` under its
  "Attempt 2" name (per `history_fm.md:4-8`: `fm.md` at "Attempt 1"/branch
  `fm`, became `fm2.md` at "Attempt 2", now split into `module_fm.md` +
  `history_fm.md`). Target depends on which section is cited — measurement/
  evaluation-narrative sections (most of `fm2.md`'s own content, e.g. §1-6)
  → `history_fm.md`; anything citing current spec → `module_fm.md`. Check
  individually, same as chip's §14 hits.

No board headers (`src/boards/*.h`) affected — confirmed zero hits, no
comment-length constraint issue there.

### "Carl" mentions (33 occurrences / 6 files) — for Phase 3

Replacement term: **"the author"** (decided).

| File | Hits |
|---|---|
| module_chip.md | 9 |
| history_fm.md | 9 |
| module_fm.md | 4 |
| module_speech.md | 3 |
| history_chip.md | 3 |
| CONTEXT.md | 2 |
| module_tracker.md | 2 |

12 distinct sentence patterns cataloged (don't blind-search-replace — reword
per pattern so it stays grammatical):

1. Decision attribution — "Carl's call" / "Carl chose to" / "Carl picked X" →
   "the author's call" / "the author chose to" / "the author picked X"
2. Joint decision — "Decided with Carl" → "Decided with the author" (or
   reword if this reads oddly once anonymized — flag if so)
3. Own request — "Carl's own ask" → "the author's own ask"
4. Own correction — "Carl's own correction" → "the author's own correction"
5. Pushback — "Carl pushed back on..." → "The author pushed back on..."
6. Pending, adjectival — "still Carl's to do" → "still outstanding" or "still
   the author's to do" (check which reads better per instance)
7. Pending, imperative header — "Still needs Carl:" → "Still needs the
   author:" or "Still pending:" (check per instance)
8. Pending, "presence needed" — "needs Carl at the bench" → "needs the
   author at the bench"
9. Testimony with quote — `` Carl: "quote" `` → `` The author, on real
   hardware: "quote" `` (or similar light rewording, not a bare name swap)
10. Testimony, parenthetical, no quote — "(Carl, first P4 by-ear pass)" →
    "(the author, first P4 by-ear pass)"
11. Verification result, past action — "Carl's [X] pass found..." → "the
    author's [X] pass found..."
12. Reported feedback — "Carl's report:" / "Carl re-flashed and reported
    back" → "The author's report:" / "The author re-flashed and reported
    back"

`CONTEXT.md`'s 2 hits are a 13th pattern not in the above list — rig
ownership ("Carl's actual rig", "Carl runs the breadboard board") — these
read fine as "the author's actual rig" / "the author runs the breadboard
board" but live in `CONTEXT.md`, not a module doc, so handle at Phase 5
wrap-up rather than inside a Phase 4 module pass.

### Section ordering comparison (for Phase 2)

Six `module_*.md` files, current `##` header order, is fully cataloged in
this session's investigation (not reproduced here — re-run the same audit at
the start of Phase 2 if this file's context has aged, since Phase 1's edits
won't change headers but Phase 4's might). Headline findings:
- `module_subtractive.md`: only 4 top-level headers (ADSR/LFO/Waveform
  Types/SVF) — no Scope/Reuse/Architecture/Decisions/Questions/Build-order
  framing at all. Leave as-is for any canonical slot it has no content for.
- `module_chip.md`: only doc with a Glossary (last section).
- `module_speech.md`: only doc using unnumbered bare-word headers (no "N."
  prefix); "Architecture Placement" appears unusually early (#3 of 14).
- `module_tracker.md`: only doc where "Build Order" comes after "Open
  Questions" (last header overall).
- Settled Decisions exists only in chip/speech/tracker, not fm/groovebox/
  subtractive.
- MIDI/control mapping has its own explicit header only in groovebox/speech;
  chip/fm/tracker have no such header (content, if present, unlabeled).

### Source comment "history narrative" scope (for Phase 4b)

~2,004 of 6,057 `//` comment lines in `src/` (≈33%) look history-narrative-
shaped (issue-number refs, "used to be X", before/after reasoning) rather
than current-behavior description. Strict issue-number-only count: 305 lines
/ 41 files. Concentration by directory:

| Directory | Comment lines | Issue-ref lines |
|---|---|---|
| engines/fm | 1,433 | 104 |
| engines/tracker | 887 | 62 |
| engines/chip | 884 | 9 |
| engines/speech | 866 | 124 |
| chip/ (shared SID/AY primitives) | 686 | 0 (narrative without `#N` tags) |
| engines/groovebox | 331 | 0 |
| src/ root (shared) | 311 | 5 |
| engines/subtractive | 181 | 1 |

`engines/subtractive/` and `engines/groovebox/` are already close to clean —
Phase 4b for those two modules should be quick verification, not a rewrite.

Hot-file list (20+ narrative lines each, carries the bulk of the work):
`src/engines/tracker/player.h`, `src/engines/fm/op.h`,
`src/engines/fm/patch.h`, `src/engines/fm/lfo.h`, `src/engines/fm/rig.h`,
`src/engines/chip/rig.h`, `src/engines/tracker/mixer.h`,
`src/engines/fm/midi_controller.cpp`, `src/engines/speech/midi_controller.cpp`,
`src/engines/speech/render.h`, `src/engines/fm/audio_engine.cpp`,
`src/engines/speech/audio_engine.cpp`, `src/engines/chip/audio_engine.cpp`,
`src/engines/chip/ay_osc.h`, `src/chip/sid_voice.h`, `src/chip/sid_filter.h`,
`src/chip/sid_osc.h`, `src/engines/speech/tract.h`,
`src/engines/speech/sequencer.h`, `src/engines/speech/engine.h`,
`src/engines/chip/engine.h`.

Target shape for "good" (keep, don't shrink further): `src/chip/sid_osc.h`'s
`SID_WAVE_ZERO_6581` comment — short, states current behavior, expands only
because the value (0x380, not the "obvious" 0x800) is genuinely non-obvious,
cites the real-hardware source for it. No issue number, no "used to be."

Target shape for "bad" (rewrite, move history to `history_X.md`):
`src/engines/fm/env_dx.h:9-17`, `src/engines/fm/lfo.h:51-100`,
`src/engines/fm/patch.h:335-340`, `src/engines/fm/op.h:151-176`,
`src/engines/tracker/player.h:970-978`, `src/engines/tracker/mixer.h:40-73`,
`src/engines/speech/midi_controller.cpp:10-24` — all multi-sentence
before/after narratives citing issue numbers, exact examples of the pattern
to fix.

## Lessons from Phase 1 (useful for Phase 2's similar renumber/rename work)

- **`chip.md §14 item N` (no letter suffix) is NOT the same as `§14a`-`§14f`.**
  `module_chip.md`'s own §14 "Recommended build order" has a real numbered
  list (1. P0 rig, 2. P1 skeleton, ...) that bare `item N` citations match
  correctly — only the *letter-suffixed* forms moved to `history_chip.md`.
  Checked against real headings before generalizing; don't assume every
  `§14`-anything moved.
- **Compound citations may need splitting across two files.** E.g.
  `chip.md §9/§14a.9` became `module_chip.md §9 / history_chip.md §14a.9` —
  §9 (CPU budget, the claim) stayed in the module doc, §14a.9 (the measured
  number backing it) is in history. Don't force a compound citation into one
  file if its two halves genuinely live in different docs now.
- **Sed/bulk-replace ordering matters when two rename rules can chain.**
  One batch ran `fm2.md`→`history_fm.md` before `fm.md`→`module_fm.md`, and
  the second rule's `fm.md` pattern then matched inside the just-produced
  `history_fm.md`, corrupting it to `history_module_fm.md` in 19 files.
  Caught by a post-fix grep for `history_module_`/`module_module_`-style
  artifacts — worth running that check after any multi-pattern bulk edit,
  not just a "did the old string disappear" check.

## Session log

- 2026-08-14: Investigation done (3 parallel Explore agents), plan approved,
  this tracking file created (Phase 0). Phase 1 complete: 3 doc-internal
  fixes (commit `50ab737`) + 463 source-comment reference fixes across 103
  files via 5 parallel agents (commit `69e98f7`). Verification sweep clean.
  Phase 2 complete: canonical order proposed and approved; user caught that
  renumbering would re-break Phase 1's 463 fixes (151+190 chip, 67+140 fm
  citations/self-refs) and decided to reorder without renumbering, deferring
  renumbering to a later pass (recorded above). Applied via 5 parallel
  agents (commit `96fa4ed`), verified via sorted-line diff (empty for all 5
  files) and line-count match. Next: Phase 4 (per-module deep validation +
  comment cleanup, Phase 3's anonymization folded in per-module) — check
  with the user before starting, it's the largest remaining phase and will
  likely span multiple sessions. A future session should also decide when
  to do the deferred renumbering pass for chip/fm (could be its own step
  before Phase 5, or folded into chip/fm's Phase 4 pass as originally
  discussed).
- 2026-08-14 (same day, later session): Started Phase 4. Read
  `module_groovebox.md` directly and spot-checked it against
  `src/engines/groovebox/engine.h` — confirmed the doc is genuinely stale as
  its own intro admits: §5.2 describes a `union` `VoiceParams` with
  `VT_TB303/VT_DRUM_BD/VT_DRUM_SNARE/VT_DRUM_TOM/VT_DRUM_CLAP/VT_DRUM_METAL/
  VT_DRUM_SAMPLE/VT_DRUM_RIM` and per-type param structs; actual code is a
  **flat struct** with `VT_SILENT/VT_TB303/VT_DRUM_BD/VT_DRUM_TOM/
  VT_DRUM_SNARE/VT_DRUM_HAT/VT_DRUM_METAL/VT_DRUM_CLAP` — no `VT_DRUM_SAMPLE`,
  no `VT_DRUM_RIM`, no per-type structs, and an undocumented `VT_DRUM_HAT`.
  Confirmed no `history_groovebox.md` exists (groovebox never went through
  the old single-file split other modules did — as-built notes for it belong
  inline in `module_groovebox.md`, not a history-file cross-reference).
  Launched 2 parallel background agents (groovebox, subtractive — smallest
  modules first per the stated order) to do the actual 4a validation + 4b
  comment-cleanup passes. **Both were killed mid-flight by the account
  session rate limit** (resets 17:40 Europe/Stockholm) — neither had made
  any file edits yet (confirmed via `git status`/`git diff`, clean), each
  had only gotten as far as the "Carl" hit-count sanity grep (0 hits
  confirmed for both `module_groovebox.md` and `module_subtractive.md`,
  matching the prior audit). **No corruption, no partial edits to review —
  safe to just re-launch fresh agents for groovebox and subtractive next
  session with the same brief.** Next session: retry Phase 4 starting with
  groovebox + subtractive, then speech → tracker → chip → fm per the
  existing order.
- 2026-08-14 (same day, third session after rate-limit reset): Re-launched
  the groovebox and subtractive agents with the same briefs. Both completed
  cleanly this time — reviewed each diff by spot-checking the agents'
  claims directly against source (`ladder.h`, `clap.h`, `metal.h`,
  `engine_base.h`, `CMakeLists.txt`, `kit.h`'s `GrooveVoice` enum,
  `polyblep.h`, `filter.h`) before committing; all claims held up.
  Committed as `9720744`. groovebox got ~20 as-built annotations (its doc
  was pure design-draft, never checked against what shipped — real gaps
  found: `VoiceParams` is a flat struct not a union, 303 uses a dedicated
  ladder not the shared SVF, rimshot was never built, CH/OH hi-hat share
  one voice slot rather than two, `voice_alloc.cpp` actually is linked
  into the build, no `route.cpp` anywhere, plus a wrong
  `engine.md`→`architecture.md` citation fix in §9 not part of the
  original Phase 1 list). subtractive needed only 3 small fixes (stale
  RP2040→RP2350 ref, wrong SVF op count, a sub-block-vs-per-sample
  computation caveat) — it was already close to accurate. Both confirmed 0
  "Carl" hits. Per-module table updated below. Next: speech (has a
  `history_speech.md`, 3 "Carl" hits to fold in, and 124 issue-ref comment
  lines per the baseline audit — the first module in this phase with real
  anonymization + comment-history work to do, not just validation).
- 2026-08-14 (same day, continued): speech done in one combined agent
  (4a+4b+Carl→author together, same approach as groovebox/subtractive).
  Real drift found: `MAX_SPEECH_VOICES` was cited throughout the doc and
  several source comments but the actual constant has always been
  `MAX_VOICES`; the res2p.h groovebox-backport Settled Decision was
  checked off as done but never happened; the "Display" open question was
  stale since #37 shipped it (struck, moved to Settled Decisions, new
  `history_speech.md` section added); two copy-paste typos referenced
  `history_subtractive.md` instead of `history_speech.md`; two source
  comments stated outright wrong current behavior (audio_engine.cpp's
  "MAX_VOICES=4", phoneme_def.h's "read by nothing yet" for fields
  sequencer.h has read since #34), not just stale narrative — worth noting
  since 4b was scoped as narrative cleanup but caught real doc-in-comment
  bugs too. 124 issue-ref comment lines cleaned across 13 files. All 3 Carl
  hits reworded. Verified `make ENGINE=speech`, `SPEECH_PROFILE=1`, and
  `make host` build clean before committing (commit `1d2d3b1`) —
  spot-checked several of the agent's specific claims against source first
  (MAX_VOICES value, res2p.h header comment, phoneme count). Next: tracker
  (per the stated module order).
- 2026-08-14 (same day, continued): tracker done (4a+4b+Carl→author, one
  combined agent). Real drift found: a function name cited 3 times
  (`tracker_apply_pitch_vol_effect()`) was never defined anywhere — the real
  split is `tracker_tick_period()` + `tracker_apply_tick_volume_effects()`;
  four stale "resolving open question N below" back-references left over
  from earlier Open-Questions-list churn; the Display section was still
  written in pre-build future tense despite #24 shipping it. 62 issue-ref
  comment lines cleaned across all 10 tracker files (concentrated in
  `player.h`/`mixer.h`); two comments stated outright wrong current behavior
  (engine.h's `VoiceParams` "no tick handoff yet", mixer.h's
  `TrackerTickState` "no player/TickBlock ring yet" — both false since #18).
  Also fixed a matching stale tag in `tools/xm2t00t/blob_format.py`
  (blob_format.h's generator) and regenerated to confirm byte-identical
  output. 2/2 `module_tracker.md` Carl hits reworded. Verified
  `make ENGINE=tracker` (clean rebuild), `make host`, and
  `render_tracker_mixer`'s regression suite all pass. Commit `5f743f1`.

  **Scope discovery:** a repo-wide `grep -rl Carl` (not just the 6
  module/history docs + CONTEXT.md the original Phase 3 baseline audited)
  turns up 9 more files with "Carl" mentions the original audit never
  counted, because it only scoped `.md` docs: `src/engines/fm/audio_engine.cpp`,
  `src/engines/chip/display.cpp`, `src/engines/chip/speaker_sim.h`,
  `src/engines/chip/audio_engine.cpp`, `src/engines/tracker/tracker_song_blob.h`
  (already flagged above, out of scope for this pass), and
  `tools/host_render/render_fm.cpp`. All are source *comments* (testimony/
  attribution patterns similar to the 12 already cataloged, e.g. "Carl's own
  ask", "Carl heard the difference", "Carl's by-ear pass"), not doc prose.
  Folding these into the chip and fm module passes below (their own files),
  since those two modules are next; `tools/host_render/render_fm.cpp` isn't
  under `src/engines/`, so treat it as part of fm's pass too (same reasoning
  `module_groovebox.md`'s tooling citations get folded into a module pass —
  it's fm's own host tool). Next: chip.
- 2026-08-14 (same day, continued): chip done — the largest module doc pair
  so far (2136 combined lines across module_chip.md/history_chip.md, 18
  source files). Real drift found: the intro overclaimed "none of P1/P2/P3
  re-measured or listened to on hardware" when P1-P3 each had a real by-ear
  pass (split into the accurate CPU-not-remeasured vs. listening-has-happened
  claims); a stale "Glossary belongs in architecture.md" note (architecture.md
  has no glossary — §16 is where it actually lives); a false `engine.md`
  citation for the 3401 c/f baseline (post-trim, that number no longer
  exists there); 7 wrong `§12.x` citations in source comments (AY's own
  P0-P3 sub-phases are `§12.1`-`§12.4`, several comments mixed up the phase
  label with the section number); a stale `VoiceParams.freq` comment
  claiming AY has no frame VM (false since AY-P2). Also fixed stale
  doc-filename refs in `CMakeLists.txt` (`chip.md`) and `Makefile` (four
  `sid.md` refs — an even older filename) that Phase 1 never scoped (that
  pass covered `src/`+`tools/` only, not root build files) — **flagging
  that `CMakeLists.txt`/`Makefile` likely still carry other stray
  `tracker.md`/`fm.md`/`engine.md` refs beyond what this pass touched,
  worth a small dedicated sweep at Phase 5**. All 18 Carl hits reworded
  (9 module_chip.md + 3 history_chip.md + 6 source-comment hits from the
  scope discovery). Verified `make ENGINE=chip` and `make host` both pass.
  Commit `420a762`.

  **One unresolved discrepancy flagged for Carl, not auto-fixed:**
  `history_chip.md` §14d.5's documented ARP_LEAD arpeggio-speed fix says
  "repeating each note's row 6x (120 ms/note, 480 ms/cycle, ~2.1 Hz)", but
  the actual shipped data (`tools/chip_instruments.txt`, `instruments.h`)
  uses `x2` (8 rows, 40 ms/note, 160 ms/cycle, ~6.25 Hz) — and AY's own
  arpeggio (§12.3) is consistent with the *current* `x2`/8-row shape, not
  the documented `6x`. Squashed git history for this module makes it
  impossible to tell whether §14d.5's "6x" was a math slip when the
  section was written, or a later re-tune of the actual data that was
  never re-documented. Needs a human call — not something to guess at
  silently in a docs-cleanup pass. Next: fm (the last module — also carries
  1 more discovered source-comment Carl hit in `tools/host_render/render_fm.cpp`,
  outside `src/engines/`, per the scope-discovery note above).
