# Model Delegation Workflow

How to hand a feature's *implementation* to a local Ollama model and/or a
free-tier model through OmniRoute, while keeping the investigation, review,
and verification with you. Written after actually running this once (a sand
feature: acid evaporation, then sand-floats-on-oil) - the steps below are
what worked, and the gotchas are the specific things that silently didn't.

Living document: update it when the providers, combos, or failure modes
change. They will - see "Things rot" below.

Use this when a user says something like "delegate this to local/free
models" or "use Ollama for local, OmniRoute for free tier." Don't reach for
it unprompted - it is slower than doing the work yourself, and it exists to
answer a specific request, not as a default implementation strategy.

---

## The division of labor

**You** do the parts that need the codebase's actual context: reading the
relevant files, finding the exact insertion point, checking whether the
approach has already been tried and rejected (grep the function name -
this codebase in particular leaves detailed "why not X" comments precisely
to stop someone re-treading a dead end), noticing constraints a model
without that context has no way to know about (a hot-path budget, a struct
size ceiling, an existing invariant a test already pins), and - once the
shape of the change is settled - **breaking it into a list of atomic
edits** (see step 3 below). Deciding the design is your job. Typing out
every resulting field, comment, doc paragraph, and test body yourself is
not, by default.

**The delegate models** do small, mechanical, fill-in-the-blank text
generation: "here is the exact current code, here is exactly what changes,
give me the replacement block." Not "implement feature X" - that's still
your job, just handed out one precise snippet at a time. This covers far
more than tricky logic diffs - it is the *default* home for struct field
declarations plus their doc comments, material/config table rows, doc
prose given a fixed set of facts to state, mechanical call-site updates
repeated across a file, and test function bodies given the exact scene-
building helpers and exact assertions to make. If you can write the spec
for an edit in one paragraph of "here is the current text, here is the
change," it is a delegation candidate - do not default to writing it
yourself just because you already know what it should say.

**You again** review what comes back, reconcile it with what you already
know, apply it by hand (rewriting comments to match house voice - delegate
output tends to be terser and in a different register), and verify with a
real build and the real test suite. A model's confident-sounding output is
not evidence; the test suite passing is.

**Self-check before calling the feature done:** count how many atomic
edits landed in the diff, and how many of them came from a delegated call
versus a direct Edit you wrote yourself. If self-authored edits are the
majority, the workflow was not actually followed, even if the feature is
correct and every test passes - go back and re-delegate what should have
been delegated in the first place. "I already knew the answer" is not a
reason to skip the call; the point of this workflow is the local model
doing the typing, not you privately drafting the same text.

---

## Step by step

1. **Investigate and plan first, entirely yourself.** Read the code around
   where the feature has to land. Identify every existing field, function,
   or convention the change should reuse rather than reinvent. If the
   codebase has comments explaining why an adjacent approach was rejected
   (search for the function/mechanism name across the tree), read them -
   they exist to save you from proposing the same rejected idea again.

