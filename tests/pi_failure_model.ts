/* Actual Pi hooks and tool execution, with an entirely local provider. */
import { appendFileSync } from "node:fs";
import { createAssistantMessageEventStream } from "@earendil-works/pi-ai";

export default function (pi: any) {
  let calls = 0;
  pi.registerTool({
    name: "probe", label: "Probe", description: "Record test tool execution",
    parameters: { type: "object", properties: {}, additionalProperties: false },
    async execute() {
      appendFileSync(process.env.UNII_TEST_PROBE!, "executed\n");
      return { content: [{ type: "text", text: "probe completed" }] };
    },
  });
  pi.registerProvider("unii-failure", {
    api: "unii-failure-api", baseUrl: "https://offline.invalid", apiKey: "offline-test",
    models: [{
      id: "deterministic", name: "Offline failure regression", reasoning: false,
      input: ["text"], contextWindow: 200000, maxTokens: 4096,
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
    }],
    streamSimple(model: any, context: any, options: any) {
      const stream = createAssistantMessageEventStream();
      const call = calls++;
      appendFileSync(process.env.UNII_TEST_PROVIDER_LOG!, JSON.stringify({ call, messages: context.messages }) + "\n");
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
          if (options?.signal?.aborted) throw new Error("aborted");
          await options?.onResponse?.({
            url: "https://offline.invalid", status: 200, statusText: "OK", headers: new Headers(),
          }, model);
          if (options?.signal?.aborted) throw new Error("aborted");
          stream.push({ type: "start", partial: output });
          output.content.push({ type: "text", text: "" });
          stream.push({ type: "text_start", contentIndex: 0, partial: output });
          const text = call ? "RETRY_FINISHED" : "BEFORE_TOOL_EXECUTION";
          output.content[0].text = text;
          stream.push({ type: "text_delta", contentIndex: 0, delta: text, partial: output });
          stream.push({ type: "text_end", contentIndex: 0, content: text, partial: output });
          if (!call) {
            const toolCall = { type: "toolCall", id: "failure-probe", name: "probe", arguments: {} };
            output.content.push(toolCall);
            stream.push({ type: "toolcall_start", contentIndex: 1, partial: output });
            stream.push({ type: "toolcall_delta", contentIndex: 1, delta: "{}", partial: output });
            stream.push({ type: "toolcall_end", contentIndex: 1, toolCall, partial: output });
          }
          output.stopReason = call ? "stop" : "toolUse";
          stream.push({ type: "done", reason: output.stopReason, message: output });
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
