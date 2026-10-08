import { Socket } from "node:net";
import { Records, registerMemory, stableContext, cachePayload } from "./common.mjs";

export default function (pi) {
  const system = process.env.UNII_SYSTEM_PROMPT;
  if (!system) throw new Error("UniiChat worker bridge needs UNII_SYSTEM_PROMPT");
  const tools = process.env.UNII_TOOLS ? JSON.parse(process.env.UNII_TOOLS) : undefined;
  const providerTools = process.env.UNII_PROVIDER_TOOLS ? JSON.parse(process.env.UNII_PROVIDER_TOOLS) : undefined;
  let socket, records;
  pi.on("session_start", () => {
    if (!records) {
      socket = new Socket({ fd: Number(process.env.UNII_BRIDGE_FD || 3), readable: true, writable: true });
      records = new Records(socket, socket);
    }
  });
  /* The descriptor belongs to this process, across fresh Pi sessions. */
  registerMemory(pi, async (name, args) => await records.request({ type: "memory", name, args }));
  pi.on("before_agent_start", () => ({ systemPrompt: system }));
  pi.on("context_with_system", event => ({ messages: stableContext(event.messages, system, tools) }));
  pi.on("cache_warming_decision", () => ({ action: "stop" }));
  pi.on("tool_call", () => process.env.UNII_COMPACTION === "1" ? { block: true, reason: "Compactions must call no tools", terminate: true } : undefined);
  pi.on("before_provider_request", async event => {
    const { payload, key } = cachePayload(event.payload, providerTools);
    if (key && process.env.UNII_CACHE_GATE === "1") await records.request({ type: "prefix", key });
    return payload;
  });
  pi.on("after_provider_response", () => { records.send({ type: "response_started" }); });
}
