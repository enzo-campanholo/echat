/* Exercise real worker sessions and Anthropic cache barriers without an API. */
import { appendFileSync } from "node:fs";
import {
  createAssistantMessageEventStream,
  getCurrentSystemPrompt,
  getCurrentTools,
} from "@earendil-works/pi-ai";

export default function (pi: any) {
  pi.registerProvider("unii-worker-test", {
    api: "unii-worker-test", baseUrl: "https://offline.invalid", apiKey: "offline",
    models: [{
      id: "deterministic", name: "Offline worker test", reasoning: true,
      input: ["text"], contextWindow: 200000, maxTokens: 4096,
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
    }],
    streamSimple(model: any, context: any, options: any) {
      const stream = createAssistantMessageEventStream();
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
          let payload: any = {
            model: model.id, max_tokens: 4096,
            system: [{ type: "text", text: getCurrentSystemPrompt(context.messages) }],
            tools: getCurrentTools(context.messages).map((tool: any) => ({
              name: tool.name, description: tool.description, input_schema: tool.parameters,
            })),
            messages: context.messages.filter((message: any) => message.role !== "system"),
          };
          payload = await options?.onPayload?.(payload, model) ?? payload;
          appendFileSync(process.env.UNII_TEST_WORKER_LOG!, JSON.stringify({
            pid: process.pid, context: context.messages, payload,
          }) + "\n");
          if (options?.signal?.aborted) throw new Error("aborted");
          await options?.onResponse?.({
            url: "https://offline.invalid", status: 200, statusText: "OK", headers: new Headers(),
          }, model);
          stream.push({ type: "start", partial: output });
          output.content.push({ type: "text", text: "" });
          stream.push({ type: "text_start", contentIndex: 0, partial: output });
          const text = "note: deterministic worker summary " + "detail ".repeat(52);
          output.content[0].text = text;
          stream.push({ type: "text_delta", contentIndex: 0, delta: text, partial: output });
          stream.push({ type: "text_end", contentIndex: 0, content: text, partial: output });
          output.stopReason = "stop";
          stream.push({ type: "done", reason: "stop", message: output });
          stream.end();
        } catch (error) {
          output.stopReason = "error";
          output.errorMessage = String(error);
          stream.push({ type: "error", reason: "error", error: output });
          stream.end();
        }
      })();
      return stream;
    },
  });
}
