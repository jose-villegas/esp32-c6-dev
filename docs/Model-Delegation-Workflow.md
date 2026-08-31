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
to stop someone re-treading a dead end), and noticing constraints a model
without that context has no way to know about (a hot-path budget, a struct
size ceiling, an existing invariant a test already pins).

**The delegate models** do small, mechanical, fill-in-the-blank text
generation: "here is the exact current code, here is exactly what changes,
give me the replacement block." Not "implement feature X" - that's still
your job, just handed out one precise snippet at a time.

**You again** review what comes back, reconcile it with what you already
know, apply it by hand (rewriting comments to match house voice - delegate
output tends to be terser and in a different register), and verify with a
real build and the real test suite. A model's confident-sounding output is
not evidence; the test suite passing is.

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

3. **Keep each delegated ask small.** One field, one function, one
   struct-literal block per call - not a five-file feature spec in one
   prompt. This isn't just cleanliness: large prompts to local/free
   endpoints reliably **timed out** in practice, while the same content
   split into three small asks (one per file) came back in single-digit
   to double-digit seconds each. If a call times out, don't retry it as-is
   - shrink it.

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
selection; the docs-side script's review model is optional. Reach for
whichever one applies when the task really is "fix N audit findings across
files," not an open-ended feature - both have already worked out the
patch-verification and (for code) review-loop machinery this doc doesn't
need to repeat. `scripts/fix-audited-code-free.sh` and
`scripts/fix-audited-docs-free.sh` are no-confirmation, free-tier-model,
isolated-worktree launchers for each, respectively.
