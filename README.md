# UniiChat

An endless chat for [pi](https://github.com/earendil-works/pi). The chat is its
memory: an append-only log, a binary summary tree, and a saved view with fine
recent detail and coarse old detail.

The memory engine is dependency-free C89 using POSIX. It owns every memory
decision: what is logged, which node is built when, the context and task each
summary gets, length corrections, retries, timeouts and cache gating. A
**host** performs the model calls it asks for. The pi extension
(`pi/unii.ts`) is that host: a thin adapter that connects pi's hooks, tools
and model registry to the engine. Pi supplies the UI, authentication,
providers, streaming, and ordinary coding tools. Any other program can host
the engine by speaking [PROTOCOL.md](PROTOCOL.md).

## Build and run

On NixOS, from this directory:

```sh
nix-shell --run 'make test'
pi --no-session -e ./pi/unii.ts
```

On another POSIX system with a C compiler and make:

```sh
make
make test
pi --no-session -e ./pi/unii.ts
```

Pi must already have a working model/account. The turn uses whichever model
you select in pi. Background summaries use GPT-6 Luna (`gpt-6-luna`) through
`openai-codex` at `xhigh` effort, called in the same pi process through its
model registry with pi's credentials, independently of the turn model. To
choose the summary provider/model explicitly:

```sh
pi --no-session -e ./pi/unii.ts \
  --unii-dir "$HOME/.local/state/uniichat" \
  --unii-provider openai-codex --unii-model gpt-6-luna --unii-effort xhigh
```

(`UNII_PROVIDER`, `UNII_MODEL`, `UNII_EFFORT` and `UNII_TIMEOUT` set the same
from the environment, for pi and `unii serve` alike; flags win.) The default
chat directory is `$XDG_STATE_HOME/uniichat`, or `~/.local/state/uniichat`. It
is shared across projects, so the chat follows you. Use `--unii-dir` for a
separate chat. One running pi process owns each chat; a second writer is
refused. Readers never need the lock (see below).

To register the built repository as a local pi package:

```sh
pi install . -l
pi --no-session
```

The extension can also be loaded by its absolute path from another directory.
`UNII_BIN` or `--unii-bin` selects a separately installed C executable.
`nix-build` builds and tests an installable package; its extension is at
`result/share/unii/pi/unii.ts` and its binary is `result/bin/unii`.

Use `--no-session` so pi does not write a second session file containing
thoughts. UniiChat itself never logs thoughts. Each provider request starts
with the saved view and only the current turn's messages; pi's UI can keep
showing earlier turns. Messages arriving during memory preparation or a reply
are logged as steering and remain in that turn. Its view stays at the
boundary before the initiating message.

Pi's included `!command` and RPC bash executions are also part of memory: the
command is saved as typed (`!ls -la`) before execution, followed by its
clipped output and exit status. Pi's explicit `!!command` /
`excludeFromContext` option keeps that execution outside UniiChat memory.
Included shell commands stop when memory cannot be saved.

## Instructions and commands

The shared prompt is [pi/prompt.txt](pi/prompt.txt) (the spec's prompt). It is
built into the binary, so it always matches the engine: run `make` after
editing it. `unii prompt` prints it, with the one sentence that spec change C2
rewords.
Pi's project instructions and custom prompt sections are captured at startup
and appended to it. Date and working directory are sent after the view.
`UNII_INSTRUCTIONS` can name a UTF-8 file with additional personal
instructions. Restart pi when changing these instructions so the prompt stays
constant during a run.

- `/unii`: message count, summarized prefix, view size, summary model, calls
  in flight, failed and blocked summaries.
- `/unii flush`: finish queued summaries; report any failed work. A message
  sent while it waits ends the flush with a notice and starts its turn.
- `/unii retry`: retry failed summaries now, without sending a message.
- `/unii restart`: relaunch the engine without restarting pi (after it
  stopped; the log is untouched).
- `/unii import /path/to/file`: append a verbatim `note` to memory.
- `zoom(id,n,page?)`: open an aligned power-of-two node into its children, or
  read a whole message at `n=1`. Pages are numbered from zero.
- `date(id)`: retrieve the message's UTC date and time.

While a turn waits for summaries, the status line shows the progress
("summarizing 3 earlier messages"); RPC clients receive it as `setStatus`.
Input that cannot be saved is never dropped silently: pi shows a notice (an
RPC `notify`).

You can inspect or import memory from the shell:

```sh
printf 'Personal instructions or earlier memories.\n' | ./unii add CHAT note
./unii view CHAT
./unii zoom CHAT 0 1
./unii date CHAT 0
./unii status CHAT
./unii log CHAT 100 50      # raw records 100..149
```

`view`, `zoom`, `date`, `status` and `log` never create, lock or write a chat
and work while pi runs it; `add` takes the writer lock. `unii log` does not
build the index, so it is the cheap way to page scrollback or poll for new
messages.

Imports build short nodes without a model. Long imports are summarized when
the extension starts. A fresh turn waits for the summaries of earlier messages
only if those messages do not fit whole in `<recent>` (spec change C2), and
for enough built parents to keep its view within 128,000 bytes. A failed
summary is retried 15, 60 and 300 seconds later, at your next message, or on
`/unii retry`; a turn that cannot get complete memory reports it, with its
words already saved. Backend and persistence failures stop the turn and block
subsequent tools. Background summary failures appear as pi notifications,
with repeated errors reported once per turn. Authentication errors name the
summary provider to select in `/login`. Unreadable or changed durable memory
also stops the turn; an invalid zoom range or page is an ordinary tool error
that the model can correct.

## Storage and scheduling

```
CHAT/
  main/YYYY-MM-DD.jsonl    {i,kind,text,size,date} plus optional images
  tree/YYYY-MM-DD.jsonl    {l,i,text,size}
  view.json                ordered [l,i] pairs
  compaction-view.json     smaller view and pending batch state
  .state                   journal for view.json's batch state
  .lock                    exclusive POSIX advisory lock
  *.jsonl.torn             set-aside torn final records
```

The format is a stable contract for readers; PROTOCOL.md describes it and the
order in which to read while a writer runs.

Every log write is one `write` and one `fsync` (`F_FULLFSYNC` on macOS, where
plain `fsync` is not a barrier); a log's directory is synced when the file is
created, and the writer syncs `main/`, `tree/`, the chat directory and the
chat directory's parent (so a new chat's own entry is durable) once at open.
Views are replaced atomically, with files and directories synced, when
merges change them or a batch starts or ends. Messages appended since the last
saved view are recovered from the log as one-message lines, exactly as they
were live; restart never rebuilds the view. A final record without its LF in
the newest file was never acknowledged: the writer moves it to `FILE.torn`
and continues. Any other malformed record, a missing or corrupt view, or a
changed indexed record is an error; the program preserves the log for
recovery from backup. Back up the whole chat directory, including the view
and metadata. Stray temp files from an interrupted save are removed at open.

Records go to the day file named for the newest of today's UTC date and the
newest existing file; the record keeps the real time. So a clock step neither
reorders the log nor stops writes.

Non-tool text splits at UTF-8 boundaries into consecutive messages of at most
30,000 bytes. Echoes retain their head and tail, within a 30,000-character
budget; multibyte echoes can span several retrieval pages. Neither message nor
summary text is kept in memory: each index entry names its record (file,
offset, size, kind), and every read re-validates the record. A 131,072-message
chat opens in about 0.33 s with 10 MB of RAM. Text must be valid UTF-8 without
NUL bytes. Invalid JSON requests close the engine's connection so callers
cannot wait indefinitely.

Attached images are stored with the original text in the same durable log
record. Only the first chunk of a split message carries them. Image-bearing
messages are summarized by a model with their images, and `zoom(id,1)` returns
the original image blocks on page zero. Images enter a turn only when attached
to new input or retrieved explicitly; the saved view contains summaries.

A source of at most 512 bytes becomes a node unchanged, including its kind.
Two child summaries join with a newline when that fits. Other nodes use one
model call with the same system prompt and tools as turns, a context view, the
512-dash ruler, and at most five attempts in the same conversation. The
shortest answer is kept; one more than 64 bytes over the limit is cut at a
code point to 512 bytes, so no node can crowd the 32,000-byte contexts out.
Built nodes are immutable.

Up to eight model calls run concurrently, each with a timeout (300 seconds by
default; `unii serve --timeout` or `UNII_TIMEOUT`, monotonic clock). Missing
message nodes and ready parents enter queues; startup makes one linear pass
and subsequent work never rescans the tree. A message starts with fewer than
eight unbuilt predecessors. Context stops before the first missing node and
never exposes part of a message. Jobs whose complete context would exceed
32,000 bytes wait for earlier parent merges, in a heap by their context
cutoff, and become eligible as the saved compaction view shrinks.

The turn view grows past 128,000 bytes, then merges toward 64,000. Only built
parents are eligible. Pair priority uses age from the pair's **last** message,
with exact integer comparisons and the oldest equal-age pair first. An
unfinished batch resumes as parents become available. The compaction view
uses the same rule at 32,000 → 16,000 bytes and follows turn-view batches.

For Anthropic, view text uses four-line blocks and ephemeral cache marks on
the shared system prefix, the last full view block, and the request end, plus
the previous turn's last full block while it is unchanged (so a long turn
does not push the next mark beyond the provider's lookback). Requests writing
the same marked prefix, turns and summaries alike, wait until the first
response begins. There are no one-hour cache purchases, keepalive requests or
native pi compactions. Cache behavior still depends on the provider, model,
account, and entry lifetime; no live hit-rate claim is made by the tests.
Other providers receive the same bounded memory view and use their own
caching behavior.

The extension uses ordinary pi tools and does not launch subagents. `work`
records can be imported through the engine; optional named subagent chats
and `zoom("Name")` are not exposed.

## Where this differs from the spec

Deliberate changes, each easy to find in the code:

- **Turns and summaries share a cache entry only on the same model (C1).**
  The spec says tools and system prompt are cached across turns and
  compactions alike, and that compactions read them from the turns' entry.
  Provider caches are per model: with a separate summary model (the spec's
  Haiku, or GPT-6 Luna here) they never share. Compactions still share their
  prefixes with each other, which is the larger win. The summary model also
  differs from the spec's Claude Haiku: GPT-6 Luna via `openai-codex`.
- **Unsummarized messages go whole after the view (C2).** The spec's turn
  waits until every earlier message is summarized. Here, when those messages
  fit in 32,000 bytes, the turn proceeds at once: the view is rendered up to
  the first unsummarized message (still only summaries, never part of a
  message, and a prefix of the later view, so the cache holds) and the
  messages follow verbatim in `<recent>`. One prompt sentence says so. Set
  `TAIL_BYTES` to 0 in `src/store.h` (or build with `-DTAIL_BYTES=0`) to
  restore the spec exactly (wait, and the spec's prompt sentence).
- **Zoom results are logged by reference (C4).** The spec logs every tool
  result as an `echo`. A successful `zoom` is logged as a short line such as
  `zoom {"id":5,"n":1,"page":0}: message 5 page 0/1, 8123 bytes; not copied;
  zoom again to read`, without images: the tree already holds what zoom
  shows, and copying it costs log space and a summary call. Errors and `date`
  results stay verbatim.
- **Shell commands are logged as typed (C7):** `!ls -la`, not `ls -la`.
- **Clock steps (A2).** The spec files records by day. A record whose day is
  earlier than the newest file goes into the newest file, keeping its real
  date: a record's day may precede its file's day.
- **Torn final records (A3).** The spec says never edit a line. An
  unterminated final line was never acknowledged; the writer moves its bytes
  to `FILE.torn` and truncates it from the log instead of refusing to open.
- **Retries (B5).** The spec retries a failed call at the next message. Here
  it is also retried after 15, 60 and 300 seconds and on `/unii retry`, and
  only the user's messages (typed input, steering, `!commands`) retry it: one
  turn logs hundreds of replies, tool calls and results, and retrying on each
  would hammer a provider that is failing or rate-limiting.
- **Size ceiling (A1).** "A few bytes over is fine" is bounded: a shortest
  answer more than 64 bytes over is cut at a code point to 512 bytes.
- **View saves (D9).** Appending a message no longer rewrites `view.json`;
  the appended one-message lines are recovered from the log at open, which is
  the spec's own crash-tail rule. The view is still never rebuilt.

## Hosting from another program, and the iOS app

PROTOCOL.md is the whole engine interface: framing, every operation and
event, cache gating, history paging (`messages`), live events (`append`,
`built`, `progress`, `error`) and the on-disk reader contract. A phone client
talks to pi in RPC mode (`pi --mode rpc --no-session -e pi/unii.ts`) through
an authenticated relay on the computer: `prompt` (with images), `steer`,
`abort`, pi's event stream, and extension UI records (`notify`, `setStatus`).
Scrollback pages by permanent message id with `unii log CHAT FROM COUNT`,
which is safe while pi runs.

## Verification

`make test` runs entirely offline. It checks strict JSON and UTF-8 behavior,
writer exclusion, lock-free readers and `unii log`, restart and crash
recovery (torn tails, clock steps, interrupted saves), exact rollback-push
equivalence through 20,000 steps, the byte sawtooth, queue scheduling,
context cutoffs, length corrections and the size ceiling, automatic and
explicit retries, `<recent>` turns, zoom references, history paging, events,
cache gating, call timeouts, the fsync budget per message, and the full engine
driven by a test host that answers its calls, including requests piped up to
end of input and one socket serving as both stdin and stdout.

Storage faults are injected by replacing `fsync` at build time
(`tests/fsync_test`): a failed log sync must stop the engine with the log
intact, and the sync count per message is bounded. That build skips
`F_FULLFSYNC` so the injection also applies on macOS; these tests have been
run on Linux only.

The optional `make test-pi` uses an installed pi with deterministic offline
providers to exercise the actual extension hooks, direct summary calls
through pi's model registry (31 calls with shared system prompt and tools,
fresh contexts and cache marks), memory tools, steering, native shell
commands, failure notices, `/unii retry` and `/unii restart`, a message sent
during `/unii flush`, and the JS adapter's cache marking, well-formed requests
and per-engine call routing, without API keys or network requests.
See [pi's extension API](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/extensions.md)
and [RPC protocol](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/rpc.md).
