# UniiChat review — 2026-10-09

Repo: /home/isolino/.t3/projects/echat (branch main, clean at 8d176e3)
Spec: /home/isolino/.t3/userdata/attachments/5139bd2d-3048-4923-a419-08b206911fce-fbbe75ef-ac9e-4601-b226-8bb5e3701411-md.md ("optchat.md")

## Evidence gathered

- `nix-shell --run 'make test'`: all offline suites pass.
- `nix-shell --run 'make test-pi'`: FAILS TO COMPILE (see A5). With
  `CFLAGS=... -Wno-unused-result` every pi suite passes (live turns, 31 real
  worker compactions, failures, steering, native shell). The live tests already
  drive pi through `--mode rpc` with `prompt`/`steer`, i.e. the path an iOS
  client would use.
- Synthetic chat, 131,072 messages (140 MB main log, 115 MB tree log, full
  tree): `unii status` takes 4.1 s and peaks at 137 MB RSS. Node summaries are
  all held in RAM; extrapolated to 1M messages: ~30 s startup, ~1 GB RSS.
- `unii view /tmp/typo` creates a brand-new chat; `unii status DIR` takes the
  writer lock and rewrites `.state` (so it cannot run while pi is running).

## What is already right (keep it)

Strict JSON/UTF-8 with NUL rejection; records re-validated on every read
("indexed message is unreadable or changed"); append + fsync + dir fsync;
atomic renames; per-process lock registry; exact rational `due` comparison
without overflow; rollback-push equivalence test through 20,000 steps; ready
queues instead of tree scans; min-heap for blocked jobs with a correct
monotonicity argument; length-correction loop exactly as specified; the turn
boundary logic for steering. The overall shape (C owns memory, JS adapts pi)
is sound.

---------------------------------------------------------------------------

## A. Correctness and robustness bugs

