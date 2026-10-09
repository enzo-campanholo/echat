/* Offline provider; a FIFO holds its first reply while the client steers. */
import { appendFileSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { createAssistantMessageEventStream } from "@earendil-works/pi-ai";

export default function (pi: any) {
  let calls = 0;
  pi.registerProvider("unii-steering", {
    api: "unii-steering-api", baseUrl: "https://offline.invalid", apiKey: "offline-test",
    models: [{
      id: "deterministic", name: "Offline steering regression", reasoning: false,
      input: ["text"], contextWindow: 200000, maxTokens: 4096,
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
          await options?.onPayload?.({ model: model.id, messages: context.messages }, model);
          appendFileSync(process.env.UNII_TEST_PROVIDER_LOG!, JSON.stringify({ call, messages: context.messages }) + "\n");
          await options?.onResponse?.({
            url: "https://offline.invalid", status: 200, statusText: "OK", headers: new Headers(),
          }, model);
          stream.push({ type: "start", partial: output });
          if (!call) await readFile(process.env.UNII_TEST_REPLY_GATE!);
          if (options?.signal?.aborted) throw new Error("aborted");
          const text = "STEERING_REPLY_" + call;
          output.content.push({ type: "text", text: "" });
          stream.push({ type: "text_start", contentIndex: 0, partial: output });
          output.content[0].text = text;
          stream.push({ type: "text_delta", contentIndex: 0, delta: text, partial: output });
          stream.push({ type: "text_end", contentIndex: 0, content: text, partial: output });
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
