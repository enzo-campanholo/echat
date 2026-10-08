/* An in-process provider for exercising the real Pi extension without an API. */
import { appendFileSync } from "node:fs";
import {
  createAssistantMessageEventStream,
  getCurrentSystemPrompt,
  getCurrentTools,
} from "@earendil-works/pi-ai";

export default function (pi: any) {
  let calls = 0;
  pi.registerProvider("unii-test", {
    api: "unii-test-api",
    baseUrl: "https://offline.invalid",
    apiKey: "in-process-test-key",
    models: [{
      id: "deterministic", name: "Deterministic offline test",
      reasoning: true, input: ["text", "image"],
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
      contextWindow: 200000, maxTokens: 4096,
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
      const emitText = (text: string) => {
        const contentIndex = output.content.length;
        output.content.push({ type: "text", text: "" });
        stream.push({ type: "text_start", contentIndex, partial: output });
        output.content[contentIndex].text = text;
        stream.push({ type: "text_delta", contentIndex, delta: text, partial: output });
        stream.push({ type: "text_end", contentIndex, content: text, partial: output });
      };
      const emitTool = (name: string, args: any) => {
        const contentIndex = output.content.length;
        const toolCall = { type: "toolCall", id: "live-" + call + "-" + name, name, arguments: {} };
        output.content.push(toolCall);
        stream.push({ type: "toolcall_start", contentIndex, partial: output });
        toolCall.arguments = args;
        stream.push({ type: "toolcall_delta", contentIndex, delta: JSON.stringify(args), partial: output });
        stream.push({ type: "toolcall_end", contentIndex, toolCall, partial: output });
      };
      (async () => {
        try {
          const system = getCurrentSystemPrompt(context.messages);
          const tools = getCurrentTools(context.messages);
          let payload: any = {
            model: model.id, max_tokens: 4096,
            system: [{ type: "text", text: system }],
            tools: tools.map((tool: any) => ({
              name: tool.name, description: tool.description, input_schema: tool.parameters,
            })),
            messages: context.messages.filter((message: any) => message.role !== "system"),
          };
          payload = await options?.onPayload?.(payload, model) ?? payload;
          appendFileSync(process.env.UNII_TEST_PROVIDER_LOG!, JSON.stringify({
            call, system, tools, context: context.messages, payload,
          }) + "\n");
          if (options?.signal?.aborted) throw new Error("aborted");
          await options?.onResponse?.({
            url: "https://offline.invalid", status: 200, statusText: "OK", headers: new Headers(),
          }, model);
          await options?.onProviderStreamEvent?.({ type: "offline_test", call }, model);
          stream.push({ type: "start", partial: output });
          if (call === 0) {
            output.content.push({ type: "thinking", thinking: "" });
            stream.push({ type: "thinking_start", contentIndex: 0, partial: output });
            output.content[0].thinking = "NEVER_LOG_LIVE_THINKING";
            stream.push({ type: "thinking_delta", contentIndex: 0, delta: "NEVER_LOG_LIVE_THINKING", partial: output });
            stream.push({ type: "thinking_end", contentIndex: 0, content: "NEVER_LOG_LIVE_THINKING", partial: output });
            emitText("TURN_ONE_TOOL_PLAN");
            emitTool("zoom", { id: 0, n: 1, page: 0 });
            emitTool("date", { id: 0 });
            emitTool("read", { path: process.env.UNII_TEST_READ });
            output.stopReason = "toolUse";
          } else if (call === 2) {
            emitTool("zoom", { id: 9, n: 1, page: 0 });
            output.stopReason = "toolUse";
          } else {
            emitText(call === 1 ? "TURN_ONE_FINAL_REPLY" : "TURN_TWO_FINAL_REPLY");
            output.stopReason = "stop";
          }
          stream.push({ type: "done", reason: output.stopReason, message: output });
          stream.end();
        } catch (error) {
          output.stopReason = options?.signal?.aborted ? "aborted" : "error";
          output.errorMessage = String(error);
          stream.push({ type: "error", reason: output.stopReason, error: output });
          stream.end();
        }
      })();
      return stream;
    },
  });
}
