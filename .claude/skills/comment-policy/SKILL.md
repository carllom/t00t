---
name: comment-policy
description: Policy for code comments and module_<name>.md/history_<name>.md documentation structure in this repo (t00t). Load before writing or editing code comments, or before writing/editing a module_*.md, history_*.md, or tool README.md file.
---

# Documentation and code comment policy

This is a multi-module embedded application. Documentation is split by role:

- `docs/module_<name>.md` — current spec/usage for a module.
- `docs/logs/history_<name>.md` — that module's development/change log, where it has one.
- `docs/engine.md`, `docs/building.md` — shared/platform documentation spanning modules.
  `module_*.md` files may freely reference these.
- `docs/logs/architecture.md`, `docs/logs/migration.md` — settled design/porting plans, kept
  with the logs rather than the live spec.
- `CONTEXT.md` (repo root) — top-level entry point/onboarding, links to all of the above.

Keep `module_<name>.md` and `history_<name>.md` strictly separated by role (see below) —
letting plan/process narrative drift into the module spec, or letting spec content drift
into the log, is the failure mode this policy exists to prevent.

## Module documentation `docs/module_<name>.md`

Describes current module state and, superficially, what's planned going forward.

Should contain:

- Overview on a functional level: module type/focus, specifications (voices, oscillators,
  LFOs, ADSRs, filters, effects, arpeggiators, sequencers), MIDI input capabilities, display
  capabilities.
- Overview on a technical level:
  - Source layout — module-specific files and a one-line responsibility for each.
  - Architecture — how the module's pieces fit together at runtime (render pipeline,
    core/thread split, data flow). Promote to its own top-level section if it outgrows the
    technical overview.
  - Build information — flags, module-specific build switches.
  - Tools — one-line description of each tool belonging to the module, linking to the tool's
    own `README.md` for setup/usage/build detail rather than duplicating it.
- Module status and plan — current state and planned/suggested features only. Not historic or
  already-implemented features (those belong in the log).
  - Current performance values, simplified (a couple of indicative numbers, not a full duty
    cycle table).
  - Future phases/TODOs, described superficially — implementation detail belongs in the log.
- Decision record — rationale for settled design/implementation choices: the decision and the
  reason, not the evidence trail (measurements, alternatives explored, dead ends) that led to
  it. That's what the depth/presence line in code comments (below) pushes out of source into.
- Glossary — terms specific to this module or with a module-specific meaning (e.g. "partial",
  "voice", "seqtable").

Should **not** contain:

- Development/process narrative, or a description of the implementation journey.
- Development plans, beyond a superficial mention of not-yet-implemented steps.
- Detailed performance/precision measurements (duty cycle tables, frequency error tables).

## Tool `README.md` files

Tools under `tools/` may have their own `README.md`. They fall under this policy for session
leakage specifically: no development narrative, no phase tags, no "why we did X instead of Y"
journey. Otherwise free-form, not bound to the `module_*.md` layout above — link to them from
the owning module's Tools list rather than restructuring them to match it.

## Module development/change log `docs/logs/history_<name>.md`

Where the plan and process for a module live: what was planned initially, and the
development/measurement/debugging steps that led to the current implementation. Phase
references (`P<n>`, `F<n>`) and GitHub issue references belong here, not in the module doc or
in source.

Ordered as a log — earliest plan first, changes appended as implementation progresses. When
moving content here during a refactor, insert it approximately in time order; don't add or
invent new text in the refactor itself. New text only gets written during actual planning or
implementation.

## Code comments

- Document how the code works in its _current state_. Not the journey to that state, not the
  requirements that led to the function/type existing.
- If there's an optimization, it's enough to say it was made and what tradeoff it costs — e.g.
  "using y because it's more efficient than x." Don't quantify it ("4x faster") unless the
  number itself is load-bearing for understanding the code.
- Arguments/rationale for a code choice belong in `module_*.md`'s Decision record, not in the
  comment.
- The line between comment and `module_*.md` is **depth, not presence**. A one-clause
  pragmatic reason — naming the constraint or the rejected alternative — stays in the comment,
  because it's what stops someone from "fixing" the code back to the naive version. The
  evidence behind that reason (measurements, alternatives considered, confidence) belongs in
  `module_*.md`. Example: for a saturating multiply, "reciprocal multiply, since the platform
  has no hardware 64-bit divide" stays in the comment; the cycle counts and the comparison
  against the textbook algorithm move to `module_*.md`.
- Don't justify a choice in a comment by cross-referencing another module's precedent (e.g.
  "same reasoning as X's case", "unlike Y, this permits Z outright"). State the constraint or
  reason on its own terms — a local decision doesn't need to lean on a comparison elsewhere to
  be justified.
- Don't cite `module_*.md`/`history_*.md` section numbers from a code comment. The doc stands
  on its own; a comment shouldn't function as an index into it.
- Remove phase/tier tags (`P0`–`P6`, `F0`–`F7`) from code and tool comments, even where the tag
  has drifted into being used as a fixed name for a specific rig or comparison effort rather
  than a live planning marker — use a plain descriptive name instead (e.g. "the SID comparison
  rig", not "the F0 rig"). The tag itself, and which phase/issue it corresponds to, belongs in
  `module_*.md`/`history_*.md`.

When applying this policy to existing code, don't change everything blindly — read the
surrounding context, and if there's a good argument for a comment to remain as-is, that's
worth raising rather than deleting on sight.
