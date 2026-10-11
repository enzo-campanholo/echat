/* Unit tests for the JS adapter, run as a pi extension (pi has no plain
 * script mode here). It exits 0 on success before pi reads any input. */
import { EventEmitter } from "node:events";
import { chatBlocks, cachePayload, Calls, Records, imageBlocks, wellFormed } from "../pi/common.mjs";

function check(ok: boolean, what: string) {
  if (!ok) {
    console.error("pi_js_test: " + what);
    process.exit(1);
  }
}

function view(lines: string[]) {
  return "<chat>\n" + lines.map(line => line + "\n").join("") + "</chat>\n\n";
}

function turn(lines: string[]) {
  return {
    model: "m", max_tokens: 100, system: [{ type: "text", text: "S" }], tools: [],
    messages: [{ role: "user", content: [...chatBlocks(view(lines)), { type: "text", text: "state" },
      { type: "text", text: "message" }] }],
  };
}

function marks(payload: any) {
  let n = payload.system.filter((block: any) => block.cache_control).length;
  for (const message of payload.messages)
    for (const block of message.content) if (block.cache_control) n++;
  return n;
}

function marked(payload: any) {
  return payload.messages[0].content.map((block: any, i: number) => block.cache_control ? i : -1)
    .filter((i: number) => i >= 0);
}

/* The engine rejects "\ud800" escapes and closes the channel; a lone
 * surrogate in a prompt must arrive as U+FFFD instead. */
function surrogates() {
  const written: string[] = [];
  const output = { write(s: string, done?: any) { written.push(s); done?.(); return true; }, on() {} };
  const records = new Records(new EventEmitter(), output);
  records.request({ op: "append", kind: "user", text: "a\ud800b\udc00c😀",
    records: [{ kind: "unii", text: "\udbff" }] });
  check(written.length === 1 && written[0].endsWith("}\n"), "one request line");
  check(!/\\ud[89a-f]/i.test(written[0]), "no surrogate escapes on the wire");
  const sent = JSON.parse(written[0]);
  check(sent.text === "a�b�c😀" && sent.records[0].text === "�",
        "lone surrogates become U+FFFD; pairs survive");
  const native = Object.getOwnPropertyDescriptor(String.prototype, "toWellFormed");
  delete (String.prototype as any).toWellFormed;
  check(wellFormed("x\udc00\ud800y😀") === "x��y😀", "fallback without toWellFormed");
  if (native) Object.defineProperty(String.prototype, "toWellFormed", native);
}

/* Call ids restart at 1 in each engine. A summary still running when the
 * engine restarts must answer the engine that asked, never the new one. */
async function restartedCalls() {
  const sent: any[] = [];
  const engine = (name: string) => ({
    request(data: any) { sent.push({ engine: name, ...data }); return Promise.resolve({ ok: true }); },
  });
  let finishOld: any, finishNew: any, oldSignal: any;
  const old = new Calls(engine("old"), async (event: any, signal: AbortSignal, send: any) => {
    oldSignal = signal;
    await send({ op: "cache_wait", key: "K", call: event.call });
    return await new Promise(resolve => { finishOld = resolve; });
  });
  const oldRun = old.run({ call: 1 });
  old.close(); /* /unii restart */
  check(oldSignal?.aborted === true, "restart aborts the old engine's calls");
  const next = new Calls(engine("new"), () => new Promise(resolve => { finishNew = resolve; }));
  const nextRun = next.run({ call: 1 });
  while (!finishOld) await new Promise(resolve => setImmediate(resolve));
  finishOld("OLD SUMMARY"); /* the reply finished despite the abort */
  await oldRun;
  check(next.live.has(1), "the old call's end keeps the new call 1's controller");
  check(!sent.some(r => r.engine === "new"), "nothing of the old call reaches the new engine");
  check(sent.some(r => r.engine === "old" && r.op === "cache_wait") &&
        sent.some(r => r.engine === "old" && r.op === "result" && r.call === 1 && r.text === "OLD SUMMARY"),
        "the old call's gating and result go to its own engine");
  finishNew("NEW SUMMARY");
  await nextRun;
  const fresh = sent.filter(r => r.engine === "new");
  check(fresh.length === 1 && fresh[0].op === "result" && fresh[0].text === "NEW SUMMARY" && !next.live.size,
        "the new engine gets only its own result");
  await old.run({ call: 2 });
  check(!sent.some(r => r.call === 2), "a closed engine's late call events start nothing");
}

export default async function () {
  const lines = (from: number, n: number, tag = "") =>
    Array.from({ length: n }, (_, i) => (from + i) + "+1|line " + (from + i) + tag);
  const blocks = chatBlocks(view(lines(0, 9)));
  check(blocks.length === 3 && blocks[0].text.startsWith("<chat>\n0+1|") &&
        blocks[2].text === "8+1|line 8\n</chat>\n\n", "chatBlocks splits four-line blocks");
  check(chatBlocks("plain").length === 1, "chatBlocks keeps non-view text");

  /* A long turn keeps the previous turn's block as a fourth mark. */
  const memory: any = {};
  let p = cachePayload(turn(lines(0, 8)), undefined, memory).payload;
  check(marks(p) === 3 && marked(p)[0] === 1, "first turn: system, last whole block, end");
  p = cachePayload(turn(lines(0, 8)), undefined, memory).payload;
  check(marks(p) === 3, "same view: the previous block is the current one");
  p = cachePayload(turn(lines(0, 100)), undefined, memory).payload;
  check(marks(p) === 4 && marked(p)[0] === 1 && marked(p)[1] === 24, "long turn: previous block marked too");
  p = cachePayload(turn(lines(0, 104, " merged")), undefined, memory).payload;
  check(marks(p) === 3, "after a batch the old block changed: no extra mark");
  p = cachePayload(turn(lines(0, 120)), undefined, undefined).payload;
  check(marks(p) === 3, "compactions pass no memory");
  const keyed = cachePayload(turn(lines(0, 8))).key;
  check(typeof keyed === "string" && keyed === cachePayload(turn(lines(0, 8))).key, "stable prefix key");
  const image = { type: "image", data: "AAAA", mimeType: "image/png" };
  const images = imageBlocks([{ type: "text", text: "t" }, { ...image, name: "extra", detail: "high" }]);
  check(JSON.stringify(images) === JSON.stringify([image]), "image blocks keep only what the engine stores");
  check(imageBlocks(undefined) === undefined, "no images, no field");
  surrogates();
  await restartedCalls();
  console.error("pi_js_test: ok");
  process.exit(0);
}
