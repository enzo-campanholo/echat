/* The Pi adapter is the JavaScript boundary; UniiChat's engine is POSIX C89. */
import { spawn, execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import { existsSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { createLocalBashOperations, SettingsManager } from "@earendil-works/pi-coding-agent";
import { Calls, Records, registerMemory, stableContext, chatBlocks, cachePayload, imageBlocks } from "./common.mjs";

const exec = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));

function textContent(content: any): string {
  if (typeof content === "string") return content;
  return (content || []).filter((block: any) => block.type === "text").map((block: any) => block.text).join("\n");
}

function clip(text: string): string {
  const chars = Array.from(text);
  if (chars.length <= 30000) return text;
  const marker = "\n[... output clipped ...]\n"; /* the engine's marker */
  const head = Math.floor((30000 - marker.length) / 2);
  const tail = 30000 - marker.length - head;
  return chars.slice(0, head).join("") + marker + chars.slice(-tail).join("");
}

/* Match Pi's native bash normalization, including ANSI split across chunks. */
const ansiEnd = "(?:\\u0007|\\u001B\\u005C|\\u009C)";
const ansiOsc = "\\u001B\\]";
const ansiCsi = "[\\u001B\\u009B][[\\]()#;?]*(?:\\d{1,4}(?:[;:]\\d{0,4})*)?";
const ansiCodes = new RegExp(`(?:${ansiOsc}[\\s\\S]*?${ansiEnd})|${ansiCsi}[\\dA-PR-TZcf-nq-uy=><~]`, "g");
const ansiPending = new RegExp(`(?:${ansiOsc}(?:[^\\u0007\\u009C\\u001B]|\\u001B(?!\\\\))*|${ansiCsi})$`);

/* Pi's structured prompt keeps user instructions separate from device state. */
function ownInstructions(prompt: string): string[] {
  const sections = [...prompt.matchAll(/<([A-Za-z_][A-Za-z0-9_]*)>\n([\s\S]*?)\n<\/\1>/g)];
  const head = prompt.slice(0, sections[0]?.index ?? prompt.length).trim();
  const own = head && !head.startsWith("You are an expert coding assistant operating inside pi,") ? [head] : [];
  for (const section of sections) {
    if (!["tools", "rules", "docs", "cwd"].includes(section[1])) own.push(section[0]);
  }
  return own;
}

