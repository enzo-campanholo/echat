# UniiChat engine protocol (version 1)

`unii serve DIR` is UniiChat's memory engine. It owns all memory policy:
what is logged, when a node is built, the context each summary sees, the
ruler and length corrections, the five-attempt rule, the size ceiling,
timeouts, retries, concurrency and cache gating. A **host** (the pi
extension `pi/unii.ts`, or any other program) owns the model, its system
prompt, its tools and the user interface. The host performs the model calls
the engine asks for.

The second half of this document is the on-disk format, which is a stable
contract for readers in its own right.

## Framing

- The host writes requests to the engine's stdin; the engine writes records
  to its stdout. stderr carries diagnostics only.
- One JSON object per line, UTF-8, terminated by LF (byte 10). Split on LF
  only; never on U+2028/U+2029 or CR.
- JSON is strict: duplicate keys, `\u0000`, unpaired surrogate escapes
  (`\ud800`), raw NUL bytes, invalid UTF-8 and trailing data are rejected.
  Hosts whose strings can hold lone UTF-16 surrogates (JavaScript) should
  replace them with U+FFFD before encoding.
- A line that does not parse as a JSON object gets one
  `{"id":null,"ok":false,"error":"invalid JSON request"}` and the engine
  exits: an unparsed request has no trustworthy id, and closing the channel
  rejects every waiter instead of stranding one.

## Records

Every output line is either a **response** or an **event**.

- A response answers one request: `{"id":ID,"ok":true,...}` or
  `{"id":ID,"ok":false,"error":"..."}`. `ID` is copied from the request
  (any JSON value; `null` if the request had none). Hosts should use
  numbers or strings.
- A response with `"fatal":true` means durable storage failed: the engine
  stops, and the host must stop using memory (the log on disk is intact up
  to the failure). A fatal response may carry the id of an unrelated waiting
  request, or `null`.
- An event has an `"event"` field and never an `"id"` field.

Requests are answered in the order they are dispatched, except `turn`,
`flush` and `cache_wait`, which may be answered later; other requests are
served while they wait.

## Operations

### hello

`{"id":1,"op":"hello","protocol":1}` ->
`{"id":1,"ok":true,"protocol":1,"messages":N,"summarized":C,"provider":P,"model":M,"effort":E}`

Starts model work and enables events. Until `hello`, the engine logs and
answers reads but asks for no model calls. `messages` is the number of
messages in the log; `summarized` the length of the prefix whose leaves are
built. `provider`, `model` and `effort` are the summary model chosen by
`unii serve --provider/--model/--effort` (defaults `openai-codex`,
`gpt-6-luna`, `xhigh`, or `UNII_COMPACT_PROVIDER`/`_MODEL`/`_EFFORT`).

`configure` is the protocol-0 form: it also starts model work, enables
events only if `"events":true`, and ignores its old `system`, `tools` and
`provider_tools` fields (the host owns those now). Protocol 0 returned
`append`'s `message_id` as a JSON string (`"0"`); it is now a number for
every host, as documented below.

### turn

`{"id":2,"op":"turn","text":"...","images":[...]}` ->
`{"id":2,"ok":true,"view":"<chat>\n...</chat>\n"}`

Logs `text` (and optional image blocks) as a `user` message, then waits
until every earlier message is summarized and the view rendered before the
new message fits in 128,000 bytes. The response's `view` is that rendering:
the host sends it, then the message, in a fresh model context. Errors (the
input stays logged; the next message retries): a summary failed or is
blocked, or the view cannot fit.

One `turn` or `flush` waits at a time. A `turn` that arrives while a
`flush` waits ends that flush with the error `flush superseded by a turn`
and proceeds normally, so typed input is never refused for a flush. A
`turn` or `flush` that arrives while a turn waits, or a `flush` while a
flush waits, is refused (`another turn is waiting for memory`) and logs
nothing.

The engine does not wait when both of these hold: the `<recent>` block
below for the messages before the turn that are not summarized yet (tags,
headers and texts) fits in 32,000 bytes, and the view up to the first of
them fits in 128,000 bytes. The response is then
`{"ok":true,"view":V,"recent":R}`, where `V` is that view (still only
summaries) and `R` holds those messages whole:

```
<recent>
[message 12 kind=unii]
full text, newlines kept
(has images: zoom(12,1))
</recent>
```

