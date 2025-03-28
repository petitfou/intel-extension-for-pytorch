import torch
import torch.ao.nn.quantized as nnq
from torch.ao.nn.quantized.modules.utils import _quantize_weight
import torch.ao.nn.intrinsic as nni
from ...quantization._qconfig import get_weight_only_quant_qconfig_mapping


class IpexWoqLinear(nnq.Linear):
    r"""
    A weight-only quantized (WOQ) linear module with floating point tensor as inputs and outputs.
    Weight is dequantized at runtime for computation.
    We adopt the same interface as `torch.nn.Linear`, please see
    https://pytorch.org/docs/stable/nn.html#torch.nn.Linear for documentation.

    Similar to :class:`torch.nn.Linear`, attributes will be randomly
    initialized at module creation time and will be overwritten later

    Attributes:
        weight (Tensor): the non-learnable quantized weights of the module which are of
                         shape :math:`(\text{out\_features}, \text{in\_features})`.
        bias (Tensor): the non-learnable floating point bias of the module of shape
                       :math:`(\text{out\_features})`. If :attr:`bias` is ``True``,
                       the values are initialized to zero.

    Examples::

        >>> # xdoctest: +SKIP
        >>> m = ipex.nn.IpexWoqLinear(20, 30)
        >>> input = torch.randn(128, 20)
        >>> output = m(input)
        >>> print(output.size())
        torch.Size([128, 30])
    """
    # version used in this class is different from the parent class nnq.Linear
    _version = 4

    def __init__(self, in_features, out_features, bias_=True, dtype=torch.qint8):
        # nnq.Linear does not support quint4x2 so we set qint8 here as a hack
        # This dtype is used for weight prepacking and we do not rely on the prepacking
        # of nnq.Linear. So, it won't affect our implementation here.
        super().__init__(in_features, out_features, bias_, dtype=torch.qint8)
        weight = torch.rand(out_features, in_features)
        qweight = torch.quantize_per_channel(
            weight, torch.ones(out_features), torch.zeros(out_features), 0, dtype
        )
        bias = torch.rand(out_features)
        self._op_context = torch.ops.ipex_prepack.weight_only_qlinear_prepack(
            qweight, bias, None
        )
        self.weight_qscheme = self.weight().qscheme()
        del weight
        del qweight

    def forward(self, x):
        # Note that we can handle self.bias == None case.
        if self._packed_params.dtype in [torch.qint8, torch.quint4x2]:
            Y = torch.ops.torch_ipex.ipex_woq_linear(
                x, self._op_context.get_data_handle()
            )
        else:
            raise RuntimeError("Unsupported dtype of wegiht only quantized linear!")
        return Y.to(x.dtype)

    def _get_name(self):
        return "IpexWeightOnlyQuantizedLinear"

    def extra_repr(self):
        extra_repr_str = "in_features={}, out_features={}, dtype={}".format(
            self.in_features, self.out_features, self._packed_params.dtype
        )
        if self._packed_params.dtype in [torch.qint8, torch.quint4x2]:
            extra_repr_str += ", qscheme={}".format(self.weight_qscheme)
        return extra_repr_str

    def _save_to_state_dict(self, destination, prefix, keep_vars):
        assert (
            not keep_vars
        ), "can not using keep_vars true when to save _IPEXConvNd's parameters"
        if self.bias is not None:
            bias = self.bias.float()
            destination[prefix + "bias"] = bias.detach()
        weight = self.weight.float()
        destination[prefix + "weight"] = self.ctx.to_public(weight.detach())

    def _load_from_state_dict(
        self,
        state_dict,
        prefix,
        local_metadata,
        strict,
        missing_keys,
        unexpected_keys,
        error_msgs,
    ):
        with torch.no_grad():
            w_name = prefix + "weight"
            b_name = prefix + "bias"
            fp32_loaded_weight = state_dict[w_name]
            loaded_weight = fp32_loaded_weight.to(self.weight.dtype)
            if b_name in state_dict:
                loaded_bias = state_dict[b_name]
                loaded_bias = loaded_bias.to(self.bias.dtype)
            else:
                loaded_bias = None
            self._op_context = torch.ops.ipex_prepack.weight_only_qlinear_prepack(
                loaded_weight, loaded_bias, None
            )

    @classmethod
    def from_float(cls, mod):
        r"""Create a weight-only quantized module from a float module or qparams_dict

        Args:
            mod (Module): a float module, either produced by torch.ao.quantization
                          utilities or provided by the user
        """
        float_modules = [torch.nn.Linear]

        assert (
            type(mod) in float_modules
        ), "IpexWoqLinear.from_float only works for one of" + str(
            [float_mod.__name__ for float_mod in float_modules]
        )
        assert hasattr(mod, "qconfig"), "Input float module must have qconfig defined"
        if type(mod) == nni.LinearReLU:
            mod = mod[0]
        if mod.qconfig is not None and mod.qconfig.weight is not None:
            weight_observer = mod.qconfig.weight()
        else:
            weight_observer = (
                get_weight_only_quant_qconfig_mapping().global_qconfig.weight()
            )
        dtype = weight_observer.dtype
        assert dtype in [torch.qint8, torch.quint4x2], (
            "The only supported dtypes for "
            "weight-only quantized linear are qint8 and quint4x2 got: {}".format(dtype)
        )
        weight_observer(mod.weight)
        if dtype in [torch.qint8, torch.quint4x2]:
            qweight = _quantize_weight(mod.weight.float(), weight_observer)
        else:
            raise RuntimeError(
                "Unsupported dtype specified for dynamic quantized Linear!"
            )
        qlinear = cls(mod.in_features, mod.out_features, dtype=dtype)
        qlinear._op_context = torch.ops.ipex_prepack.weight_only_qlinear_prepack(
            qweight, mod.bias, None
        )
        qlinear.weight_qscheme = qlinear.weight().qscheme()
        del qweight
        return qlinear

    @classmethod
    def from_reference(cls, ref_qlinear):
        """Create a weight-only quantized module from a reference quantized module
        Args:
            ref_qlinear (Module): a reference quantized  module, either produced by
            torch.ao.quantization functions or provided by the user
        """
        qlinear = cls(
            ref_qlinear.in_features,
            ref_qlinear.out_features,
            dtype=ref_qlinear.weight_dtype,
        )
        qweight = ref_qlinear.get_quantized_weight()
        bias = ref_qlinear.bias
        # qlinear.set_weight_bias(qweight, bias)
        qlinear._op_context = torch.ops.ipex_prepack.weight_only_qlinear_prepack(
            qweight, bias, None
        )
        qlinear.weight_qscheme = qlinear.weight().qscheme()
        return qlinear
