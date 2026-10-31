/* Pi 1.4.0 flattens Responses refusals into text. Observe raw events per
 * completion, not through a session/global provider hook. */
const responsesAPIs = new Set(["openai-responses", "openai-codex-responses", "azure-openai-responses"]);
const refusalItem = item => item?.type === "message" &&
  Array.isArray(item.content) && item.content.some(part => part?.type === "refusal");
const toolItem = item => item?.type === "function_call" || item?.type === "custom_tool_call";

export function summaryOutcome() {
  let identity, refused, completed, failed;
  /* Tool evidence survives SDK transport restarts within this completion. */
  let toolUsed = false;
  const reset = () => {
    identity = undefined;
    refused = completed = failed = false;
  };
  reset();
  return {
    reset,
    observe(data, model) {
      if (!responsesAPIs.has(model?.api)) return;
      /* Codex can restart its transport inside one SDK call. Do not carry an
       * earlier response's refusal or failure into a newly created response. */
      if (data?.type === "response.created") reset();
      identity = [model.api, model.provider, model.id];
      if (data?.type === "error" || data?.type === "response.failed") failed = true;
      /* Raw tool evidence must fail closed even if normalization omits it. */
      if (((data?.type === "response.output_item.added" || data?.type === "response.output_item.done") &&
            toolItem(data.item)) ||
          data?.type === "response.function_call_arguments.delta" || data?.type === "response.function_call_arguments.done" ||
          data?.type === "response.custom_tool_call_input.delta" || data?.type === "response.custom_tool_call_input.done")
        toolUsed = true;
      if (data?.type === "response.refusal.delta" || data?.type === "response.refusal.done" ||
          ((data?.type === "response.content_part.added" || data?.type === "response.content_part.done") &&
            data.part?.type === "refusal") ||
          ((data?.type === "response.output_item.added" || data?.type === "response.output_item.done") &&
            refusalItem(data.item))) refused = true;
      if (data?.type === "response.completed" || data?.type === "response.incomplete" ||
          (model.api === "openai-codex-responses" && data?.type === "response.done")) {
        const response = data.response;
        completed = !response?.error && (response?.status === "completed" ||
          (response?.status === "incomplete" && response.incomplete_details?.reason === "max_output_tokens"));
        if (!completed) failed = true;
        if (Array.isArray(response?.output)) {
          if (response.output.some(refusalItem)) refused = true;
          if (response.output.some(toolItem)) toolUsed = true;
        }
      }
    },
    result(reply, terminal) {
      /* Pi 1.4.0 maps clean Anthropic refusals and late read/parse failures to
       * the same error result, even after message_stop. Raw refusal evidence
       * therefore cannot override the normalized terminal status. */
      if (terminal !== "done" || reply.stopReason === "error" || reply.stopReason === "aborted")
        throw new Error(reply.errorMessage || "summary request " + (reply.stopReason || "did not complete"));
      if (toolUsed || reply.content.some(block => block.type === "toolCall"))
        throw new Error("compaction attempted to call a tool");
      if (reply.stopReason !== "stop" && reply.stopReason !== "length")
        throw new Error("unexpected summary stop reason: " + reply.stopReason);
      if (responsesAPIs.has(reply.api)) {
        if (!identity || identity[0] !== reply.api || identity[1] !== reply.provider || identity[2] !== reply.model ||
            !completed || failed)
          throw new Error("summary Responses stream lacks a successful structured terminal response");
        if (refused) return { outcome: "refused" };
      }
      const text = reply.content.filter(block => block.type === "text").map(block => block.text).join("\n");
      return text.trim() ? { outcome: "ok", text } : { outcome: "empty" };
    },
  };
}
