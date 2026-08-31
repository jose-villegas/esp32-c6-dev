# Beads Workflow Context (esp32-c6)

> Run `bd prime` after compaction, clear, or new session. The SessionStart
> hook already does this automatically.

## What bd is for here — and what it is NOT for

bd tracks the **actionable, cross-session backlog**: banked ideas, deferred
decisions, known bugs not being fixed right now, planned features. It does
**not** replace two things that already exist in this project and stay in
full use:

- **TodoWrite** — in-session step tracking for the task at hand. bd issues
  are for work that outlives this conversation; TodoWrite is for the steps
  of doing that work right now. Don't create a bd issue for "run the
  tests" or "fix the typo I just introduced."
- **The user's cross-project Claude memory system** (`~/.claude/.../memory/`)
  — feedback on how to work, facts about the user/project, external
  references. This is broader than task tracking and travels across every
  project, not just this repo. **Do not use `bd remember`** — routing
  insights through two separate persistent-memory systems just fragments
  them; the existing memory system is the one place for that content.

When memory and bd overlap (a banked idea that's also a real backlog item),
the pattern already in place: bd holds the short, queryable status/
dependency entry; the memory file holds the full design rationale; each
cross-links the other by ID/name. See any of the `*-banked.md` memory
files for the shape.

## When to create a bd issue

- The user raises something explicitly deferred — "let's do this later",
  "banked", a design idea not being built now.
- You discover a real bug or TODO while working on something else, and it's
  not worth fixing inline right now. Link it `discovered-from:<current-id>`
  if there is a current issue in flight.
- A concrete decision is blocking future work but isn't being made right
  now — `--type=decision`.
- Prefer `spawn_task` instead of bd when the thing found is small,
  self-contained, and someone could hand it to a fresh session
  *immediately* — that tool exists for exactly that. Reach for bd when it's
  genuinely backlog (not urgent, may have real dependencies, isn't a
  one-shot fix).

Don't create an issue for routine steps, for something you're about to do
in the next few minutes anyway, or for a vague code-smell observation with
no concrete next action.

## When to update/close

Close a bd issue as part of actually finishing the matching work, not as a
separate end-of-session chore. If a decision issue gets resolved in
conversation, update or close it then — `bd ready` re-derives what's
unblocked automatically from the dependency graph, so downstream issues
don't need manual follow-up.

## Git / sync policy

This repo's standing rule applies over anything bd suggests by default: no
commits, no `git push`, no `bd dolt push` without the user explicitly
asking for it, regardless of how routine it looks. Report what changed and
what commands you'd run; wait for authorization.

## Essential Commands

### Finding work
- `bd ready` — issues with no active blockers
- `bd list --status=open` / `--status=in_progress`
- `bd show <id>` — full detail incl. dependencies
- `bd graph <id>` / `bd graph --all --compact` — dependency view (terminal;
  `bd graph --help` also lists `--html`, but see the note below)

### Creating & updating
- `bd create --title="..." --description="..." --type=task|bug|feature|decision --priority=2 [--deps blocks:<id>|discovered-from:<id>]`
  Priority is 0-4 (0=critical, 2=medium, 4=backlog) — never "high"/"medium"/"low".
- `bd update <id> --claim` / `--notes="..."` / `--append-notes="..."`
- `bd close <id> [<id2> ...] [--reason="..."]`
- **Never `bd edit`** — opens `$EDITOR` and blocks a non-interactive agent.

### Dependencies
- `bd dep add <issue> --blocked-by <depends-on>`
- `bd blocked` — everything currently blocked

## Known rough edges (don't re-discover these)

- `bd graph --html` is NOT actually self-contained despite its own
  --help text: it loads D3 from `https://d3js.org` at view time, and if
  that fails the page silently loses all interactivity (zoom/click) while
  still rendering static boxes. Its "click for details" text is also
  aspirational — the generated script only wires a hover handler, no click
  handler exists. We evaluated and dropped a custom viewer wrapper around
  this (2026-08-31) rather than keep patching around it; `bd graph`
  (terminal/`--compact`) is the reliable option.
- The global `npm install -g @beads/bd` binary has intermittently gone
  missing mid-session on this machine (root cause unconfirmed — several
  worktrees/sessions run concurrently here and any of them can touch
  global npm state). If `bd` is ever unexpectedly not found, don't assume
  it needs reinstalling — check `where bd` / re-run the failing command
  before troubleshooting further.