A1. Oversized summaries can permanently stall all compaction.
    src/main.c:509 keeps the shortest answer after 5 attempts with no upper
    bound. A node larger than COMPACT_HIGH (32,000) makes every later job's
    context (`view_render_limit(..., 32000)`) return NULL, so every later job
    is deferred; its own parent's context includes it, so it can never be
    merged away. No new leaf is built again, every turn errors with
    "compaction blocked or failed". Smaller oversized lines (2–16 KB) silently
    crowd the 16–32 KB compaction view and the turn view (spec mistake #5).
    Fix: hard ceiling. After the last attempt, if the shortest answer exceeds
    NODE_BYTES + slack (slack = 64 bytes; "a few bytes over"), cut it at a
    UTF-8 boundary to NODE_BYTES. Same ceiling for any path that builds a node.

A2. One bad clock reading can stop all writes for good.
    src/store.c:753-771 and :950 refuse to write if today's file name sorts
    before the last file. If the clock jumps forward once (one message lands in
    2099-01-01.jsonl), every later write fails until 2099. A backward step of a
    day (VM restore, NTP) also halts the chat.
    Fix: write to max(last file day, today); the record's `date` stays the real
    clock time. Relax validation (`date_valid` / `add_message`) from
    "record day == file day" to "record day <= file day". File names stay
    monotone, so load order still equals append order. Same rule for tree/.

A3. A torn final record stops the chat until manual repair.
    A power loss during an append leaves a final line without LF;
    load_logs (src/store.c:385-391) refuses to open. That write was never
    acknowledged (fsync never returned), so dropping it loses nothing that was
    promised. Fix: on a writer open, if the LAST file of main/ or tree/ ends in
    a partial line, append those bytes to `<file>.torn` (outside the `.jsonl`
    pattern), fsync it, truncate the log to the last LF, fsync, and continue;
    report it on stderr. Corruption anywhere else (complete but invalid lines,
    partial lines in earlier files) must still refuse to open.

A4. Read-only commands mutate storage and take the writer lock.
    src/main.c:701 opens every CLI command with `store_open`, which mkdirs,
    locks, and writes `.state` (src/store.c:703) and can write a crash tail.
    Fix: a read-only open (see B3) used by `view`, `zoom`, `date`, `status`,
    `log`: never creates, locks or writes; works while pi holds the lock.

A5. `make test-pi` does not build under the documented toolchain.
    tests/pi_{live,worker,failure,steering,bash}_test.c ignore `write()`'s
    result; nix's gcc has `warn_unused_result` + `-Werror`. A `(void)` cast
    does not silence GCC; assign to a variable.

A6. Input is silently dropped when the backend is down.
    pi/unii.ts:215 returns `{action:"handled"}` with no notice: the message is
    neither saved nor answered. Notify (and over RPC this becomes a visible
    `notify`). Pair with B6 (`/unii restart`).

A7. Timeouts use the wall clock. src/main.c:492 `time(NULL) - started > 180`.
    Use `clock_gettime(CLOCK_MONOTONIC)`. Make the timeout a `serve` option
    (`--timeout SECONDS`, default 300: xhigh-effort calls can exceed 180 s);
    tests can then use short timeouts.

A8. fsync is not a durability barrier on macOS (the computer an iOS user will
    likely run pi on). Route every fsync through one `sync_fd()` that uses
    `fcntl(fd, F_FULLFSYNC)` when `F_FULLFSYNC` is defined, falling back to
    fsync. Keep the fsync fault-injection test working.

A9. Worker processes receive the whole system prompt (with the user's personal
    instructions) in argv and environ (src/pi.c:110, :129): readable by other
    local users through `ps`/`/proc`, and Linux caps one argument at 128 KiB.
    Resolved by B1.

## B. Extensibility: other hosts and the iOS app

B1. Make the engine host-agnostic: the host performs model calls.
    Today the C engine hard-wires model access by spawning up to eight extra
    `pi --mode rpc` processes (src/pi.c, pi/bridge.mjs), parsing pi's private
    event schema, passing prompts through argv/env, and juggling fd 3. That
    couples the engine to one pi version and makes it unusable by any other
    host. Pi offers in-process calls: `ctx.modelRegistry.find(provider,id)`,
    `ctx.modelRegistry.hasConfiguredAuth(model)`,
    `ctx.modelRegistry.complete(model, {systemPrompt, messages, tools},
    {reasoningEffort, signal, sessionId, ...})` and `streamSimple`
    (see pi docs/extensions.md "Context and session changes",
    examples/extensions/{handoff,custom-compaction,summarize}.ts under
    /nix/store/6bnb2h1llhj0vy86x8f7krmkpwf8acrd-pi-1.1.0/libexec/pi/).
    Plain completion calls never execute tools, which is strictly safer than
    today's tool-blocking guard.
    Design (the C keeps ALL policy: what, when, context, ruler, corrections,
    shortest-of-5, ceiling, timeouts, retries, concurrency, cache gating):
      engine -> host  {"event":"call","call":N,"messages":[
                         {"role":"user","text":VIEW+"\n\n"+TASK,"images":[...]},
                         {"role":"assistant","text":PREVIOUS},      (corrections)
                         {"role":"user","text":CORRECTION}]}
      host -> engine  {"op":"result","call":N,"text":"..."}
                   or {"op":"result","call":N,"error":"..."}
      engine -> host  {"event":"cancel","call":N}     (timeout; host aborts)
    The host supplies the system prompt and tools: they are the turn's own,
    per the spec, and the host already owns them. So the engine no longer needs
    the system prompt; `configure` shrinks to events/options (keep accepting
    old fields for compatibility, or replace with `hello`, see B2).
    Unify cache gating: compaction calls use the same `cache_wait` /
    `cache_started` ops as the turn (add an optional `call` field); the engine
    keeps one set of keys whose first request has started without a response.
    Remove src/pi.c, src/pi.h, pi/bridge.mjs, tests/pi_test.c, fake_pi, and
    the Records-over-fd-3 path; rewrite server_test to play the host
    (deterministic results, over-long answers, failures, timeouts).
    In unii.ts, guard `before_provider_request` (and any other turn hooks) so
    direct compaction calls do not disturb turn state, if pi fires those
    events for direct calls; verify against pi 1.1.0's behavior.
    Keep the defaults (provider openai-codex, model gpt-6-luna, effort xhigh)
    and the existing flags/env vars.

B2. A documented, versioned engine protocol. Add PROTOCOL.md: framing (JSONL,
    split on LF only), every op and event with fields and errors, the
    `ok`/`fatal` semantics, ordering guarantees, and a `hello` op that returns
    `{"protocol":1,"messages":N,...}` and enables events. Document the on-disk
    format as a stable reader contract too (append-only daily JSONL, atomic
    view.json, the read order in B3). The files are the API for readers, the
    way SQLite's file format is.

B3. Lock-free readers and `unii log`.
    Writers only append whole lines and rename views atomically, so readers
    need no lock: read view.json FIRST, then main/, then tree/ (anything the
    view names already exists), and treat a partial final line as
    "not written yet" (EOF), not as corruption. Add
      `unii log DIR FROM [COUNT]`  raw JSONL records for ids [FROM, FROM+COUNT)
    which must not need the full index: read the first record of each daily
    file to find the file holding FROM, then stream. A phone relay can page
    scrollback with it while pi runs. `unii status DIR` must also be
    lock-free and cheap enough to poll.

B4. History and live events in the engine protocol.
    - `{"op":"messages","from":N,"count":K,"images":false}` -> records with
      i, kind, date, size, text, has_images (images only when asked), bounded
      by a byte budget with a `next` cursor.
    - With events enabled: `{"event":"append","first":N,"count":K,"kind":...}`,
      `{"event":"built","l":L,"i":I}`, `{"event":"progress","waiting":P}`
      while a turn waits for memory (on change only), and the existing
      `error`.
    - unii.ts shows progress with `ctx.ui.setStatus("unii", ...)` during the
      wait and clears it after (RPC clients receive setStatus records), so the
      wait is never silent.

B5. Retry without inventing a user message.
    Spec says a failed call retries at the next message; on a phone, waiting
    for the user to type is the wrong trigger and a "retry" message pollutes
    memory. Keep the new-message retry AND add (a) automatic retries with
    capped backoff (15 s, 60 s, 300 s, then wait for a message or (b)) using
    the monotonic clock and the poll timeout, and (b) a `retry` op plus
    `/unii retry`.

B6. `/unii restart`: relaunch a dead backend without restarting pi (the
    launch path already exists; reset the extension state as on shutdown).

## C. Spec assumptions worth changing

C1. Prompt caches are per model. "Tools and system prompt ... cached across
    all calls, turns and compactions alike" (spec §3.3) and "it reads them
    from the turns' cache entry" (§4) hold only if compactions use the turn's
    model. With the spec's Haiku or the implementation's GPT-6 Luna, turns and
    compactions never share an entry, and today's turn<->worker cache gate can
    never trigger. Compactions still share among themselves, which is the
    real win. No code removal needed once B1 unifies gating; document it in
    README, and note the implementation's GPT-6 Luna default differs from the
    spec's Haiku.

C2. The turn waits on summaries. Spec §6 says "the wait takes seconds", but
    the previous reply is usually >512 bytes, so the next turn waits for an
    xhigh-effort call (often tens of seconds), and any failed summary blocks
    the turn. Keep the spec's invariants (the saved view holds only summaries,
    never a whole message, never half a message, cache prefix stable), and
    send the not-yet-summarized tail VERBATIM AFTER the view as transient turn
    input:
      view rendered up to the first unbuilt leaf (cut <= before), then
      <recent>
      [message ID kind=KIND]
      full text (newlines kept; "(has images: zoom(ID,1))" if any)
      ...
      </recent>
    only when the whole tail fits in TAIL_BYTES (32,000); otherwise wait
    exactly as today. Return it from `turn` as a separate `recent` field; the
    extension places it after the chat blocks and before the date/cwd state.
    Update prompt.txt's placeholder sentence to say unsummarized messages
    follow the view whole in <recent>. Setting TAIL_BYTES to 0 must restore
    the spec's behavior exactly, so the change is easy to undo.

