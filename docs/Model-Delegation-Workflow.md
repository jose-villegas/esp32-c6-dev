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
   ollama list                              # what's actually pulled
   cat prompt.txt | ollama run <model> 2>/dev/null   # stderr carries the
                                                       # spinner; drop it
   ```

   Re-test this assumption before trusting it again if OmniRoute's own
   config changes - see "Things rot" below.

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

`scripts/fix-audited-code.sh` (and its docs/ own header comment) already
does a more automated version of this for one specific, high-volume case:
bulk-fixing MISRA/cppcheck findings as exact find/replace patches, with a
mandatory review-model gate and its own `--pool local|free|subscription|all`
selection. Reach for that script when the task really is "fix N audit
findings across files," not an open-ended feature - it has already worked
out the patch-verification and review-loop machinery this doc doesn't need
to repeat.