2. **Write the spec as literal, current code, not prose.** Quote the
   *exact* current content of every block that's changing, state the
   change in plain imperative terms, and specify the exact output format
   you want back (e.g. "reply with only fenced code blocks, one per file,
   no other commentary"). A model asked to "add evaporation to acid" will
   invent variable names and drift from house style; a model handed the
   real `step_one_dissolver_cell()` body and told exactly which two things
   to change inside it will not.

3. **Keep each delegated ask small - and before writing any code yourself,
   write out the FULL list of atomic edits the feature needs.** One field,
   one function, one struct-literal block, one doc paragraph, one test
   body per call - not a five-file feature spec in one prompt, but also
   not silently absorbed into "the parts that need context." Go through
   every file the plan touches and list each self-contained change as its
   own line (e.g. "material.h: two new reaction_t fields + comments",
   "material.c: dirt's row gains four fields", "suite_sand.c: rewrite
   test X's assertion", "docs/Foo.md: revise the 'Decisions' paragraph").
   Every line on that list is a delegation candidate by default; only pull
   one back to do yourself if it genuinely cannot be specified without
   context a model would have to reconstruct from scratch (a novel
   algorithm's core logic, a change spanning an invariant that isn't
   written down anywhere). This is where the previous version of this doc
   went wrong in practice - see "The failure mode this section used to
   allow" below.

   This isn't just cleanliness: large prompts to local/free endpoints
   reliably **timed out** in practice, while the same content split into
   three small asks (one per file) came back in single-digit to
   double-digit seconds each. If a call times out, don't retry it as-is -
   shrink it further.

4. **Route "local" through the Ollama CLI directly, not OmniRoute.**
   OmniRoute's `ollama-local` provider had **no active connection pool** -
   confirmed with `mcp__omniroute__omniroute_pool_status` returning `"No
   pool found for provider 'ollama-local'"` - and every model in it timed
   out identically at exactly 30s on a one-word test prompt, which is a
   dead link, not a slow model. The user's own Ollama installation worked
   fine standalone. Use it directly instead:

   ```sh
   ollama list                                        # what's actually pulled
   cat prompt.txt | ollama run <model> --think=false --nowordwrap 2>/dev/null
   #                                    ^^^^^^^^^^^^^  ^^^^^^^^^^^^  stderr
   #                                    carries the spinner; drop it. Both
   #                                    flags are load-bearing - see below.
   ```

   **`--nowordwrap` is not optional either, and is easy to miss because it
   looks cosmetic.** Confirmed live (2026-08-31, building
   `write-test-local.sh`, with `--think=false` already in place):
   `qwen3-coder:30b` still wrote raw ANSI redraw bytes (cursor-back-N +
   clear-to-EOL) straight into stdout whenever a streamed line was long
   enough to soft-wrap on ollama's client-side renderer - this is a
   *different* mechanism from the thinking-preamble leak below, not fixed
   by `--think=false`, and not a TTY-detection bug either: piping through
   `cat` and setting `TERM=dumb` both failed to suppress it, only
   `--nowordwrap` did. Left uncaught it corrupts output silently and
   specifically: the word sitting at the wrap column came back duplicated
   and truncated (`"written"` arrived as `"wri" + escape bytes + "written"`
   in the same line), which reads as plausible text at a glance and will
   pass right through a naive extractor. Confirmed present on
   `qwen3-coder:30b`; not yet re-tested on the models below, so assume any
   of them could do the same on a long enough line and pass `--nowordwrap`
   unconditionally, the same reasoning `--think=false` already gets.

   **`--think=false` is not optional if you need clean, parseable output.**
   Confirmed live (2026-08-31): reasoning-capable models print a full
   `Thinking... ...done thinking.` preamble straight to **stdout** by
   default, plain text, no `<think>` tags to regex out - this isn't limited
   to obviously-named reasoning models. `gemma4:26b` and `qwen3.8:27b` both
   do it despite not having "reasoning" or "r1" in their names; only
   `qwen2.5:14b`, `qwen2.5-coder:32b-instruct-q4_K_M`, and `mistral-nemo`
   were confirmed to never emit it. If you're parsing the response for
   anything structured (JSON, a specific format), that preamble - and any
   `[`/`{` it happens to contain - lands in your output right along with
   the real answer. `--think=false` suppressed it cleanly on every model
   tested, reasoning and non-reasoning alike, so pass it unconditionally
   rather than special-casing by model name.

   **Which local model for which job** (from the same session's research,
   ranked against real coding/instruction-following benchmarks, not
   guessed):
   - **Code generation / fixing**: `qwen3-coder:30b` if pulled (30B MoE,
     3.3B active params - newer and benchmarks higher than qwen2.5-coder at
     a similar ~18-19GB footprint, and the fewer active params should make
     it faster too; needs `--nowordwrap`, see above, not yet re-confirmed
     clean under `--think=false` alone the way the models below were).
     `qwen2.5-coder:32b-instruct-q4_K_M` otherwise (dedicated coder model,
     confirmed never emits the thinking preamble); `qwen2.5:14b` as a
     smaller/faster fallback below that (HumanEval 83.5%, MBPP 82%, never
     reasons).
   - **Review / second opinion**: pick a genuinely different model family
     than whatever did the generation, not just a different size of the
     same one - `gemma4:26b` (strong instruction-following, IFEval 98.5%,
     needs `--think=false`) is the current pick; `mistral-nemo:latest` is
     the safe, always-clean fallback if `--think=false` ever proves
     unreliable on a given Ollama version (weaker at code, HumanEval ~32%,
     but fine for JSON-verdict-style review).
   - **Docs / long-context prose**: `gemma4:26b` (best IFEval/long-context
     fit of what's pulled here; needs `--think=false`).
   - **Avoid for structured-output delegation**: `deepseek-r1:14b` and any
     `DeepSeek-R1-Distill-*` variant - always-on reasoning by design, and
     while Ollama does support disabling it for this family too, there's
     less margin than the above picks if a future Ollama/model update
     changes how that suppression behaves.

   Re-test all of this before trusting it again if OmniRoute's own config
   changes, Ollama updates its thinking-suppression behavior, or the locally
   pulled model set changes - see "Things rot" below.

5. **Route "free tier" through OmniRoute, but test the combo first.**
   Existing combos rot: in one session, `deepseek` had no active
   credentials, `gemini/gemini-2.5-pro` was deprecated server-side, and one
   `ollama-cloud` model id wasn't in the live catalog - all inside a combo
   that looked fine from its listing. Before trusting a combo:

   ```
   mcp__omniroute__omniroute_test_combo  (comboId, a short testPrompt)
   ```

   This reports per-provider success/failure/latency without spending a
   real request. If a combo is polluted with dead entries, either accept
   the latency of falling through them, or build a clean one:

   ```
   mcp__omniroute__omniroute_create_combo
     name: something scoped to the task, e.g. "acid-feature-free-fast"
     models: only the providers that actually responded, fastest first
     strategy: "priority"
   ```

   Then call `mcp__omniroute__omniroute_route_request` with that combo
   name in both `model` and `combo`.

6. **Dispatch local and free in one message when they're independent** -
   two tool calls in the same turn, not sequential turns, so they run
   concurrently.

7. **Review both outputs against each other and against the codebase.**
   Do they agree on the logic? Does either one miss an edge case the other
   caught (e.g. one used `const`, one didn't - trivial; one forgot a
   guard the other included - not trivial)? Rewrite the accepted version's
   comments to match the file's actual voice before it goes in - don't
   paste a delegate model's comment style into a codebase with a
   deliberate, consistent one.

8. **Apply by hand, then verify for real.** For this project:

   ```sh
   ./launcher/test/run_tests.sh   # host suite, portable, <1s - run this first
   ```

   Then a real target build (see `docs/Testing-Guide.md` for the Windows/
   PowerShell specifics `idf.py` needs). Both, not one - a change can
   compile clean and still fail a behavioral assertion, and it can pass
   every host test and still not compile for the target if it touches
   something host tests don't exercise.

9. **Treat a test failure as the process working, not failing.** In the
   acid-evaporation feature, the chosen "low chance" constant sounded
   reasonable in isolation and was still roughly 100x too aggressive once
   checked against how many cells a real pool has and how many steps a
   real scene runs - the host suite caught it immediately (mass-accounting
   test failed by 15x). That's not a delegate-model mistake to blame on
   the model; it's exactly the kind of miscalibration a fast, deterministic
   test loop exists to catch before it reaches a device. Fix it, re-run,
   and consider whether the fix needs its own regression test (it did,
   here - see `test_acid_evaporates_into_gas_when_forced` in
   `suite_sand.c`).

10. **Commit only when asked. Push/merge only when asked**, and even then,
    check the git mechanics below before assuming a plain `git merge` or
    `git push` does what you think in a worktree.

---

## The failure mode this section used to allow

Recorded 2026-08-31, from the sand dirt/heat-flaw feature (dirt smelting
into metal-or-stone with a rolling-modulo clump, plus a wet-dirt spoil
path). The user asked for this workflow by name, watched the session run,
and reported back: their machine's CPU/GPU usage never moved. They were
right to call it out - the diff had four new `reaction_t` fields plus
comments, a `material.c` tuning block, new `sand_t` state, the actual
`try_heat_transform()` logic, a `dump_reactions.c` doc-generator update,
and roughly ten test edits. Of all of that, exactly one function tail was
sent to a local model - and even that call was used as a *cross-check* on
an answer already worked out by hand, not as the source of the diff that
got applied. Every doc comment, every struct field, every test body was
typed directly.

**What went wrong, specifically:** step 1 of this doc ("investigate and
plan first, entirely yourself") was read as license to also *implement*
everything the investigation touched, on the theory that dense, comment-
heavy files like this one's need someone tracking every existing
invariant while writing each line. That reasoning is true of the *design*
- which invariant a change has to respect, which existing field to reuse,
what the new comment has to say to stay accurate - and false of the
*typing*. A model handed the exact surrounding comment style, the exact
field it is documenting, and a one-paragraph statement of what to say does
not need to independently discover the invariant; it needs to be told it,
the same way the delegation prompt for a logic diff already tells the
model exactly what to change and where.

**The fix is the "atomic edit list" now required in step 3 above**, plus
the self-check at the end of "The division of labor": before writing a
single line of the implementation, list every self-contained edit the
plan implies, and default every line of that list to a delegated call. Do
not stop at the trickiest piece of logic and call the rest "context-
dependent" - a struct field's doc comment is not context-dependent just
because you personally already know what it should say. If a whole
category of edit (struct fields + comments, doc paragraphs, test bodies)
never went through a model this session, that is the signal this section
exists to catch.

---

## Git mechanics specific to worktrees

If you're running in a `.claude/worktrees/...` checkout (this project uses
one worktree per branch), `main` is very likely checked out somewhere else
entirely - the user's primary checkout. That has real consequences:

- **You cannot update a branch ref that's checked out in another
  worktree**, even locally, even a plain fast-forward. `git fetch . src:main`
  or `git push . src:main` both fail with `refusing to fetch into branch
  'refs/heads/main' checked out at '<path>'`. This is a real git safety
  feature, not a bug to work around by force.
- **Pushing to the actual remote is unaffected by that restriction** -
  `git push origin <local-branch>:main` updates `origin/main` directly and
  doesn't touch the other worktree's checked-out ref at all. That's the
  correct way to land work on `main` from here.
- After pushing to the remote this way, the user's primary checkout is now
  behind `origin/main` until they fast-forward it themselves:
  `git -C <primary-checkout-path> pull --ff-only`. Tell them so; don't try
  to do it for them from a worktree that can't touch that ref.
- **Landing more than one feature branch**: push the first one straight to
  `origin/main` (fast-forward, since it branched from the same point), then
  `git merge origin/main` into the *next* branch to bring it up to date and
  surface any conflict between the two features before it reaches `main` -
  resolve, re-verify (build + tests), then push that one too. Don't try to
  merge sibling feature branches into each other directly; integrate each
  through `main` in turn.

---

## Things rot

Everything provider-specific in this doc - which OmniRoute combo is clean,
whether `ollama-local`'s pool is back, which cloud model ids are still
live - is a snapshot from one session, not a promise. Re-verify with
`omniroute_test_combo` or a trivial `ollama run` before trusting any of it
again. If something here turns out to be stale, fix this doc in the same
change rather than silently working around it - that's the whole point of
it being a living document instead of a one-off spec.

## Related, narrower tooling

`scripts/fix-audited-code.sh` and `scripts/fix-audited-docs.sh` (each has
its own header comment) already do a more automated version of this for one
specific, high-volume case: bulk-fixing MISRA/cppcheck findings, or doc
audit findings, as exact find/replace patches. The code-side script has a
mandatory review-model gate and its own `--pool local|free|subscription|all`
selection; the docs-side script's review model is optional. Both also take
`--local` (routes every model call through the Ollama CLI directly, per
step 4 above - including `--think=false`, already handled for you) and
`--no-push` (commit without pushing). Reach for whichever one applies when
the task really is "fix N audit findings across files," not an open-ended
feature - both have already worked out the patch-verification, the
local/free routing, and (for code) review-loop machinery this doc doesn't
need to repeat.

Single-click launchers wrap both, so you don't need to remember the flags:
`fix-audited-code-free.sh` / `fix-audited-docs-free.sh` (free-tier cloud,
no confirmation prompt), `-local.sh` variants of each (same, but `--local`
instead - zero cloud calls), and `-choose-app.sh` variants of each
(interactive: lists real scoping targets - apps under `launcher/main/apps/`
for code, `docs/<Name>/` folders that actually exist for docs - and prompts
for a number or name before running free-tier). `fix-audited-code-free.sh`
and `-local.sh` also take `--project` to widen from the apps/sand default to
the whole project; the docs-side `-free.sh`/`-local.sh` take `--app <name>`
to narrow from the all-docs default to one app's own doc folder.

`scripts/resolve-conflicts-local.sh` is the same idea applied to git merge
conflicts instead of audit findings: one hunk, one fixer call
(`qwen2.5-coder:32b-instruct-q4_K_M`) plus one reviewer call (`gemma4:26b`,
a different model family - a real second opinion, not the fixer checking
its own work), looped up to `--rounds` times on an INVALID verdict. The
part that makes this safe to actually delegate is the hard gate after: it
only ever commits if `./launcher/test/run_tests.sh` AND
`check_app_sources.sh` both pass on the resolved tree, and it never forces
a hunk the reviewer never approved - that hunk is left with its real
`<<<<<<<`/`=======`/`>>>>>>>` markers in place instead, so a partial
success still shows up as a normal, honest merge conflict rather than a
silently-wrong commit. Takes `--target <branch>` (default `main`), `--
worktree` (isolated, then pushes straight to `origin/<your-branch>` per
the git mechanic below), and `--no-push`. See the script's own header
comment for the full design rationale - it was worked out and tested
end-to-end (real Ollama calls, a synthetic conflict, and both the
test-gate-red and reviewer-never-approves failure paths, each verified to
refuse the commit) in the session that added it.

`scripts/write-test-local.sh` applies the same shape to writing ONE Unity
test body, following this doc's own division of labor to the letter: YOU
write the spec (a plain-text file naming the suite, the exact scene-setup
statements, and the exact `TEST_ASSERT_*` calls - the judgment part stays
yours), and `qwen3-coder:30b` (reviewed by `gemma4:26b`) only renders that
into a correctly-formatted function matching the target suite's own
examples. The gate: the suite must not already have a test of that name,
`run_tests.sh` must still pass afterward, and the new test must show up as
PASS *exactly once* - CLAUDE.md's "watch it fail before it passes" turned
into an automatic wiring check. `--regression-commit <SHA>` goes further
and actually proves the test can fail, by inserting the same generated
function into the tree as it stood at `<SHA>^` in an isolated worktree and
checking it does NOT pass there - a WARNING, not a hard abort, since a
test that also passes on the pre-fix code may simply be new coverage
rather than a regression guard, which is a legitimate thing for it to be.

Built in the same session as resolve-conflicts-local.sh, immediately after
pulling `qwen3-coder:30b`, and every one of the following was a REAL bug
caught by actually running the tool against this repo's real files, not
assumed away:
- `qwen3-coder:30b` writes raw ANSI word-wrap-redraw bytes into stdout
  even with `--think=false` already set - `--nowordwrap` is required for
  it specifically (see this doc's own `--nowordwrap` entry above).
- The same model sometimes writes the bare function name as its own title
  line before the real declaration - fixed by mechanically stripping an
  exact-match leading line rather than trying to prompt it away, since a
  mechanical strip is a guarantee and re-prompting is only ever a
  reduction in frequency.
- The naive "insert after the last `RUN_TEST(...)` in the function" landed
  a portable test's wiring inside an `#ifdef DEVICE_BUILD` guard, where a
  host build silently compiles the RUN_TEST call back out - the only
  symptom was `-Werror=unused-function`, not a test failure. Fixed by
  tracking `#if`/`#ifdef`/`#endif` depth and only considering unconditional
  `RUN_TEST` lines.
- Formatting the whole suite file with `scripts/check-format.sh` (as
  fix-audited-code.sh does) rewrote ~18000 unrelated lines of a real
  file's house style for the sake of one new function - exactly what
  CLAUDE.md's own formatting section warns against. Fixed by having the
  insertion logic report the exact 1-based line ranges it touched, then
  calling `clang-format -i --lines=N:M` (repeatable per range) instead of
  formatting the file whole.
- The regression-commit proof's first version copied the CURRENT
  (already-modified) suite file into the pre-fix worktree before
  re-inserting - creating a duplicate definition, a compile error, and an
  exit-code-only check that misreported the resulting failure as
  "confirmed fails as expected". Fixed by using the worktree's own,
  correctly-checked-out pre-fix file untouched, and by checking for the
  test's specific PASS/FAIL line rather than the suite's overall exit
  code, with an INCONCLUSIVE result (not a false "confirmed") when neither
  line appears at all.