C3. Retrying only at the next message: see B5.

C4. Zoom results are logged again as `echo` messages, images included
    (unii.ts message_end -> append echo). Each zoom(id,1) copies up to 30 KB
    (and every attached image's base64) into the permanent log and pays a
    compaction call to summarize text the tree already summarizes. Log
    successful zoom results by reference instead: a short echo such as
    `zoom {"id":5,"n":1,"page":0}: message 5 page 0/1, 8123 bytes, not
    copied; zoom again to read` (and for n>1, the two child line heads), with
    no images. Keep it under 512 bytes so it is a free fast-path node. Keep
    error results and `date` results verbatim.

C5. Long turns miss the whole view cache. With 4-line blocks and Anthropic's
    ~20-block lookback, a turn that adds more than ~80 lines (about 40 tool
    calls) leaves the next turn's view mark too far from the previous entry:
    the full 64–128 KB view is rewritten at 1.25x. Use the 4th breakpoint
    (limit is 4: system, previous-boundary, last whole block, request end):
    remember the previous turn's last whole chat block (index and text) and,
    if the same block is still identical in the new request, mark it too.

C6. Endless chat, unbounded RAM: every node's text stays in memory
    (src/store.c add_node / store_build). Keep only `{file, offset, size,
    flags}` per node and read text on demand (render/zoom/merge sources
    re-validate records, like messages). Shrink Message too: intern daily file
    names (index instead of a strdup'd path per message), kind as a small
    enum, date inline. Speed up startup: `read_line` reads one byte at a time
    through `fgetc`+`buf_putc`; use `getc` or a block reader with memchr.
    Re-measure with the synthetic chat (generator: /tmp/unii-scale/, see
    "Evidence"; regenerate if missing).
    Measured on the 140 MB main log: read_line alone 1.02 s (137 MB/s); a
    block read + LF split 0.07 s; json_parse ~0.8 s (~170 MB/s). So the
    reader is the first fix. In json `string()`, copy runs of plain bytes in
    one buf_add instead of one call per code point (UTF-8 still validated).
    If startup is still over ~1 s per 100k messages after that, add an index
    checkpoint (derived, validated, rebuildable file of offsets/sizes/kinds
    up to id N) so startup parses only records after N.

C7. `!command` is logged as plain user words (unii.ts:172), so the summarizer
    reads `user: ls -la` as chat. Log it as typed: `!ls -la`.

C8. (Recommendation only; do not implement unless trivial.) `.state` exists to
    journal one bit next to view.json, and `generation` is dead state
    (only compared within update_views). A single atomic state file would be
    simpler, but it changes the spec's documented view.json and needs a
    migration; leave it, just drop `generation` from the logic if harmless
    and keep reading/writing the field for compatibility.

## D. C style (the Bellard / Lua / Pall / Hipp bar)

D1. Mixed return conventions: store returns 1/0, everything else 0/-1.
    Convert the store API to 0/-1 while refactoring it (C6/B3), carefully.
D2. `Node.queued` uses magic 0/1/2/3: use an enum.
D3. Duplication: engine.c save_compact/load_compact re-implement store.c's
    atomic_file/parse_view/dump_view (and lose errno and `broken`); store.c
    zoom_line duplicates view.c line; store_zoom walks pages twice; main.c
    hand-builds `{"id":...}` responses in four places. Share one of each.
D4. Thresholds: `128000` is hard-coded in store.c next to VIEW_HIGH in
    engine.h. One header owns the spec constants.
D5. `strcpy(e->store->error, ...)` bypasses fail()/io_fail() and drops errno.
D6. `signal()` -> `sigaction()`; restore stdin's flags (O_NONBLOCK) on exit.
D7. buf_add's alias check handles only `s == b->s`: document "s must not
    point into b" and drop it, or handle interior pointers. xstrdup's
    `n == (size_t)-1` check is dead.
D8. Dead product code: pi_steer, pi_abort, the log/display callbacks (only
    tests use them). Goes away with B1.
D9. I/O cost: every append fsyncs the directory even when the file exists
    (only needed on creation), then saves `.state`, `view.json` and
    `compaction-view.json`: ~8 fsyncs per message, plus reading `.state`
    back. Fsync the directory only on file creation; skip a compaction-view
    save when nothing changed (engine_sync can save twice). Optional: an
    `append` with several records in one request (one log write, one fsync,
    one view save) used for an assistant message's blocks.
D10. Leftover `.save-XXXXXX` temp files from crashes are never removed: clean
     them on writer open.
D11. Makefile: object rules list incomplete header dependencies (e.g.
     src/main.o, src/view.o through store.h/base.h). Keep `.POSIX`.

## E. Vendoring: decided against (do not vendor anything)

- JSON: base.c's parser is ~330 lines and enforces what the format relies
  on: duplicate-key rejection, NUL/\u0000 rejection, raw number text kept for
  byte-exact re-emission, overflow-safe unsigned reads, depth limit. The best
  candidate at our bar, yyjson (MIT, ANSI C89, strict UTF-8, ~1.8 GB/s), is
  ~20,000 lines, allows duplicate keys and \u0000 (so our checks would stay),
  and the parser is not the bottleneck (see C6).
- Unicode: 30,000-byte splits and echo clipping cut at code points, so an
  emoji ZWJ sequence or combining mark can straddle two messages. Cosmetic;
  not worth grapheme tables. Leave it.
- SQLite would fix readers-while-writing, indexing, RAM, torn writes and
  multi-file atomicity at once, but it replaces the spec's plain-text,
  append-only log (the artifact meant to outlive everything) with ~250k
  vendored lines needing non-C89 flags. Revisit only if that requirement
  goes away.
- If anything is ever vendored: vendor/ directory, pinned version and
  checksum, license file, no local edits (wrap it), its own compile flags so
  our code stays under -std=c89 -pedantic -Werror, tests against the wrapper.

---------------------------------------------------------------------------

## Implementation brief

Constraints
- C stays ISO C89 + POSIX.1-2008 (`-std=c89 -pedantic -Wall -Wextra -Werror`,
  `_POSIX_C_SOURCE=200809L`), no dependencies, no `//` comments,
  declarations at block start. Match the existing style (4-space indent,
  short names, sparse comments that state invariants).
- Keep the JS a thin adapter; all memory policy stays in C.
- Existing chat directories must open unchanged: on-disk changes are
  additive only (A2 relaxes validation; A3 adds `.torn` side files).
- Do NOT commit. Leave changes in the working tree.
- Every fix gets a test. Both suites must pass at the end:
  `nix-shell --run 'make clean && make test'` and
  `nix-shell --run 'make test-pi'` (pi 1.1.0 is installed; tests use a
  deterministic offline provider). Also `nix-build` if time allows.
- Update README.md and add PROTOCOL.md. In README, list every deliberate
  divergence from the spec (C1 note, C2, C4, C7, A2, A3, B5).

Priority (do them in this order; if something must be deferred, say why)
 1. A1, A2, A3, A5, A6, A7, A8           (bugs)
 2. B1 (+A9, D8), B2                     (host-agnostic engine, protocol)
 3. B3/A4, B4, B5, B6                     (readers, history, events, retry)
 4. C2, C4, C5, C7                        (spec changes)
 5. C6, D9                                (scalability/IO)
 6. D1–D7, D10, D11                       (style)

Final report: for every ID above, done / partly / deferred (with reason),
the files touched, and the test that covers it; the exact commands run and
their outcomes; anything you found that this review missed.

## Appendix: reproducing the scale measurement

    mkdir -p /tmp/unii-scale && cd /tmp/unii-scale && rm -rf chat
    mkdir -p chat/main chat/tree && N=131072
    awk -v N=$N 'BEGIN{t=""; for(k=0;k<990;k++) t=t "x"; for(i=0;i<N;i++)
      printf "{\"i\":%d,\"kind\":\"user\",\"text\":\"%s\",\"size\":%d,\"date\":\"2026-01-01T00:00:00Z\"}\n",
      i, t, length(t)}' > chat/main/2026-01-01.jsonl
    awk -v N=$N 'BEGIN{t=""; for(k=0;k<400;k++) t=t "s"; n=N; l=0;
      while(n>=1){ for(i=0;i<n;i++) printf "{\"l\":%d,\"i\":%d,\"text\":\"%s\",\"size\":%d}\n",
      l, i, t, length(t); n=n/2; l++; if (n<1) break }}' > chat/tree/2026-01-01.jsonl
    echo '[[17,0]]' > chat/view.json
    # then time `./unii status /tmp/unii-scale/chat` and sample VmHWM from
    # /proc/<pid>/status. Baseline at 8d176e3: 4.1 s, 137 MB peak RSS.

## Appendix: how this should plug into the iOS app (target shape)

    iPhone app ──(authenticated transport: SSH channel / WebSocket over a
                  tailnet; a small relay on the computer, out of scope)──►
      pi --mode rpc --no-session -e <uniichat>/pi/unii.ts    (live turns)
        └─ unii serve CHAT        (memory engine; host performs model calls)
      unii log CHAT FROM COUNT / unii status CHAT      (lock-free scrollback)

- Live interaction is plain pi RPC: `prompt` (with images), `prompt` +
  `streamingBehavior:"steer"` during a run, `abort`, the event stream
  (`message_update` deltas, `tool_execution_*`, `agent_settled`), and
  extension UI records (`notify` for UniiChat failures, `setStatus` for
  memory progress). `/unii status|flush|retry|restart` are slash commands sent
  as prompts.
- Scrollback and sync: messages have permanent ids; the app caches by id and
  pages with `unii log` through the relay, safe while pi runs.
- Background agents report back as `work` messages through the engine's
  `append` op.
- Any other host (not pi) can embed the engine by speaking PROTOCOL.md and
  answering `call` events with model completions.
