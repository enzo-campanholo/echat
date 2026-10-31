import { createHash } from "node:crypto";

export const zoomSchema = {
  type: "object", properties: {
    id: { type: "integer", minimum: 0 },
    n: { type: "integer", minimum: 1, default: 1 },
    page: { type: "integer", minimum: 0, default: 0 },
  }, required: ["id"], additionalProperties: false,
};
export const dateSchema = {
  type: "object", properties: { id: { type: "integer", minimum: 0 } },
  required: ["id"], additionalProperties: false,
};

export function registerMemory(pi, memory) {
  const content = result => typeof result === "string" ? [{ type: "text", text: result }]
    : [{ type: "text", text: result.text }, ...(result.images || [])];
  pi.registerTool({
    name: "zoom", label: "Zoom", parameters: zoomSchema,
    description: "Open line id+n into its two child lines. n is a power of two and id a multiple of n. n=1 gives the whole message in numbered pages; page defaults to 0.",
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    async execute(_id, args) {
      const { id, n = 1, page = 0 } = args;
      const result = await memory("zoom", { id, n, page });
      return { content: content(result), details: result.log ? { log: result.log } : undefined };
    },
  });
  pi.registerTool({
    name: "date", label: "Date", parameters: dateSchema,
    description: "The date and time of message id.",
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false },
    async execute(_id, args) {
      return { content: content(await memory("date", args)), details: undefined };
    },
  });
}

/* Extra image fields would fail the engine's record validation. */
export function imageBlocks(content) {
  if (!Array.isArray(content)) return undefined;
  return content.filter(block => block?.type === "image")
    .map(({ type, data, mimeType }) => ({ type, data, mimeType }));
}

/* Four-line cache blocks must preserve the rendered view's bytes. */
export function chatBlocks(text) {
  if (!text.startsWith("<chat>\n")) return [{ type: "text", text }];
  const boundary = text.indexOf("\n</chat>\n", 6);
  if (boundary < 0) return [{ type: "text", text }];
  const end = boundary + 1;
  const lines = text.slice(7, end).split("\n");
  if (lines.at(-1) === "") lines.pop();
  const blocks = [];
  const whole = lines.length - lines.length % 4;
  let head = "<chat>\n";
  for (let i = 0; i < whole; i += 4) {
    blocks.push({ type: "text", text: head + lines.slice(i, i + 4).join("\n") + "\n" });
    head = "";
  }
  const tail = lines.slice(whole).map(line => line + "\n").join("");
  blocks.push({ type: "text", text: head + tail + text.slice(end) });
  return blocks;
}

export function stableContext(messages, system, tools) {
  const initial = messages.find(m => m.role === "system");
  return [
    { role: "system", content: system, toolsAdded: tools || initial?.toolsAdded || [], timestamp: 0 },
    ...messages.filter(m => m.role !== "system").map(m => {
      if (m.role !== "user") return m;
      if (typeof m.content === "string") return { ...m, content: chatBlocks(m.content) };
      if (!Array.isArray(m.content) || m.content[0]?.type !== "text") return m;
      return { ...m, content: [...chatBlocks(m.content[0].text), ...m.content.slice(1)] };
    }),
  ];
}

/* Retain the previous turn's breakpoint while unchanged: a long turn can
 * push the new breakpoint beyond Anthropic's cache lookback. */
