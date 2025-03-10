#pragma once

#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/core/symbol.h>
#include <ATen/quantized/QTensorImpl.h>
#include <oneapi/dnnl/dnnl_graph.hpp>
#include <torch/csrc/jit/ir/ir.h>

namespace torch_ipex {
namespace jit {
namespace fuser {
namespace onednn {

struct LlgaTensorDesc {
  using desc = dnnl::graph::logical_tensor;

  LlgaTensorDesc(
      size_t tid,
      std::vector<int64_t> sizes,
      std::vector<int64_t> strides,
      desc::data_type dtype,
      desc::property_type property_type,
      bool is_scalar_tensor)
      : tid_(tid),
        sizes_(sizes),
        strides_(strides),
        dtype_(dtype),
        property_type_(property_type),
        layout_type_(desc::layout_type::strided),
        layout_id_(-1),
        is_scalar_tensor_(is_scalar_tensor) {}

  LlgaTensorDesc(const desc& t)
      : tid_(t.get_id()),
        sizes_(t.get_dims()),
        strides_({-1}),
        dtype_(t.get_data_type()),
        property_type_(t.get_property_type()),
        layout_type_(t.get_layout_type()),
        layout_id_(-1) {
    if (is_opaque())
      layout_id_ = t.get_layout_id();
    if (is_strided())
      strides_ = t.get_strides();
  }

  LlgaTensorDesc(const torch::jit::Value* v)
      : LlgaTensorDesc(
            v->unique(),
            {},
            {},
            desc::data_type::f32,
            get_property_type(v),
            /* is_scalar_tensor = */ false) {
    if (v->type()->isSubtypeOf(torch::jit::TensorType::get())) {
      auto tt = v->type()->cast<torch::jit::TensorType>();

      if (tt->scalarType())
        dtype_ = getLlgaDataType(tt->scalarType().value());

      // Use numel() if IValue can be used.
      // Otherwise, check if scalar attribute exists.
      auto user_nodes = v->uses();
      if (toIValue(v).has_value()) {
        auto v_tensor = toIValue(v).value().toTensor();
        if ((v_tensor.numel() == 1) && (v_tensor.sizes().size() == 0)) {
          is_scalar_tensor_ = true;
        }
      } else if (user_nodes.size() != 0) {
        for (auto user : user_nodes) {
          if (user.user->hasAttributeS("scalar") &&
              ((user.offset == 1) ||
               ((user.offset == 2) &&
                (user.user->kind() ==
                 c10::Symbol::fromQualString("aten::where"))))) {
            // either a binary op, whose second input was a scalar,
            // or aten::where, whose third input was scalar
            is_scalar_tensor_ = true;
            break;
          }
        }
      }

      if (!is_scalar_tensor_) {
        auto sizes = tt->sizes();
        if (sizes.sizes()) {
          for (auto d : *sizes.sizes()) {
            sizes_.push_back(d.value_or(DNNL_GRAPH_UNKNOWN_DIM));
          }
        }
        auto strides = tt->strides();
        if (strides.sizes()) {
          for (auto d : *strides.sizes()) {
            strides_.push_back(d.value_or(DNNL_GRAPH_UNKNOWN_DIM));
          }
        }
      }
    }
  }

  LlgaTensorDesc supplementTensorInfo(const at::Tensor& t) const;

  at::ScalarType aten_scalar_type() const;

  dnnl::graph::logical_tensor::data_type getLlgaDataType(
      at::ScalarType dt) const;

  const std::vector<int64_t>& sizes() const {
    return sizes_;
  }

  const std::vector<int64_t>& strides() const {
    TORCH_CHECK(!is_opaque(), "Cannot get strides on opaque layout");
    return strides_;
  }

  size_t tid() const {
    return tid_;
  }

  LlgaTensorDesc tid(uint64_t new_id) const {
    auto ret = *this;
    ret.tid_ = new_id;
    return ret;
  }

  desc::data_type dtype() const {
    return dtype_;
  }

  LlgaTensorDesc dtype(desc::data_type new_dtype) const {
    return LlgaTensorDesc(
        tid_, sizes_, strides_, new_dtype, property_type_, is_scalar_tensor_);
  }

  desc::layout_type layout_type() const {
    return layout_type_;
  }

  LlgaTensorDesc layout_type(desc::layout_type new_layout_type) {
    auto ret = *this;
    ret.layout_type_ = new_layout_type;
    return ret;
  }

