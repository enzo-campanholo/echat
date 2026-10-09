/* The Pi adapter is the JavaScript boundary; UniiChat's engine is POSIX C89. */
import { spawn, execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { createLocalBashOperations, SettingsManager } from "@earendil-works/pi-coding-agent";
import { Records, registerMemory, stableContext, chatBlocks, cachePayload } from "./common.mjs";

const exec = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));

function textContent(content: any): string {
  if (typeof content === "string") return content;
  return (content || []).filter((block: any) => block.type === "text").map((block: any) => block.text).join("\n");
}

function clip(text: string): string {
  const chars = Array.from(text);
  if (chars.length <= 30000) return text;
  const marker = "\n[... clipped ...]\n";
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
  pi.registerFlag("unii-model", { type: "string", description: "Compaction model (default gpt-6-luna)" });
  pi.registerFlag("unii-provider", { type: "string", description: "Compaction provider (default openai-codex)" });
  pi.registerFlag("unii-effort", { type: "string", description: "Compaction reasoning effort (default xhigh)" });
  let child: any, backend: any, ctx: any, startup: Promise<void> | undefined;
  let exited: Promise<void> | undefined;
  let baseSystem = "", system = "", view = "", configured = "";
  let fresh = false, cut = -1, running = false, lastPrefix: string | undefined;
  let turnState = "", anchor: number | undefined;
  let preparingInput: Promise<void> | undefined;
  let tools: any[] = [], providerTools: any[] | undefined;
  let shuttingDown = false;
  let failure: Error | undefined;
  const summaryErrors = new Set<string>();

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
  const configure = async (nextProviderTools?: any[]) => {
    if (nextProviderTools) providerTools = nextProviderTools;
    const active = pi.getActiveTools();
    const all = pi.getAllTools();
    tools = active.map((name: string) => all.find((tool: any) => tool.name === name)).filter(Boolean)
      .map((tool: any) => ({ name: tool.name, description: tool.description, parameters: tool.parameters }));
    const configuration: any = { system, tools, events: true };
    if (providerTools) configuration.provider_tools = providerTools;
    const signature = JSON.stringify(configuration);
    if (signature !== configured) {
      await request("configure", configuration);
      configured = signature;
    }
  };
  const launch = async () => {
    const candidates = [resolve(here, "../unii"), resolve(here, "../../../bin/unii")];
    const binary = String(pi.getFlag("unii-bin") || process.env.UNII_BIN || candidates.find(existsSync) || "unii");
    const directory = String(pi.getFlag("unii-dir") || process.env.UNII_DIR || join(process.env.XDG_STATE_HOME || join(homedir(), ".local", "state"), "uniichat"));
    const prompt = await exec(binary, ["prompt"], { maxBuffer: 1024 * 1024, env: { ...process.env, UNII_ASSETS: here } });
    baseSystem = prompt.stdout.trimEnd();
    if (process.env.UNII_INSTRUCTIONS) baseSystem += "\n\n" + await readFile(process.env.UNII_INSTRUCTIONS, "utf8");
    const instructions = ownInstructions(ctx.getSystemPrompt());
    if (instructions.length) baseSystem += "\n\n" + instructions.join("\n\n");
    system = baseSystem;
    const args = ["serve", directory, "--bridge", resolve(here, "bridge.mjs")];
    for (const [flag, variable, fallback] of [
      ["model", "UNII_MODEL", "gpt-6-luna"],
      ["provider", "UNII_PROVIDER", "openai-codex"],
      ["effort", "UNII_EFFORT", "xhigh"],
    ]) args.push("--" + flag, String(pi.getFlag("unii-" + flag) || process.env[variable] || fallback));
    if (process.env.UNII_PI) args.push("--pi", process.env.UNII_PI);
    child = spawn(binary, args, { stdio: ["pipe", "pipe", "inherit"] });
    exited = new Promise(resolveExit => {
      child.once("exit", () => resolveExit());
      child.once("error", () => resolveExit());
    });
    backend = new Records(child.stdout, child.stdin, (event: any) => {
      if (event.event !== "error") return;
      const reason = String(event.text).split(/\n|; stack=/, 1)[0];
      const message = "UniiChat summary failed (" + event.provider + "/" + event.model + "): " + reason;
      if (summaryErrors.has(message)) return;
      summaryErrors.add(message);
      const auth = /oauth|api key|credentials|authentication|unauthorized|\b40[13]\b/i.test(reason);
      const help = auth ? "Use /login for " + event.provider + ", then send a message to retry."
        : "Send a message to retry. /unii shows failed summaries.";
      if (ctx?.hasUI) ctx.ui.notify(message + "\n" + help, "error");
      else console.error(message + "\n" + help);
    }, (error: Error) => { if (!shuttingDown) stop(error); });
    child.on("error", (error: Error) => backend.fail(error));
    child.on("exit", (code: number, signal: string) => {
      backend.fail(new Error("UniiChat backend exited (" + (signal || code) + ")"));
      if (!shuttingDown && ctx?.hasUI) ctx.ui.notify("UniiChat stopped; the permanent log is intact", "error");
    });
    await configure();
    if (ctx?.hasUI) ctx.ui.setStatus("unii", "UniiChat · " + directory);
  };

  pi.on("session_start", async (_event: any, context: any) => {
    ctx = context;
    if (shuttingDown) {
      startup = undefined; backend = undefined; configured = "";
      providerTools = undefined; shuttingDown = false; running = false;
      failure = undefined;
      summaryErrors.clear();
    }
    if (!startup) startup = launch().catch(stop);
    await startup;
  });
  pi.on("session_shutdown", async () => {
    shuttingDown = true;
    try {
      if (backend && !backend.closed) await request("shutdown").catch(() => {});
    } finally {
      child?.stdin.end();
      await exited;
    }
  });
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
      await request("append", { kind: "user", text: event.command });
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
  pi.on("input", async (event: any, context: any) => {
    ctx = context;
    await startup;
    if (!backend || backend.closed) return { action: "handled" };
    let preparing = false;
    let releaseInput: (() => void) | undefined;
    try {
      if (running || !context.isIdle()) {
        await preparingInput;
        if (failure) return { action: "handled" };
        await request("steer", { text: event.text, images: event.images });
      } else {
        failure = undefined;
        summaryErrors.clear();
        preparing = true;
        running = true;
        fresh = true;
        cut = -1;
        anchor = undefined;
        preparingInput = new Promise(resolveInput => { releaseInput = resolveInput; });
        await configure();
        const turn = request("turn", { text: event.text, images: event.images });
        /* Later input must follow the initiating record, while still reaching
         * the backend during its wait for preceding summaries. */
        releaseInput!();
        preparingInput = undefined;
        const response = await turn;
        view = response.view;
        if (response.system) system = response.system;
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
    current[0] = { ...first, content: [...chatBlocks(view + "\n\n"), { type: "text", text: turnState }, ...content] };
    return { messages: stableContext(current, system, tools) };
  }));
  pi.on("before_provider_request", guard(async (event: any) => {
    const { payload, key } = cachePayload(event.payload);
    if (Array.isArray(payload?.system) && Array.isArray(payload?.tools)) await configure(payload.tools);
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
      for (const block of message.content) {
        if (block.type === "text" && block.text) await request("append", { kind: "unii", text: block.text });
        else if (block.type === "toolCall") await request("append", { kind: "tool", text: block.name + " " + JSON.stringify(block.arguments) });
      }
    } else if (message.role === "toolResult") {
      await request("append", { kind: "echo", text: textContent(message.content),
        images: message.content.filter((block: any) => block.type === "image") });
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
      images: event.result?.content?.filter((block: any) => block.type === "image") });
  }));
  pi.on("agent_settled", async () => {
    running = false;
    if (lastPrefix && !failure) await request("cache_started", { key: lastPrefix }).catch(stop);
    lastPrefix = undefined;
  });

  pi.registerCommand("unii", {
    description: "UniiChat status, flush, or import FILE as a memory note",
    handler: async (args: string, context: any) => {
      ctx = context;
      await startup;
      const command = args.trim();
      let response;
      if (command.startsWith("import ")) response = await request("append", { kind: "note", text: await readFile(resolve(command.slice(7).trim()), "utf8") });
      else if (command === "flush") response = await request("flush");
      else response = await request("status");
      if (context.hasUI) context.ui.notify(JSON.stringify(response), "info");
    },
  });
}