export function cachePayload(payload, providerTools, memory) {
  if (!payload || !Array.isArray(payload.messages) || !Array.isArray(payload.system) || !("max_tokens" in payload)) return { payload };
  if (providerTools) payload.tools = structuredClone(providerTools);
  const clean = blocks => {
    if (!Array.isArray(blocks)) return;
    for (const block of blocks) {
      if (!block || typeof block !== "object") continue;
      delete block.cache_control;
      if (Array.isArray(block.content)) clean(block.content);
    }
  };
  clean(payload.system);
  clean(payload.tools);
  for (const message of payload.messages) clean(message.content);
  const base = payload.system.at(-1);
  if (base) base.cache_control = { type: "ephemeral" };
  let mark, prefix;
  for (let mi = 0; mi < payload.messages.length; mi++) {
    const message = payload.messages[mi];
    if (!Array.isArray(message.content)) continue;
    let inChat = false, foundChat = false;
    for (let bi = 0; bi < message.content.length; bi++) {
      const block = message.content[bi];
      if (block.type !== "text") continue;
      if (block.text.startsWith("<chat>\n")) inChat = foundChat = true;
      if (inChat && /(^|\n)<\/chat>\n/.test(block.text)) break;
      if (inChat) {
        mark = block;
        prefix = [mi, bi];
      }
    }
    if (foundChat) break;
  }
  if (mark) mark.cache_control = { type: "ephemeral" };
  if (memory && mark) {
    const old = memory.at && payload.messages[memory.at[0]]?.content?.[memory.at[1]];
    const before = memory.at && (memory.at[0] < prefix[0] || (memory.at[0] === prefix[0] && memory.at[1] < prefix[1]));
    if (before && old?.type === "text" && old.text === memory.text) old.cache_control = { type: "ephemeral" };
    memory.at = prefix;
    memory.text = mark.text;
  }
  const last = payload.messages.at(-1);
  if (last && typeof last.content === "string") last.content = [{ type: "text", text: last.content }];
  const final = last?.content?.at(-1);
  if (final && final.type !== "thinking" && final.type !== "redacted_thinking") final.cache_control = { type: "ephemeral" };
  if (!prefix && !base) return { payload };
  const messages = prefix ? payload.messages.slice(0, prefix[0] + 1).map((m, i) =>
    i === prefix[0] ? { ...m, content: m.content.slice(0, prefix[1] + 1) } : m) : [];
  const key = createHash("sha256").update(JSON.stringify({ model: payload.model, tools: payload.tools, system: payload.system, messages })).digest("hex");
  return { payload, key };
}

/* JSON.stringify escapes lone surrogates, but the engine rejects them. */
const loneSurrogate = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/g;
export function wellFormed(text) {
  return typeof text.toWellFormed === "function" ? text.toWellFormed() : text.replace(loneSurrogate, "�");
}
const wellFormedStrings = (_key, value) => typeof value === "string" ? wellFormed(value) : value;

/* Call ids restart with each engine. Bind requests to that engine's Records,
 * not the adapter's current backend, so late results cannot cross a restart.
 * perform(event, signal, send) returns text or a protocol result object. */
export class Calls {
  constructor(records, perform) {
    this.records = records;
    this.perform = perform;
    this.live = new Map();
    this.closed = false;
  }
  async run(event) {
    if (this.closed) return;
    const controller = new AbortController();
    this.live.set(event.call, controller);
    let outcome;
    try {
      const result = await this.perform(event, controller.signal, data => this.records.request(data));
      outcome = typeof result === "string" ? { text: result } : result;
    } catch (error) {
      outcome = { error: error?.message || String(error) };
    } finally {
      if (this.live.get(event.call) === controller) this.live.delete(event.call);
    }
    /* Cancelled calls and closed engines can reject a late result. */
    await this.records.request({ op: "result", call: event.call, ...outcome }).catch(() => {});
  }
  cancel(call) { this.live.get(call)?.abort(); }
  close() {
    this.closed = true;
    for (const controller of this.live.values()) controller.abort();
  }
}

export class Records {
  constructor(input, output, notify = () => {}, onFailure = () => {}) {
    this.output = output;
    this.pending = new Map();
    this.serial = 0;
    this.buffer = Buffer.alloc(0);
    this.closed = false;
    this.onFailure = onFailure;
    input.on("data", chunk => {
      this.buffer = Buffer.concat([this.buffer, Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk)]);
      let end;
      while ((end = this.buffer.indexOf(10)) >= 0) {
        const bytes = this.buffer.subarray(0, end);
        this.buffer = this.buffer.subarray(end + 1);
        try {
          const record = JSON.parse(bytes.toString("utf8"));
          if (record.event) { notify(record); continue; }
          if (record.fatal || ((record.ok === false || record.error) && record.id == null)) {
            this.fail(new Error(record.error || "UniiChat protocol error"));
            return;
          }
          const waiter = this.pending.get(String(record.id));
          if (waiter) {
            this.pending.delete(String(record.id));
            if (record.ok === false || record.error) waiter.reject(new Error(record.error || "UniiChat backend error"));
            else waiter.resolve(record);
          }
        } catch (error) { this.fail(error); }
      }
    });
    input.on("error", error => this.fail(error));
    input.on("end", () => this.fail(new Error("UniiChat backend closed")));
    output.on("error", error => this.fail(error));
  }
  fail(error) {
    if (this.closed) return;
    this.closed = true;
    for (const waiter of this.pending.values()) waiter.reject(error);
    this.pending.clear();
    this.onFailure(error);
  }
  request(data) {
    if (this.closed) return Promise.reject(new Error("UniiChat backend unavailable"));
    const id = String(++this.serial);
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.output.write(JSON.stringify({ ...data, id }, wellFormedStrings) + "\n", error => {
        if (error) { this.pending.delete(id); reject(error); }
      });
    });
  }
}
