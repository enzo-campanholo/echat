# UniiChat

Persistent chat memory for [pi](https://github.com/earendil-works/pi), stored
as an append-only log, a binary summary tree, and a saved view. Recent
messages remain detailed; older messages are represented by larger summaries.
`zoom` retrieves the originals.

The engine is C89 with POSIX interfaces and no external libraries. It handles
storage, summary scheduling, context limits, retries and cache gating. The
pi extension (`pi/unii.ts`) performs model calls through pi's model registry
and connects its hooks and tools to the engine. Other hosts can use the same
interface, documented in [PROTOCOL.md](PROTOCOL.md).

## Build and run

On NixOS:

```sh
nix-shell --run 'make test'
pi --no-session -e ./pi/unii.ts
```

On another POSIX system with a C compiler and make:

```sh
make test
pi --no-session -e ./pi/unii.ts
```

Pi must have a configured model and credentials. Turns use the model selected
in pi. Summaries use `openai-codex/gpt-6-luna` at `xhigh` effort by default,
with pi's credentials. To choose another summary model:

```sh
pi --no-session -e ./pi/unii.ts \
  --unii-dir "$HOME/.local/state/uniichat" \
  --unii-provider openai-codex --unii-model gpt-6-luna --unii-effort xhigh
```

`UNII_PROVIDER`, `UNII_MODEL` and `UNII_EFFORT` set the same options for pi
and `unii serve`; flags take precedence. `UNII_TIMEOUT` sets the per-call
timeout in seconds (default 300, range 1–86400). The engine also accepts
`unii serve --timeout`.

The default chat directory is `$XDG_STATE_HOME/uniichat`, or
`~/.local/state/uniichat`. It is shared across projects. Use `--unii-dir`
for a separate chat. Each chat permits one writer; readers need no lock.

To register the built repository as a local pi package:

```sh
pi install . -l
pi --no-session
```

The extension can be loaded by absolute path from another directory.
`UNII_BIN` or `--unii-bin` selects a separately installed engine.
`nix-build` builds and tests an installable package, with the extension at
`result/share/unii/pi/unii.ts` and the engine at `result/bin/unii`.

Use `--no-session` to avoid a second pi session log containing thoughts.
UniiChat does not log thinking blocks. Each model request contains the saved
view and the current turn's messages; earlier turns remain visible in pi's UI.
Messages arriving during memory preparation or a reply are saved as steering
in that turn. Its view ends before the initiating message.

## Instructions and commands

[pi/prompt.txt](pi/prompt.txt) is built into the engine. Run `make` after
editing it; `unii prompt` prints the active prompt. One sentence changes when
`<recent>` is enabled, as described below.

Pi's project instructions and custom prompt sections are captured at startup
and appended to the prompt. `UNII_INSTRUCTIONS` can name an additional UTF-8
instructions file. Restart pi after changing instructions. The date and
working directory are sent separately after the view.

- `/unii`: show message count, summarized prefix, view size, model and jobs.
- `/unii flush`: finish queued summaries or report failed work. A new message
  ends a waiting flush and starts its turn.
- `/unii retry`: retry failed summaries without sending a message.
- `/unii restart`: relaunch the engine without restarting pi.
- `/unii import /path/to/file`: append the file as a `note`.
- `zoom(id,n,page?)`: open a node into its children. `n` must be a power of
  two and `id` a multiple of `n`. At `n=1`, retrieve the original message;
  pages start at zero.
- `date(id)`: retrieve a message's recorded timestamp.

Included `!command` and RPC bash executions are saved before execution as
typed, followed by clipped output and exit status. `!!command` and RPC's
`excludeFromContext` option exclude an execution from memory. Included
commands do not run if their input cannot be saved.

The status line reports summary progress while a turn waits. Unsaved input
and summary failures produce notices, including RPC `notify` records.
Authentication failures identify the provider to select in `/login`.

The CLI can inspect or import memory while pi runs:

```sh
printf 'Personal instructions or earlier memories.\n' | ./unii add CHAT note
./unii view CHAT
./unii zoom CHAT 0 1
./unii date CHAT 0
./unii status CHAT
./unii log CHAT 100 50      # records 100..149
```

`view`, `zoom`, `date`, `status` and `log` do not create, lock or write a chat.
`add` takes the writer lock. `log` streams records without building an index;
use it to page scrollback or read messages since a known id.

## Memory and summaries

A turn can proceed before earlier messages are summarized if those messages
fit whole in a 32,000-byte `<recent>` block and the preceding view fits in
128,000 bytes. Otherwise it waits. Set `TAIL_BYTES` to 0 in `src/store.h`, or
build with `-DTAIL_BYTES=0`, to require all earlier summaries.

Text-only sources of at most 512 bytes become nodes unchanged, including
the message kind. Two child summaries join with a newline when they fit.
Other nodes require a model call with the turn's system prompt and tool
definitions, a context view and a 512-dash ruler. Summaries cannot execute
tools. The engine allows five answers in the same conversation and keeps
the shortest. Answers over 576 bytes are cut at a UTF-8 boundary to 512 bytes.
Built nodes are immutable. CLI imports build shortcuts immediately and leave
model work for the next host session.

Up to eight calls run concurrently, with monotonic-clock timeouts. Missing
leaves and ready parents enter queues. Startup scans the tree once;
completion enqueues subsequent work. Jobs whose context exceeds 32,000 bytes
wait for earlier merges to reduce it. Context ends before the first missing
node and does not contain partial messages.

The turn view starts merging above 128,000 bytes and continues toward 64,000
as built parents become available. Pairs are ranked by age from their last
message, with the older pair first on a tie. The summary context view uses
the same rule at 32,000 and 16,000 bytes.

Failed summaries retry after 15, 60 and 300 seconds, on a new user message,
or on `/unii retry`. Replies and tool output do not trigger retries. A turn
that cannot obtain its memory reports an error; its input remains saved.
Storage failures stop the engine and block subsequent tools. Invalid zoom
arguments or ranges remain ordinary tool errors.

When a host reports an answered-empty result or explicit model refusal, the
engine uses the shortest earlier answer, or a marked source excerpt of at
most 512 bytes with a `zoom` reference. Provider errors never trigger this
fallback. The pi adapter captures structured refusal events before pi's
OpenAI Responses normalization (including Codex and Azure), and requires both
a successful terminal provider response and pi's completed `done` event.
Aborts, stream failures, tool calls and persistence errors remain retryable,
even after partial text or refusal fragments. Responses function/custom-tool
calls are rejected from raw item events and terminal snapshots as well as
normalized content. Completed empty answers also use fallback. Refusal support
is not universal; see [Known limitations](#known-limitations).

For Anthropic, the view is split into four-line blocks. Ephemeral cache marks
cover the system prefix, last full view block and request end. The previous
turn's breakpoint is retained while unchanged to stay within the provider's
cache lookback. Requests writing the same marked prefix wait for the first
response to begin. Cache entries are model-specific. Other providers receive
the same bounded view and use their own caching behavior. Native pi
compaction and idle cache warming are disabled.

## Storage

```
CHAT/
  main/YYYY-MM-DD.jsonl    {i,kind,text,size,date} plus optional images
  tree/YYYY-MM-DD.jsonl    {l,i,text,size}
  view.json                ordered [l,i] pairs
  compaction-view.json     summary context view and pending batch state
  .state                   journal for view.json's batch state
  .lock                    POSIX advisory writer lock
  *.jsonl.torn             preserved incomplete records, in main/ or tree/
```

[PROTOCOL.md](PROTOCOL.md#on-disk-format-reader-contract) defines the record
format and read order for concurrent readers.

The engine syncs each append batch before acknowledging it, and syncs the
containing directory when creating a file. On macOS it tries `F_FULLFSYNC`
before falling back to `fsync`. Writer startup batches directory barriers
across the chat, log directories and every ancestor through the namespace
root. Paths are walked from that root; directory identity deduplicates barriers
within one open, but never stops traversal. Relative paths include the working
directory's ancestors; symlink chains, target prefixes and `..` are traversed
without lexical normalization. Preexisting directories are synced even after
`EEXIST`: existence does not prove durability.
New directories themselves are synced too. The initial empty view and journal
share setup's final directory barrier; later view replacement still persists
the journal first. An empty chat needs no compaction-view file: it is created
when nonempty initialization, merges or batch changes require saved state.
Every open retries the required barriers.
Directory-open, sync and close failures prevent acknowledgment. This adds
startup barriers, not per-append work. External processes must not
rename or replace the directory namespace while a writer uses it; the chat
lock excludes competing engine writers, not arbitrary filesystem mutation.

Views are saved with write, sync, rename and directory sync when merges or
batch state change. Messages appended since the last saved view are recovered
as leaves; restart does not rebuild merges. An unterminated final record in
the newest log was not acknowledged. The writer saves its bytes in
`FILE.torn` before truncating it. Other malformed records, missing views on
nonempty logs, and changed indexed records are errors. Back up the entire
chat directory, including views and metadata. Startup removes stray save
temporary files.

Daily filenames never move backward when the clock changes. Records retain
their actual timestamps and use the newest of today's date and the newest
existing filename.

Non-echo text splits at UTF-8 boundaries into messages of at most 30,000
bytes. Echoes keep their head and tail within 30,000 code points, so multibyte
echoes can occupy several retrieval pages. Text must be valid UTF-8 without
NUL. Invalid JSON requests close the engine connection.

The in-memory index holds disk locations and SHA-256 fingerprints of complete
JSON records, excluding LF. Rereads detect changes to text, dates and image
data, including same-length edits. Fingerprints last only for the open store:
they do not authenticate valid edits made before indexing or restart. Checks
are lazy; reading a summary does not rescan its original messages.

Images share their message's durable record. Only the first part of a split
message carries them. Image-bearing messages require model summaries.
`zoom(id,1)` returns images on page zero. Images enter a turn with new input
or explicit retrieval, not through the saved view.

Successful zoom results are logged as short references without images, not
copied back into memory. The full retrieval page, including its header,
bypasses generic tool-output clipping. Errors and `date` results are logged
normally.

## Differences from upstream

Compared with [uniichat](https://github.com/VictorTaelin/uniichat):

- Summaries use a separately configured pi model rather than Claude Haiku.
  Turns and summaries share cache entries only when they use the same model.
- `<recent>` permits turns before all earlier summaries finish.
- Successful zooms are logged by reference; shell commands retain `!`.
- Backward clock steps do not change append order. Unterminated final records
  are preserved separately rather than preventing writer startup.
- Automatic retries and `/unii retry` supplement retries on user input.
- Summary overruns have a 64-byte allowance; larger overruns are cut.
- Appends do not rewrite `view.json`; restart recovers their leaves from logs.
- Named subagent chats and `zoom("Name")` are not exposed. The extension does
  not launch subagents, though hosts can append `work` records.

## Other hosts

[PROTOCOL.md](PROTOCOL.md) specifies requests, events, history paging and
cache gating. A remote client can also use pi's RPC mode through an
authenticated relay:

```sh
pi --mode rpc --no-session -e pi/unii.ts
```

Pi RPC supplies prompts, images, steering, aborts and UI events. Use
`unii log CHAT FROM COUNT` for scrollback by permanent message id.
See pi's [extension API](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/extensions.md)
and [RPC protocol](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/rpc.md).

## Verification

`make test` runs offline C and integration suites for storage, recovery,
UTF-8 and JSON, views, scheduling, retries, timeouts and the engine protocol.
Storage-fault checks replace `fsync` at build time and verify that failed log
syncs stop the engine. These suites have been run on Linux.

`make test-pi` requires an installed pi and uses deterministic offline
providers. It exercises extension hooks, summary calls, memory tools, images,
steering, shell execution, cache marks, failure notices and engine restarts
without API keys or network requests. It does not establish live-provider
behavior, cache hit rates, power-loss recovery or macOS durability.

## Known limitations

- Pi 1.4.0 maps Anthropic `stop_reason: "refusal"` to `stopReason: "error"`.
  `rawStopReason` survives, but the same error result can represent a later
  transport/parse failure, even after `message_stop`. The supported raw-event
  callback has no clean-stream-end/error-provenance signal. The adapter keeps
  these ambiguous Anthropic results retryable rather than hiding a failure.
  Safe Anthropic refusal fallback needs trustworthy clean-stream-end/error
  provenance. This adapter has no Anthropic wire validator and does not
  change installed pi code or mutate its read-only events.
- Structured refusal capture currently covers `openai-responses`,
  `openai-codex-responses` and `azure-openai-responses`. Compatible/custom
  implementations must forward the request-local `onProviderStreamEvent`
  callback and terminal response status; missing metadata is retryable.
  Other providers' generic errors remain retryable, and unstructured refusal
  prose cannot be distinguished safely from ordinary answer text. No refusal
  is inferred from wording or prompt instructions.
