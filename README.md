# UniiChat

An endless chat for [pi](https://github.com/earendil-works/pi). The chat is its
memory: an append-only log, a binary summary tree, and a saved view with fine
recent detail and coarse old detail.

The memory engine is dependency-free C89 using POSIX. A pi extension is the
interface; its small JavaScript/TypeScript adapter connects pi's extension
hooks and custom tools to the C process. Pi supplies the UI, authentication,
providers, streaming, and ordinary coding tools.

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
you select in pi. Background summaries default to Anthropic
`claude-haiku-4-5` at `xhigh` effort, using pi's credentials. You can select a
different summary provider/model:

```sh
pi --no-session -e ./pi/unii.ts \
  --unii-dir "$HOME/.local/state/uniichat" \
  --unii-provider anthropic --unii-model claude-haiku-4-5 --unii-effort xhigh
```

The default chat directory is `$XDG_STATE_HOME/uniichat`, or
`~/.local/state/uniichat`. It is shared across projects, so the chat follows
you. Use `--unii-dir` for a separate chat. One running pi process owns each
chat; a second writer is refused.

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
showing earlier turns.
Messages arriving during memory preparation or a reply are logged as steering
and remain in that turn. Its view stays at the boundary before the initiating
message.

## Instructions and commands

The shared prompt is [pi/prompt.txt](pi/prompt.txt). Pi's project instructions
and custom prompt sections are captured at startup and appended to it. Date
and working directory are sent after the view. `UNII_INSTRUCTIONS` can name a
UTF-8 file with additional personal instructions. Restart pi when changing
these instructions so the prompt stays constant during a run.

- `/unii`: show message count, summarized prefix, view size and worker status.
- `/unii flush`: finish currently queued summaries; report any failed work.
- `/unii import /path/to/file`: append a verbatim `note` to memory.
- `zoom(id,n,page?)`: open an aligned power-of-two node into its children, or
  read a whole message at `n=1`. Pages are numbered from zero.
- `date(id)`: retrieve the message's UTC date and time.

You can inspect or import memory while pi is stopped:

```sh
printf 'Personal instructions or earlier memories.\n' | ./unii add CHAT note
./unii view CHAT
./unii zoom CHAT 0 1
./unii date CHAT 0
./unii status CHAT
```

Imports build short nodes without a model. Long imports are summarized when
the extension starts. A failed model call stays queued for the next message;
a fresh turn waits for all earlier message summaries and enough built parents
to keep its view within 128,000 bytes. It reports a failure if either condition
cannot be met. Its incoming words are already saved, and the next message
retries failed work. Backend and persistence
failures stop the turn and block subsequent tools.
Unreadable or changed durable memory also stops the turn; an invalid zoom
range or page is an ordinary tool error that the model can correct.

## Storage and scheduling

```
CHAT/
  main/YYYY-MM-DD.jsonl    {i,kind,text,size,date} plus optional images
  tree/YYYY-MM-DD.jsonl    {l,i,text,size}
  view.json               ordered [l,i] pairs
  compaction-view.json    smaller view and pending batch state
  .state                  journal for saved-view batch state
  .lock                   exclusive POSIX advisory lock
```

Every log line is flushed and synced. Views are replaced atomically, with
files and directories synced. Restart loads the saved view exactly; only a
fully written crash tail missing from the saved view is appended. Missing or
corrupt views, invalid records and torn final log lines cause an error;
the program preserves the log for recovery from backup. Back up the whole
chat directory, including the view and metadata.

Non-tool text splits at UTF-8 boundaries into consecutive messages of at most
30,000 bytes. Echoes retain their head and tail, within a 30,000-character
budget; multibyte echoes can span several retrieval pages. Message text is
indexed by file offsets rather than kept in memory. Node summaries are kept
in memory; their storage grows with the chat.
Text must be valid UTF-8 without NUL bytes. Invalid JSON requests close the
backend protocol connection so callers cannot wait indefinitely.

Attached images are stored with the original text in the same durable log
record. Only the first chunk of a split message carries them. Image-bearing
messages are summarized by a model with their images, and `zoom(id,1)` returns
the original image blocks on page zero. Images enter a turn only when attached
to new input or retrieved explicitly; the saved view contains summaries.

A source of at most 512 bytes becomes a node unchanged, including its kind.
Two child summaries join with a newline when that fits. Other nodes use one
model call with the same system prompt and tools as turns, a context view,
the 512-dash ruler, and at most five attempts. The shortest successful answer
is kept, including a slightly oversized answer after the last correction.
Built nodes are immutable.

Up to eight model calls run concurrently. Missing message nodes and ready
parents enter queues; startup makes one linear pass and subsequent work
never rescans the tree. A message starts with fewer than eight unbuilt
predecessors. Context stops before the first missing node and never exposes
part of a message.
Jobs whose complete context would exceed 32,000 bytes wait for earlier parent
merges. Blocked jobs are kept in a heap by their context cutoff, and become
eligible as the saved compaction view shrinks. Failed calls remain separate
and retry only after a new message.

The turn view grows past 128,000 bytes, then merges toward 64,000. Only built
parents are eligible. Pair priority uses age from the pair's **last** message,
with exact integer comparisons and the oldest equal-age pair first. An
unfinished batch resumes as parents become available. The compaction view
uses the same rule at 32,000 → 16,000 bytes and follows turn-view batches.

For Anthropic, view text uses four-line blocks and ephemeral cache marks on
the shared system prefix, the last full view block, and the request end.
Calls writing the same prefix wait
until the first response begins. There are no one-hour cache purchases,
keepalive requests or native pi compactions. Cache behavior still depends on
the provider, model, account, and entry lifetime; no live hit-rate claim is
made by the tests. Other providers receive the same bounded memory view and
use their own caching behavior.

The extension uses ordinary pi tools and does not launch subagents. `work`
records can be imported through the backend; optional named subagent chats
and `zoom("Name")` are not exposed.

## Verification

`make test` runs entirely offline. It checks strict JSON and UTF-8 behavior,
writer exclusion, restart/crash recovery, exact rollback-push equivalence
through 20,000 steps, the byte sawtooth, queue scheduling, context cutoffs,
length corrections, streamed thoughts versus durable messages, and full
C-daemon turns with eight concurrent deterministic pi RPC peers.

The optional `make test-pi` uses an installed pi with a deterministic test
provider to exercise the actual extension hooks and memory tools without
API keys or network requests. It also checks shared worker/turn prompts and
tool definitions, reused worker sessions, cache barriers, and failure paths.
See [pi's extension API](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/extensions.md)
and [RPC protocol](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/rpc.md).
