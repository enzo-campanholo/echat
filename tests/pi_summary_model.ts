/* Offline summary model for UniiChat's direct compaction calls. Behavior is
 * chosen by environment so every live test can share one provider. */
import { appendFileSync, closeSync, openSync, readFileSync, writeFileSync } from "node:fs";
import { readFile } from "node:fs/promises";
import {
  createAssistantMessageEventStream,
  getCurrentSystemPrompt,
  getCurrentTools,
} from "@earendil-works/pi-ai";

export default function (pi: any) {
  let calls = 0;
  pi.registerProvider("unii-summary", {
    api: "unii-summary-api", baseUrl: "https://offline.invalid", apiKey: "offline-test",
    models: [{
      id: "deterministic", name: "Offline summary model", reasoning: true,
      input: ["text", "image"], contextWindow: 200000, maxTokens: 4096,
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
    }],
    streamSimple(model: any, context: any, options: any) {
      const stream = createAssistantMessageEventStream();
      const call = calls++;
      const output: any = {
        role: "assistant", api: model.api, provider: model.provider,
        model: model.id, timestamp: Date.now(), content: [], stopReason: "pending",
        usage: {
          input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
          cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
        },
      };
      (async () => {
        try {
          const system = getCurrentSystemPrompt(context.messages);
          const tools = getCurrentTools(context.messages);
          const conversation = context.messages.filter((message: any) => message.role !== "system");
          let payload: any = {
            model: model.id, max_tokens: 4096,
            system: [{ type: "text", text: system }],
            tools: tools.map((tool: any) => ({
              name: tool.name, description: tool.description, input_schema: tool.parameters,
            })),
            messages: conversation,
          };
          payload = await options?.onPayload?.(payload, model) ?? payload;
          if (process.env.UNII_TEST_COMPACTION_LOG)
            appendFileSync(process.env.UNII_TEST_COMPACTION_LOG, JSON.stringify({
              call, system, tools, provider_tools: payload.tools, context: context.messages,
              payload, reasoning: options?.reasoning,
            }) + "\n");
          const first = conversation[0];
          const firstText = first.content.filter((block: any) => block.type === "text").map((block: any) => block.text).join("");
          const images = first.content.filter((block: any) => block.type === "image");
          if (process.env.UNII_TEST_COMPACTION_IMAGES && images.length && conversation.length === 1)
            appendFileSync(process.env.UNII_TEST_COMPACTION_IMAGES, JSON.stringify({ message: firstText, images }) + "\n");
          if (!call && process.env.UNII_TEST_COMPACTION_STARTED) {
            writeFileSync(process.env.UNII_TEST_COMPACTION_STARTED, "1");
            await readFile(process.env.UNII_TEST_COMPACTION_GATE!);
          }
          let fail = false;
          if (process.env.UNII_TEST_FAIL_ONCE) {
            try { closeSync(openSync(process.env.UNII_TEST_FAIL_ONCE, "wx")); fail = true; } catch {}
          }
          if (process.env.UNII_TEST_FAIL_MARKER) {
            try { readFileSync(process.env.UNII_TEST_FAIL_MARKER); fail = true; } catch {}
          }
          if (options?.signal?.aborted) throw new Error("aborted");
          await options?.onResponse?.({
            url: "https://offline.invalid", status: 200, statusText: "OK", headers: new Headers(),
          }, model);
          if (fail) throw new Error("test transient failure");
          const last = conversation[conversation.length - 1];
          const lastText = last.content.filter((block: any) => block.type === "text").map((block: any) => block.text).join("");
          const text = process.env.UNII_TEST_SUMMARY_SHORT || lastText.startsWith("Too long:")
            ? "note: deterministic summary " + "detail ".repeat(52)
            : "x".repeat(600);
          stream.push({ type: "start", partial: output });
          output.content.push({ type: "thinking", thinking: "never log this thought" });
          output.content.push({ type: "text", text });
          stream.push({ type: "text_start", contentIndex: 1, partial: output });
          stream.push({ type: "text_delta", contentIndex: 1, delta: text, partial: output });
          stream.push({ type: "text_end", contentIndex: 1, content: text, partial: output });
          output.stopReason = "stop";
          stream.push({ type: "done", reason: "stop", message: output });
        } catch (error) {
          output.stopReason = options?.signal?.aborted ? "aborted" : "error";
          output.errorMessage = String(error);
          stream.push({ type: "error", reason: output.stopReason, error: output });
        }
        stream.end();
      })();
      return stream;
    },
  });
}