export default function (pi: any) {
  pi.registerFlag("unii-dir", { type: "string", description: "Directory for the permanent UniiChat log" });
  pi.registerFlag("unii-bin", { type: "string", description: "Path to the UniiChat C executable" });
  pi.registerFlag("unii-model", { type: "string", description: "Summary model (or UNII_MODEL)" });
  pi.registerFlag("unii-provider", { type: "string", description: "Summary provider (or UNII_PROVIDER)" });
  pi.registerFlag("unii-effort", { type: "string", description: "Summary reasoning effort (or UNII_EFFORT)" });
  let child: any, backend: any, ctx: any, startup: Promise<void> | undefined;
  let exited: Promise<void> | undefined;
  let system = "", view = "", recent = "", directory = "";
  let fresh = false, cut = -1, running = false, lastPrefix: string | undefined;
  let turnState = "", anchor: number | undefined;
  const turnMark: any = {}; /* the previous turn's last whole view block */
  let preparingInput: Promise<void> | undefined;
  let tools: any[] = [], providerTools: any[] | undefined;
  let shuttingDown = false;
  let failure: Error | undefined;
  const summaryErrors = new Set<string>();
  let summaries: any; /* the running engine's summary calls */
  const summarySession = "uniichat-" + randomUUID();

  const stop = (error: any, context = ctx) => {
    failure = error instanceof Error ? error : new Error(String(error));
    context?.abort();
    if (context?.hasUI) context.ui.notify("UniiChat stopped: " + failure.message, "error");
    else console.error("UniiChat stopped: " + failure.message);
  };
  /* Pi reports thrown event handlers and continues. Abort explicitly whenever
   * the memory boundary fails, including before executing a tool batch. */
  const guard = (handler: any) => async (event: any, context: any) => {
    ctx = context;
    if (failure) { context.abort(); return; }
    try { return await handler(event, context); }
    catch (error) { stop(error, context); }
  };

  const request = (op: string, data: any = {}) => {
    if (!backend) return Promise.reject(new Error("UniiChat backend is not running"));
    return backend.request({ op, ...data });
  };
  /* Turns and summaries share the active tools (never called by summaries). */
  const refreshTools = (nextProviderTools?: any[]) => {
    if (nextProviderTools) providerTools = nextProviderTools;
    const active = pi.getActiveTools();
    const all = pi.getAllTools();
    tools = active.map((name: string) => all.find((tool: any) => tool.name === name)).filter(Boolean)
      .map((tool: any) => ({ name: tool.name, description: tool.description, parameters: tool.parameters }));
  };
  const report = (event: any) => {
    const reason = String(event.text).split(/\n|; stack=/, 1)[0];
    const message = "UniiChat summary failed (" + event.provider + "/" + event.model + "): " + reason;
    if (summaryErrors.has(message)) return;
    summaryErrors.add(message);
    const auth = /oauth|api key|credentials|authentication|unauthorized|\b40[13]\b/i.test(reason);
    const help = auth ? "Use /login for " + event.provider + ", then send a message to retry."
      : "Send a message to retry. /unii shows failed summaries.";
    if (ctx?.hasUI) ctx.ui.notify(message + "\n" + help, "error");
    else console.error(message + "\n" + help);
  };
  /* The engine decides what to ask, when, and how to correct it; this only
   * performs one plain completion (no tool execution). Calls (common.mjs)
   * binds it, its cache gating (SEND) and its result to the engine that
   * asked. */
  const summarize = async (event: any, signal: AbortSignal, send: (data: any) => Promise<any>) => {
    let key: string | undefined;
    const model = ctx.modelRegistry.find(event.provider, event.model);
    if (!model) throw new Error("summary model " + event.provider + "/" + event.model + " is not available");
    if (!ctx.modelRegistry.hasConfiguredAuth(model)) throw new Error("no credentials for " + event.provider);
    const usage = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
    /* The view goes in four-line blocks, so cache marks land as in turns. */
    const messages = event.messages.map((m: any) => m.role === "user"
      ? { role: "user", content: [...chatBlocks(m.text), ...(m.images || [])], timestamp: Date.now() }
      : { role: "assistant", content: [{ type: "text", text: m.text }], api: model.api, provider: model.provider,
          model: model.id, usage, stopReason: "stop", timestamp: Date.now() });
    const reply = await ctx.modelRegistry.streamSimple(model, { systemPrompt: system, messages, tools }, {
      reasoning: event.effort, signal, sessionId: summarySession,
      onPayload: async (payload: any) => {
        const marked = cachePayload(payload, providerTools);
        key = marked.key;
        if (key) await send({ op: "cache_wait", key, call: event.call });
        return marked.payload;
      },
      onResponse: async (response: any) => {
        /* An error response wrote no cache entry; the call's end releases it. */
        if (key && !(response?.status >= 400)) await send({ op: "cache_started", key, call: event.call });
        key = undefined;
      },
    }).result();
    if (reply.stopReason === "error" || reply.stopReason === "aborted")
      throw new Error(reply.errorMessage || "summary request " + reply.stopReason);
    if (reply.content.some((block: any) => block.type === "toolCall"))
      throw new Error("compaction attempted to call a tool");
    return reply.content.filter((block: any) => block.type === "text").map((block: any) => block.text).join("\n");
  };
  /* Memory progress replaces the status line while a turn or flush waits. */
  const idle = () => { if (ctx?.hasUI && directory) ctx.ui.setStatus("unii", "UniiChat · " + directory); };
  const waiting = (count: number) => {
    if (!ctx?.hasUI) return;
    ctx.ui.setStatus("unii", count ? "UniiChat · summarizing " + count + " earlier message" + (count === 1 ? "" : "s")
      : "UniiChat · merging older summaries");
  };
  const onEvent = (calls: any, event: any) => {
    if (event.event === "call") calls.run(event);
    else if (event.event === "cancel") calls.cancel(event.call);
    else if (event.event === "error") report(event);
    else if (event.event === "progress") waiting(event.waiting);
  };
  const launch = async () => {
    const candidates = [resolve(here, "../unii"), resolve(here, "../../../bin/unii")];
    const binary = String(pi.getFlag("unii-bin") || process.env.UNII_BIN || candidates.find(existsSync) || "unii");
    directory = String(pi.getFlag("unii-dir") || process.env.UNII_DIR || join(process.env.XDG_STATE_HOME || join(homedir(), ".local", "state"), "uniichat"));
    const prompt = await exec(binary, ["prompt"], { maxBuffer: 1024 * 1024 });
    system = prompt.stdout.trimEnd();
    if (process.env.UNII_INSTRUCTIONS) system += "\n\n" + await readFile(process.env.UNII_INSTRUCTIONS, "utf8");
    const instructions = ownInstructions(ctx.getSystemPrompt());
    if (instructions.length) system += "\n\n" + instructions.join("\n\n");
    /* The engine owns the summary defaults and reads UNII_MODEL, UNII_PROVIDER,
     * UNII_EFFORT and UNII_TIMEOUT itself; only pi's flags are passed on. */
    const args = ["serve", directory];
    for (const flag of ["model", "provider", "effort"]) {
      const value = pi.getFlag("unii-" + flag);
      if (value) args.push("--" + flag, String(value));
    }
    const proc = spawn(binary, args, { stdio: ["pipe", "pipe", "inherit"] });
    /* This engine's calls answer only to it, even after a restart. */
    let calls: any;
    const records = new Records(proc.stdout, proc.stdin, (event: any) => onEvent(calls, event), (error: Error) => {
      calls.close();
      if (!shuttingDown && backend === records) stop(error);
    });
    calls = new Calls(records, summarize);
    child = proc;
    backend = records;
    summaries = calls;
    exited = new Promise(resolveExit => {
      proc.once("exit", () => resolveExit());
      proc.once("error", () => resolveExit());
    });
    proc.on("error", (error: Error) => records.fail(error));
    proc.on("exit", (code: number, signal: string) => {
      records.fail(new Error("UniiChat backend exited (" + (signal || code) + ")"));
      if (!shuttingDown && backend === records && ctx?.hasUI)
        ctx.ui.notify("UniiChat stopped; the permanent log is intact. /unii restart relaunches it.", "error");
    });
    refreshTools();
    const hello = await request("hello", { protocol: 1 });
    if (hello.protocol !== 1) throw new Error("UniiChat backend speaks protocol " + hello.protocol);
    idle();
  };
  const shutdown = async () => {
    shuttingDown = true;
    summaries?.close();
    try {
      if (backend && !backend.closed) await request("shutdown").catch(() => {});
    } finally {
      child?.stdin.end();
      await exited;
    }
  };
  const reset = () => {
    startup = undefined; backend = undefined; child = undefined; summaries = undefined;
    providerTools = undefined; shuttingDown = false; running = false;
    failure = undefined;
    summaryErrors.clear();
  };

  pi.on("session_start", async (_event: any, context: any) => {
    ctx = context;
    if (shuttingDown) reset();
    if (!startup) startup = launch().catch(stop);
    await startup;
  });
  pi.on("session_shutdown", shutdown);
  pi.on("cache_warming_decision", () => ({ action: "stop" }));
  pi.on("session_before_compact", () => ({ cancel: true }));

  pi.on("user_bash", async (event: any, context: any) => {
    if (event.excludeFromContext) return;
    ctx = context;
    try {
      await startup;
      await preparingInput;
      if (!backend || backend.closed) throw new Error("UniiChat backend is not running");
      if (!running && context.isIdle()) { failure = undefined; summaryErrors.clear(); }
      if (failure) throw failure;
      /* Logged as typed, so summaries read a command, not chat words. */
      await request("append", { kind: "user", text: "!" + event.command });
    } catch (error) {
      stop(error, context);
      throw error; /* user_bash errors block execution instead of falling through. */
    }
    const settings = SettingsManager.create(event.cwd, undefined, { projectTrusted: context.isProjectTrusted() });
    const local = createLocalBashOperations({ shellPath: settings.getShellPath() });
    return { operations: {
      async exec(command: string, cwd: string, options: any) {
        const decoder = new TextDecoder("utf-8", { ignoreBOM: true });
        let output = "", pending = "", result: any, executionError: any;
        const capture = (text: string, final = false) => {
          text = pending + text;
          const window = Math.max(0, text.length - 256);
          const unfinished = final ? null : ansiPending.exec(text.slice(window));
          const end = unfinished ? window + unfinished.index : text.length;
          pending = text.slice(end);
          text = text.slice(0, end).replace(ansiCodes, "")
            .replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\uFFF9-\uFFFB]/g, "").replace(/\r/g, "");
          output = clip(output + text);
        };
        try {
          result = await local.exec(command, cwd, { ...options, onData(data: Buffer) {
            capture(decoder.decode(data, { stream: true }));
            options.onData(data);
          } });
        } catch (error) { executionError = error; }
        capture(decoder.decode(), true);
        let status = "bash: exit=" + (result?.exitCode ?? "unknown") + " cancelled=" + Boolean(options.signal?.aborted);
        if (executionError) status += " error=" + JSON.stringify(String(executionError));
        try { await request("append", { kind: "echo", text: status + "\n" + output }); }
        catch (error) { stop(error, context); throw error; }
        if (executionError) throw executionError;
        return result;
      },
    } };
  });

  registerMemory(pi, async (name: string, args: any) => await request(name, { args }));
  pi.on("tool_call", () => failure ? { block: true, reason: failure.message, terminate: true } : undefined);
  /* Never drop input silently: over RPC this is a visible notify record. */
  const dropped = (context: any, why: string) => {
    const text = "UniiChat did not save this message: " + why + ". Use /unii restart, then resend.";
    if (context?.hasUI) context.ui.notify(text, "error");
    else console.error(text);
    return { action: "handled" };
  };
  pi.on("input", async (event: any, context: any) => {
    ctx = context;
    await startup;
    if (!backend || backend.closed) return dropped(context, "the backend is not running");
    let preparing = false;
    let releaseInput: (() => void) | undefined;
    try {
      if (running || !context.isIdle()) {
        await preparingInput;
        if (failure) return dropped(context, failure.message);
        await request("steer", { text: event.text, images: imageBlocks(event.images) });
      } else {
        failure = undefined;
        summaryErrors.clear();
        preparing = true;
        running = true;
        fresh = true;
        cut = -1;
        anchor = undefined;
        preparingInput = new Promise(resolveInput => { releaseInput = resolveInput; });
        refreshTools();
        const turn = request("turn", { text: event.text, images: imageBlocks(event.images) });
        /* Later input must follow the initiating record, while still reaching
         * the backend during its wait for preceding summaries. */
        releaseInput!();
        preparingInput = undefined;
        const response = await turn.finally(idle);
        view = response.view;
        recent = response.recent || "";
        turnState = "Current date: " + new Date().toISOString() + "\nWorking directory: " + context.cwd + "\n\n";
      }
      return { action: "continue" };
    } catch (error) {
      if (preparing) { running = false; fresh = false; cut = -1; anchor = undefined; }
      stop(error, context);
      releaseInput?.();
      if (preparing) preparingInput = undefined;
      return { action: "handled" };
    }
  });
  pi.on("message_start", guard((event: any, context: any) => {
    if (fresh && event.message.role === "user") {
      /* Pi emits the initiating user before draining queued steering. Capture
       * its position in the UI transcript before message_end persists it. */
      cut = context.sessionManager.buildSessionContext().messages
        .filter((message: any) => message.role !== "system").length;
      anchor = event.message.timestamp;
      fresh = false;
    }
  }));
  pi.on("before_agent_start", guard(() => ({ systemPrompt: system })));
  pi.on("context_with_system", guard((event: any) => {
    const conversation = event.messages.filter((message: any) => message.role !== "system");
    if (fresh || cut < 0 || conversation[cut]?.role !== "user" || conversation[cut]?.timestamp !== anchor)
      throw new Error("UniiChat received a model request without its initiating user message");
    const current = conversation.slice(cut);
    const first = current[0];
    const content = typeof first.content === "string" ? [{ type: "text", text: first.content }] : first.content;
    /* Unsummarized messages, whole, follow the view and precede the state. */
    const tail = recent ? [{ type: "text", text: recent + "\n" }] : [];
    current[0] = { ...first, content: [...chatBlocks(view + "\n\n"), ...tail, { type: "text", text: turnState }, ...content] };
    return { messages: stableContext(current, system, tools) };
  }));
  pi.on("before_provider_request", guard(async (event: any) => {
    const { payload, key } = cachePayload(event.payload, undefined, turnMark);
    if (Array.isArray(payload?.system) && Array.isArray(payload?.tools)) refreshTools(payload.tools);
    lastPrefix = key;
    if (key) await request("cache_wait", { key });
    return payload;
  }));
  pi.on("after_provider_response", guard(async () => {
    if (lastPrefix) await request("cache_started", { key: lastPrefix });
    lastPrefix = undefined;
  }));
  pi.on("message_end", guard(async (event: any) => {
    const message = event.message;
    if (message.role === "assistant") {
      /* One write for the message's replies and tool calls, in order. */
      const records = message.content.flatMap((block: any) =>
        block.type === "text" && block.text ? [{ kind: "unii", text: block.text }]
        : block.type === "toolCall" ? [{ kind: "tool", text: block.name + " " + JSON.stringify(block.arguments) }]
        : []);
      if (records.length) await request("append", { records });
    } else if (message.role === "toolResult" && message.toolName === "zoom" &&
               !message.isError && message.details?.log) {
      /* The tree already holds what zoom shows: log a reference, not a copy. */
      await request("append", { kind: "echo", text: message.details.log });
    } else if (message.role === "toolResult") {
      await request("append", { kind: "echo", text: textContent(message.content),
        images: imageBlocks(message.content) });
    }
  }));
  pi.on("tool_result", (event: any) => {
    const text = textContent(event.content);
    const clipped = clip(text);
    if (clipped === text) return;
    const content = [{ type: "text", text: clipped }, ...event.content.filter((block: any) => block.type !== "text")];
    return { content, structuredContent: event.structuredContent };
  });
  pi.on("tool_execution_start", guard(async (event: any) => {
    if (event.parentToolCallId) await request("append", { kind: "tool", text: event.toolName + " " + JSON.stringify(event.args) });
  }));
  pi.on("tool_execution_end", guard(async (event: any) => {
    if (event.parentToolCallId) await request("append", { kind: "echo", text: textContent(event.result?.content),
      images: imageBlocks(event.result?.content) });
  }));
  pi.on("agent_settled", async () => {
    running = false;
    if (lastPrefix && !failure) await request("cache_started", { key: lastPrefix }).catch(stop);
    lastPrefix = undefined;
  });

  pi.registerCommand("unii", {
    description: "UniiChat status, flush, retry, restart, or import FILE as a memory note",
    handler: async (args: string, context: any) => {
      ctx = context;
      await startup;
      const command = args.trim();
      let response;
      try {
        if (command === "restart") {
          /* Relaunch the engine without restarting pi; the log is untouched. */
          await shutdown();
          reset();
          startup = launch().catch(stop);
          await startup;
          response = failure ? { ok: false, error: failure.message } : { ok: true, restarted: directory };
        } else if (command.startsWith("import ")) {
          response = await request("append", { kind: "note", text: await readFile(resolve(context.cwd, command.slice(7).trim()), "utf8") });
        } else if (command === "flush") {
          response = await request("flush").finally(idle);
        } else if (command === "retry") {
          summaryErrors.clear();
          response = await request("retry");
        } else {
          response = await request("status");
        }
      } catch (error: any) {
        /* A refused command (a flush superseded by a turn, a missing file)
         * is a notice, not a thrown handler. A stopped engine reports itself. */
        const text = "UniiChat: " + (error?.message || String(error));
        if (context.hasUI) context.ui.notify(text, "error");
        else console.error(text);
        return;
      }
      if (context.hasUI) context.ui.notify(JSON.stringify(response), "info");
    },
  });
}