  LlgaTensorDesc set_quantizer(at::QuantizerPtr new_quantizer) {
    auto ret = *this;
    ret.quantizer_ = new_quantizer;
    return ret;
  }

  LlgaTensorDesc update_desc(const desc& t) const {
    return LlgaTensorDesc(t).set_quantizer(quantizer_);
  }

  at::QuantizerPtr get_quantizer() {
    return quantizer_;
  }

  desc::property_type get_property_type(const torch::jit::Value* v) {
    switch (v->node()->kind()) {
      case torch::jit::prim::Constant:
        return desc::property_type::constant;
      default:
        return desc::property_type::variable;
    }
  }

  LlgaTensorDesc any() {
    return layout_type(desc::layout_type::any);
  }

  size_t storage_size() const {
    return logical_tensor().get_mem_size();
  }

  desc logical_tensor() const {
    if (is_scalar_tensor_) {
      return desc(
          tid_,
          dtype_,
          dnnl::graph::logical_tensor::dims{},
          dnnl::graph::logical_tensor::dims{});
    } else if (is_dimensionality_unknown()) {
      return desc(
          tid_, dtype_, DNNL_GRAPH_UNKNOWN_NDIMS, layout_type_, property_type_);
    } else if (is_opaque()) {
      return desc(tid_, dtype_, sizes_, layout_id_, property_type_);
    } else if (is_any()) {
      return desc(tid_, dtype_, sizes_, layout_type_, property_type_);
    } else {
      return desc(tid_, dtype_, sizes_, strides_, property_type_);
    }
  }

  bool is_strided() const {
    return layout_type_ == desc::layout_type::strided;
  }

  bool is_any() const {
    return layout_type_ == desc::layout_type::any;
  }

  bool is_opaque() const {
    return layout_type_ == desc::layout_type::opaque;
  }

  bool is_quantized() const {
    return (dtype_ == desc::data_type::u8) || (dtype_ == desc::data_type::s8);
  }

  bool operator==(const LlgaTensorDesc& desc) const {
    return tid_ == desc.tid_ && sizes_ == desc.sizes_ &&
        dtype_ == desc.dtype_ && layout_type_ == desc.layout_type_ &&
        ((is_opaque() && layout_id_ == desc.layout_id_) ||
         strides_ == desc.strides_);
  }

  bool operator!=(const LlgaTensorDesc& desc) const {
    return *this != desc;
  }

  static size_t hash(const LlgaTensorDesc& desc) {
    return c10::get_hash(
        desc.tid_,
        desc.sizes_,
        desc.dtype_,
        desc.layout_type_,
        desc.layout_id_);
  }

 private:
  bool is_dimensionality_unknown() const {
    return sizes_.size() == 0;
  }

  size_t tid_;
  std::vector<int64_t> sizes_;
  std::vector<int64_t> strides_;
  desc::data_type dtype_;
  desc::property_type property_type_;
  desc::layout_type layout_type_;
  size_t layout_id_;
  bool is_scalar_tensor_ = false;
  at::QuantizerPtr quantizer_;
};

// Initially, oneDNN Graph also used to have blocked layout for tensors between
// partitions, and the LlgaTensorImpl wrapper helped us bypass guard checks.
// oneDNN Graph has switched over to using strided tensors between partitions.
// So why are we still wrapping tensors between partitions with LlgaTensorImpl?
// The answer is that it helps us bypass guard checks because the strides of
// tensors between partitions would be different from the ones the guard is
// otherwise expecting.
struct TORCH_API LlgaTensorImpl : public c10::TensorImpl {
  LlgaTensorImpl(
      c10::Storage&& storage,
      const caffe2::TypeMeta& data_type,
      const LlgaTensorDesc& desc);

  const LlgaTensorDesc& desc() const {
    return desc_;
  }

  // Override a bunch of methods inherited from TensorImpl to return error
  // messages.
  bool has_storage() const override;
  static at::Tensor llga_to_aten_tensor(LlgaTensorImpl* llgaImpl);
  static at::Tensor llga_to_aten_tensor(
      LlgaTensorImpl* llgaImpl,
      at::QuantizerPtr quantizer);

 private:
  LlgaTensorDesc desc_;
};

at::Tensor empty_llga(
    const LlgaTensorDesc& desc,
    const at::TensorOptions& options);

dnnl::graph::tensor llga_from_aten_tensor(const at::Tensor& tensor);

} // namespace onednn
} // namespace fuser
} // namespace jit
} // namespace torch_ipex
