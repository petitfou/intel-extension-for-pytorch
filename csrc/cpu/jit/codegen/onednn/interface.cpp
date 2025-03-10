#include "interface.h"
#include <oneapi/dnnl/dnnl_graph.hpp>
#include "defer_size_check.h"
#include "fusion_group_name.h"
#include "graph_fuser.h"
#include "guard_shape.h"
#include "kernel.h"
#include "layout_propagation.h"
#include "lift_up_quant.h"
#include "prepare_binary.h"
#include "prepare_dequant.h"
#include "prepare_silu.h"
#include "process_cast.h"
#include "quantization_patterns.h"
#include "remove_mutation.h"

#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/common_subexpression_elimination.h>
#include <torch/csrc/jit/passes/decompose_ops.h>
#include <torch/csrc/jit/passes/pass_manager.h>
#include <torch/csrc/jit/passes/remove_mutation.h>
#include <torch/csrc/jit/passes/tensorexpr_fuser.h>
#include <torch/csrc/jit/runtime/custom_operator.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <torch/csrc/jit/runtime/operator_options.h>

namespace torch_ipex {
namespace jit {
namespace fuser {
namespace onednn {

using namespace torch::jit;
namespace {
thread_local bool llga_fp32_bf16_enabled = false;
}

bool is_llga_fp32_bf16_enabled() {
  return llga_fp32_bf16_enabled;
}
void set_llga_fp32_bf16_enabled(bool new_enabled) {
  llga_fp32_bf16_enabled = new_enabled;
}

void fuseGraph(std::shared_ptr<Graph>& g) {
  // Follow the process of the tensorexpr_fuser in profiling mode:
  // Remove prim::profile nodes and embed the profile info directly in the
  // IR in value types to avoid breaking the fusion patterns.
  // Will add shape guard after LLGA optimization passes and
  // wipe the tensor type information from the IR, so that it's not
  // accidentally used by any other pass.

  // We rely on the shape specialization and shape guard to ensure the validity
  // of the cached compilation in the kernel, thus only support profiling mode.
  // TODO: add check on LlgaFusionGroup to ensure allShapesAreKnown on nodes to
  // fuse: torch/csrc/jit/passes/tensorexpr_fuser.cpp: allShapesAreKnown
  if (getProfilingMode()) {
    GRAPH_DUMP(
        "Before mutation removal. Beginning of INT8 "
        "optimization pass",
        g);
    IPEXRemoveTensorMutation(g);
    RemoveListMutation(g);
    GRAPH_DUMP("After mutation removal. Before DecomposeOps", g);
    DecomposeOps(g);
    GRAPH_DUMP("After DecomposeOps. Before PrepareBinaryForLLGA", g);
    PrepareBinaryForLLGA(g);
    GRAPH_DUMP("After PrepareBinaryForLLGA. Before PrepareSiluForLLGA", g);
    PrepareSiluForLLGA(g);
    GRAPH_DUMP(
        "After PrepareSiluForLLGA. Before EliminateCommonSubexpression", g);
    EliminateCommonSubexpression(g);
    GRAPH_DUMP(
        "After EliminateCommonSubexpression. Before SaveDequantInformation", g);
    // SaveDequantInformation must be placed before LiftUpQuant
    SaveDequantInformation(g);
    GRAPH_DUMP("After SaveDequantInformation. Before PrepareDequantForLLGA", g);
    // PrepareDequantForLLGA must be placed after EliminateCommonSubexpression
    PrepareDequantForLLGA(g);
    GRAPH_DUMP("After PrepareDequantForLLGA. Before LiftUpQuant", g);
    // LiftUpQuant must be place before DeferSizeCheck
    LiftUpQuant(g);
    GRAPH_DUMP("After LiftUpQuant. Before ProcessCast", g);
    ProcessCast(g);
    GRAPH_DUMP("After ProcessCast. Before DeferSizeCheck", g);
    DeferSizeCheck(g);
    GRAPH_DUMP("After DeferSizeCheck. Before CreateLlgaSubgraphs", g);
    // CreateLlgaSubgraphs must be placed after all the preparation passes above
    CreateLlgaSubgraphs(g);
    GRAPH_DUMP("After CreateLlgaSubgraphs. Before PropagateLayout", g);
    // PropagateLayout must be placed after CreateLlgaSubgraphs
    PropagateLayout(g);
    GRAPH_DUMP("After PropagateLayout. Before RevertPrepareBinaryForLLGA", g);
    // Add shape guard for profiling mode and wipe the tensor type information
    // from the IR
    prepareFusionGroupAndGuardOutputs(g->block());
    GRAPH_DUMP(
        "After prepareFusionGroupAndGuardOutputs. Before "
        "RevertPrepareBinaryForLLGA",
        g);
    RevertPrepareBinaryForLLGA(g);
    GRAPH_DUMP("After RevertPrepareBinaryForLLGA. Before IpexQuantFusion", g);
    IpexQuantFusion(g);
    GRAPH_DUMP("After IpexQuantFusion. End of INT8 optimization pass", g);
  }
}

void setLlgaWeightCacheEnabled(bool enabled) {
  dnnl::graph::set_constant_tensor_cache(enabled);
}

bool getLlgaWeightCacheEnabled() {
  return dnnl::graph::get_constant_tensor_cache();
}

} // namespace onednn
} // namespace fuser

using namespace torch::jit;

Operation createLlgaKernel(const Node* node) {
  auto kernel = std::make_shared<fuser::onednn::LlgaKernel>(node);
  return [kernel](Stack* stack) {
    RECORD_FUNCTION(kernel->profileName(), c10::ArrayRef<c10::IValue>());

    kernel->run(*stack);
    return 0;
  };
}

torch::jit::RegisterOperators LLGAFusionGroupOp({
    torch::jit::Operator(
        Symbol::fromQualString(fuser::onednn::LlgaFusionGroupName()),
        createLlgaKernel,
        AliasAnalysisKind::PURE_FUNCTION),
});

Operation createLlgaGuardKernel(const Node* node) {
  return [node](Stack* stack) {
    RECORD_FUNCTION(
        fuser::onednn::LlgaGuardName(), c10::ArrayRef<c10::IValue>());

    GRAPH_DEBUG("Guarding node: ", node->kind().toQualString());
    std::vector<TypePtr> types = node->tys(attr::types);
    const auto num_inputs = types.size();

    GRAPH_DEBUG("num_inputs to guard: ", num_inputs);

    for (size_t i = 0; i < num_inputs; i++) {
      GRAPH_DEBUG("checking input ", i);
      auto& input = peek(stack, i, num_inputs);
      const c10::TensorTypePtr& guard_tensor_type =
          types[i]->cast<TensorType>();

      if (!input.isTensor()) {
        GRAPH_DEBUG("input ", i, " is not a tensor, return false");
        push(stack, IValue(false));
        return;
      }
      const at::Tensor& tensor = input.toTensor();

      // If input tensor is of mkldnn, it's originated from an upstream
      // LLGA partition that has passed the check on input shapes.
      // It is valid to continue here as long as the output shapes from
      // oneDNN graph partitions are determined by the input shapes.
      if (tensor.is_mkldnn()) {
        GRAPH_DEBUG("input ", i, " is_mkldnn, continue");
        continue;
      }

      if (!guard_tensor_type->matchTensor(tensor)) {
        GRAPH_DEBUG("input ", i, " check failed, return false");
        push(stack, IValue(false));
        return;
      }
    }

    // TODO: check type and return the right flag
    // naively return true;
    GRAPH_DEBUG("all check done, return true");
    push(stack, IValue(true));
    return;
  };
}

torch::jit::RegisterOperators LLGAGuardOp({
    torch::jit::Operator(
        Symbol::fromQualString(fuser::onednn::LlgaGuardName()),
        createLlgaGuardKernel,
        AliasAnalysisKind::PURE_FUNCTION),
});

} // namespace jit
} // namespace torch_ipex