The host places `R` after the view and before its per-turn state. `recent`
is absent when memory has caught up. If either bound fails, the turn waits
as above.
(This is a deliberate change from the spec; building with `TAIL_BYTES` 0,
for example `make CPPFLAGS='-D_POSIX_C_SOURCE=200809L -DTAIL_BYTES=0'`,
restores the spec's wait.)

Images are pi blocks `{"type":"image","data":BASE64,"mimeType":"image/..."}`.

### flush

`{"id":3,"op":"flush"}` -> `{"id":3,"ok":true}` once no summary work is
queued or running; an error if some failed or are blocked, or if a `turn`
arrived while it waited (`flush superseded by a turn`).

### append, steer

`{"id":4,"op":"append","kind":"unii","text":"...","images":[...]}` ->
`{"id":4,"ok":true,"message_id":N}`

Logs a message. `kind` is one of `user`, `unii`, `tool`, `echo`, `work`,
`note`; `message_id` is the first id used (long text is split into several
messages of at most 30,000 bytes; `echo` text is clipped to 30,000
characters, head and tail). `steer` is `append` with kind `user`, for input
that arrives while a turn runs.

`{"id":4,"op":"append","records":[{"kind":"unii","text":"..."},{"kind":"tool","text":"..."}]}`
logs several entries, in order, with one write and one fsync (for example an
assistant message's replies and tool calls). Invalid kinds, text or images
are an ordinary error and log nothing; a storage failure is fatal.

### zoom, date

`{"id":5,"op":"zoom","args":{"id":0,"n":4,"page":0}}` ->
`{"id":5,"ok":true,"text":"0+2|...\n2+2|...\n"}`

`n` is a power of two and `id` a multiple of `n`. `n > 1` opens the node
into its two children; `n = 1` returns message `id` whole in pages of at
most 30,000 bytes, with `"images"` on page 0 if it has any. An impossible
range is an ordinary error the model can correct.

A successful zoom also carries `"log"`, a line of under 512 bytes such as
`zoom {"id":5,"n":1,"page":0}: message 5 page 0/1, 8123 bytes; not copied;
zoom again to read` (for `n > 1`, the heads of the two child lines). Log
that as the tool result's `echo`, without images, instead of the result:
the tree already holds what zoom shows.

`{"id":6,"op":"date","args":{"id":0}}` -> `{"id":6,"ok":true,"text":"2026-10-09T12:00:00Z"}`

Arguments are read only from `args`: a top-level `id` is the request's
correlation id. Missing or invalid arguments are answered with `ok:true`
and a `text` saying so (for example `zoom: id, n and page must be
nonnegative integers`), for the model to read and correct.

### messages

`{"id":12,"op":"messages","from":N,"count":K,"images":false,"bytes":B}` ->
`{"id":12,"ok":true,"records":[{"i":N,"kind":"user","date":"...","size":4,"text":"...","has_images":true},...],"next":M}`

History by permanent id: records `[N, N+K)` that exist (`count` defaults to
100). The first record is always returned; later ones stop once the response
passes `bytes` (default and maximum 1 MiB). `next` is the first id not
returned: page on with `from = next`, and stop when `next` reaches the log's
end (`messages` from `hello` or `status`, or the last `append` event).
Image blocks are included only with `"images":true`. Every record is
re-validated against the log; an unreadable one is fatal.

### retry

`{"id":13,"op":"retry"}` -> `{"id":13,"ok":true}`

Retries every failed node now, without logging a message. Failed nodes also
retry by themselves 15, 60 and 300 seconds after each failure, then wait
for a new message or `retry`, which restart that schedule.

### status

`{"id":7,"op":"status"}` ->
`{"id":7,"ok":true,"messages":N,"summarized":C,"view_bytes":B,"lines":L,"workers":W,"failed":F,"blocked":K,"summary_provider":P,"summary_model":M}`

### shutdown

`{"id":8,"op":"shutdown"}` -> `{"id":8,"ok":true}`, then the engine exits
without dispatching any later request. Calls still in flight are abandoned.

At end of input the engine first dispatches every complete (LF-terminated)
request it has received, in order, then exits as for `shutdown`. Requests
still waiting then (`turn`, `flush`, `cache_wait`) get no response; a
`turn`'s input is already logged.

## Model calls

### event call

```
{"event":"call","call":N,"provider":P,"model":M,"effort":E,"messages":[
  {"role":"user","text":"<chat>\n...</chat>\n\nCompaction: ...","images":[...]},
  {"role":"assistant","text":"previous answer"},
  {"role":"user","text":"Too long: your line is ..."}]}
```

Perform one plain completion (no tool execution) with the host's own
system prompt and tool definitions (the same ones its turns use, so the
prefix can be shared), the given summary model and reasoning effort, and
these messages in order. `images` appear only on the first message. Each
attempt is a new call id carrying the whole conversation: the host keeps no
state between calls. Up to eight calls are outstanding at once.

### result

`{"id":9,"op":"result","call":N,"text":"the summary line"}` or
`{"id":9,"op":"result","call":N,"error":"why"}` -> `{"id":9,"ok":true}`

The text of the assistant's reply (text blocks joined with LF; thoughts
excluded). A reply that called a tool should be reported as an error. A
result for an unknown or cancelled call gets `ok:false` and is otherwise
ignored; it is not fatal.

### event cancel

`{"event":"cancel","call":N}`: the call exceeded `unii serve --timeout`
(default 300 seconds, monotonic clock). Abort it; its result is refused.

### event error

`{"event":"error","call":N,"l":L,"i":I,"provider":P,"model":M,"text":"why"}`:
building node (L, I) failed. See `retry` for when it is tried again.

## Live events

After `hello`, the engine also reports changes, so a host can mirror the
chat without polling:

- `{"event":"append","first":N,"count":K,"kind":KIND}`: messages `N` to
  `N+K-1` were logged (a turn's input, steering, or an `append`). `kind` is
  present when all of them share it. Fetch them with `messages`.
- `{"event":"built","l":L,"i":I}`: node (L, I) was logged.
- `{"event":"progress","waiting":P}`: a `turn` or `flush` is waiting; `P`
  earlier messages still lack a summary (0: waiting for merges). Sent when
  `P` changes; the wait ends with the request's response.

## Prompt-cache gating

A request whose cache-marked prefix another request is writing should wait
until that request's response starts, so the prefix is paid for once. One
gate serves turns and calls alike (prefixes are per model, so turns and
summaries share entries only when they use the same model).

- `{"id":10,"op":"cache_wait","key":K}` (turn) or with `"call":N` (a call's
  request) -> `{"id":10,"ok":true}` once no other request holds `K`; the
  requester then holds `K`. One waiting `cache_wait` per requester.
- `{"id":11,"op":"cache_started","key":K}` (with `"call":N` for calls):
  the response started, so the entry exists: every waiter on `K` proceeds.
- A requester's next `cache_wait`, or the end of its call, releases what it
  held without a started response; then the first waiter becomes the
  writer.

`K` is any string identifying the marked prefix, for example a hash of the
model, tools, system prompt and messages up to the cache mark.

## On-disk format (reader contract)

```
CHAT/
  main/YYYY-MM-DD.jsonl    {"i":ID,"kind":K,"text":T,"size":BYTES,"date":D[,"images":[...]]}
  tree/YYYY-MM-DD.jsonl    {"l":LEVEL,"i":INDEX,"text":T,"size":BYTES}
  view.json                [[LEVEL,INDEX],...]
  compaction-view.json     {"shrinking":0|1,"view":[[LEVEL,INDEX],...]}
  .state                   writer journal for view.json's batch state
  .lock                    writer lock (POSIX advisory, fcntl)
  *.jsonl.torn             set-aside torn final records (never read)
```

- Logs are append-only daily JSONL; a line is never edited or removed.
  Message ids are consecutive from 0 in file-name order, then line order.
  Node (l, i) covers messages `i*2^l` to `(i+1)*2^l - 1`; children precede
  their parent.
- `size` is the UTF-8 byte length of `text`. `date` is UTC
  (`YYYY-MM-DDTHH:MM:SSZ`, or with a `+HH:MM` offset). A record's day may be
  earlier than its file's day (after a clock step), never later.
- Views are replaced atomically (write, fsync, rename, fsync directory).
  A view lists `[l, i]` pairs covering messages `0..` contiguously; it may
  end before the log does (messages logged since are appended as leaves).
- A final line without LF in the newest file is a write in progress (or, to
  the writer, an unacknowledged torn record). Anything else malformed is
  corruption.

### Reading while the engine runs

Readers need no lock, because the writer only appends whole lines and
renames views atomically. Read in this order:

1. `view.json` first. Everything it names was durable before it was renamed.
2. `main/` in file-name order. Stop at a final line without LF.
3. `tree/` in file-name order. Stop at a final line without LF, and stop at
   the first node covering a message you did not read in step 2: it was
   built after your snapshot, and so was everything after it.
4. Messages after the view's end join it as one-message lines.

A writer that starts while you read may set aside a torn final record
(truncate it, then append new records). A read racing that repair can see
one malformed record and fail with an invalid-record error; retry the read
once before treating it as corruption.

`unii view`, `zoom`, `date`, `status` and `log` read this way: they never
create, lock or write a chat, and work while `unii serve` runs.

`unii log DIR FROM [COUNT]` prints the raw JSONL records with ids
`[FROM, FROM+COUNT)` (to the end without COUNT). It does not build the
index: it reads the first record of each daily file, newest first, to find
the file holding `FROM`, then streams. Each printed record is validated.
This is the cheap way to page scrollback, or to poll for new messages
(`unii log DIR LAST+1`).
