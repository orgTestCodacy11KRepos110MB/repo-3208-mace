// Copyright 2018 The MACE Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#ifdef MACE_ENABLE_QUANTIZE
#include "mace/ops/arm/q8/quantization_util.h"
// We reuse TensorFlow Lite's optimized depthwiseconv_uint8 and parallelized it
// using thread pool for MACE's quantized depthwise_conv2d.
#include "tensorflow/contrib/lite/kernels/internal/optimized/depthwiseconv_uint8.h"
#endif  // MACE_ENABLE_QUANTIZE

#include "mace/core/future.h"
#include "mace/core/ops/operator.h"
#include "mace/core/registry/ops_registry.h"
#include "mace/ops/activation.h"
#include "mace/ops/conv_pool_2d_base.h"
#include "mace/ops/delegator/activation.h"
#include "mace/ops/delegator/bias_add.h"
#include "mace/ops/delegator/depthwise_conv_2d.h"
#include "mace/public/mace.h"
#include "mace/utils/memory.h"
#include "mace/core/quantize.h"
#ifdef MACE_ENABLE_OPENCL
#include "mace/ops/opencl/buffer/depthwise_conv2d.h"
#include "mace/ops/opencl/image/depthwise_conv2d.h"
#include "mace/runtimes/opencl/transform/buffer_transformer.h"
#endif  // MACE_ENABLE_OPENCL

namespace mace {
namespace ops {

class DepthwiseConv2dOpBase : public ConvPool2dOpBase {
 public:
  explicit DepthwiseConv2dOpBase(OpConstructContext *context)
      : ConvPool2dOpBase(context),
        activation_(ops::StringToActivationType(
            Operation::GetOptionalArg<std::string>("activation",
                                                   "NOOP"))),
        relux_max_limit_(Operation::GetOptionalArg<float>("max_limit", 0.0f)),
        activation_coefficient_(Operation::GetOptionalArg<float>(
            "activation_coefficient", 0.0f)) {}
 protected:
  const ActivationType activation_;
  const float relux_max_limit_;
  const float activation_coefficient_;
};

template<RuntimeType D, class T>
class DepthwiseConv2dOp;

template<class T>
class DepthwiseConv2dOp<RuntimeType::RT_CPU, T> : public DepthwiseConv2dOpBase {
 public:
  explicit DepthwiseConv2dOp(OpConstructContext *context)
      : DepthwiseConv2dOpBase(context),
        activation_delegator_(
            delegator::Activation::Create(
                context->workspace(),
                MACE_DELEGATOR_KEY(Activation, RuntimeType::RT_CPU,
                                   T, kCpuImplType),
                delegator::ActivationParam(
                    activation_, relux_max_limit_, activation_coefficient_,
                    Operation::GetOptionalArg<float>(
                        "hardsigmoid_alpha", 0.f),
                    Operation::GetOptionalArg<float>(
                        "hardsigmoid_beta", 0.f)))),
        bias_add_delegator_(delegator::BiasAdd::Create(
            context->workspace(),
            MACE_DELEGATOR_KEY(BiasAdd, RuntimeType::RT_CPU, T, kCpuImplType),
            DelegatorParam())) {}

  MaceStatus Run(OpContext *context) override {
    MACE_UNUSED(context);
    const Tensor *input = this->Input(INPUT);
    const Tensor *filter = this->Input(FILTER);
    const Tensor *bias = nullptr;
    if (this->InputSize() >= 3) {
      bias = this->Input(BIAS);
    }
    Tensor *output = this->Output(OUTPUT);
    MACE_CHECK_NOTNULL(input);
    MACE_CHECK_NOTNULL(filter);
    MACE_CHECK_NOTNULL(output);

    if (depthwise_conv2d_delegator_ == nullptr) {
      auto tag = MACE_DELEGATOR_KEY(DepthwiseConv2d, RuntimeType::RT_CPU,
                                    T, ImplType::REF);
      if (kCpuImplType == NEON) {
        const index_t filter_h = filter->dim(2);
        const index_t filter_w = filter->dim(3);
        const index_t stride_h = strides_[0];
        const index_t stride_w = strides_[1];
        const index_t dilation_h = dilations_[0];
        const index_t dilation_w = dilations_[1];
        if (filter_h == 3 && filter_w == 3 && stride_h == 1 && stride_w == 1
            && dilation_h == 1 && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(DepthwiseConv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K3x3S1);
        } else if (filter_h == 3 && filter_w == 3 && stride_h == 2
            && stride_w == 2
            && dilation_h == 1 && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(DepthwiseConv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K3x3S2);
        }
      }
      delegator::Conv2dParam param(strides_, dilations_,
                                   paddings_, padding_type_);
      depthwise_conv2d_delegator_ = delegator::DepthwiseConv2d::Create(
          context->workspace(), tag, param);
    }

    depthwise_conv2d_delegator_->Compute(context, input, filter, output);
    bias_add_delegator_->Compute(context, output, bias, output);
    activation_delegator_->Compute(context, output, output);

    return MaceStatus::MACE_SUCCESS;
  }

 private:
  std::unique_ptr<delegator::Activation> activation_delegator_;
  std::unique_ptr<delegator::BiasAdd> bias_add_delegator_;
  std::unique_ptr<delegator::DepthwiseConv2d> depthwise_conv2d_delegator_;

 protected:
  MACE_OP_INPUT_TAGS(INPUT, FILTER, BIAS);
  MACE_OP_OUTPUT_TAGS(OUTPUT);
};

#ifdef MACE_ENABLE_QUANTIZE
template<>
class DepthwiseConv2dOp<RuntimeType::RT_CPU, uint8_t>
    : public DepthwiseConv2dOpBase {
 public:
  explicit DepthwiseConv2dOp(OpConstructContext *context)
      : DepthwiseConv2dOpBase(context) {}

  MaceStatus Run(OpContext *context) override {
    MACE_UNUSED(context);
    const Tensor *input = this->Input(INPUT);
    const Tensor *filter = this->Input(FILTER);
    const Tensor *bias = nullptr;
    if (this->InputSize() >= 3) {
      bias = this->Input(BIAS);
    }
    Tensor *output = this->Output(OUTPUT);
    MACE_CHECK_NOTNULL(input);
    MACE_CHECK_NOTNULL(filter);
    MACE_CHECK_NOTNULL(output);

    std::vector<index_t> output_shape(4);
    std::vector<int> paddings(2);

    // reuse OHWI format, only for calculating output
    std::vector<index_t> ohwi_shape{
        filter->dim(2) * filter->dim(3), filter->dim(0), filter->dim(1), 1};
    if (paddings_.empty()) {
      CalcPaddingAndOutputSize(input->shape().data(),
                               DataFormat::NHWC,
                               ohwi_shape.data(),
                               DataFormat::OHWI,
                               dilations_.data(),
                               strides_.data(),
                               padding_type_,
                               output_shape.data(),
                               paddings.data());
    } else {
      paddings = paddings_;
      CalcOutputSize(input->shape().data(),
                     DataFormat::NHWC,
                     ohwi_shape.data(),
                     DataFormat::OHWI,
                     paddings_.data(),
                     dilations_.data(),
                     strides_.data(),
                     RoundType::FLOOR,
                     output_shape.data());
    }

    MACE_RETURN_IF_ERROR(output->Resize(output_shape));
    output->Clear();

    MACE_CHECK(output->dim(0) == input->dim(0),
               "Input/Output batch size mismatch");
    MACE_CHECK(filter->dim(2) == input->dim(3), filter->dim(2), " != ",
               input->dim(3));

    index_t out_channels = output_shape[3];
    index_t stride_h = strides_[0];
    index_t stride_w = strides_[1];
    index_t dilation_h = dilations_[0];
    index_t dilation_w = dilations_[1];
    int pad_top = paddings[0] >> 1;
    int pad_left = paddings[1] >> 1;

    auto input_data = input->data<uint8_t>();
    auto filter_data = filter->data<uint8_t>();
    auto output_data = output->mutable_data<uint8_t>();
    auto bias_data = GetBiasData(bias,
                                 input->scale(),
                                 filter->scale(),
                                 out_channels,
                                 &bias_);

    if (dilation_h == 1 && dilation_w == 1) {
      int32_t quantized_multiplier;
      int32_t right_shift;
      GetOutputMultiplierAndShift(input->scale(), filter->scale(),
                                  output->scale(), &quantized_multiplier,
                                  &right_shift);
      // 1HWO
      std::vector<index_t> filter_shape{
          1, filter->dim(0), filter->dim(1), filter->dim(2) * filter->dim(3)};
      std::vector<index_t> bias_shape{out_channels};

      tflite::optimized_ops::DepthwiseConv(
          &context->runtime()->thread_pool(),
          input_data, ShapeToTfliteDims(input->shape()), -input->zero_point(),
          filter_data, ShapeToTfliteDims(filter_shape), -filter->zero_point(),
          bias_data, ShapeToTfliteDims(bias_shape), stride_w, stride_h,
          pad_left, pad_top, filter->dim(3), output->zero_point(),
          quantized_multiplier, right_shift, 0, 255, output_data,
          ShapeToTfliteDims(output->shape()));
    } else {
      float output_multiplier =
          input->scale() * filter->scale() / output->scale();
      const int pad_hw[2] = {pad_top, pad_left};
      DepthwiseConv2dGeneral(context,
          input_data, filter_data, bias_data, input->shape().data(),
          output_shape.data(), filter->shape().data(), input->zero_point(),
          filter->zero_point(), output->zero_point(), output_multiplier,
          strides_.data(), dilations_.data(), pad_hw, output_data);
    }

    return MaceStatus::MACE_SUCCESS;
  }
 private:
  void DepthwiseConv2dGeneral(const OpContext *context,
                              const uint8_t *input,
                              const uint8_t *filter,
                              const int32_t *bias,
                              const index_t *in_shape,
                              const index_t *out_shape,
                              const index_t *filter_shape,
                              const int32_t input_zero,
                              const int32_t filter_zero,
                              const int32_t output_zero,
                              const float output_multiplier,
                              const int *stride_hw,
                              const int *dilation_hw,
                              const int *pad_hw,
                              uint8_t *output) {
    utils::ThreadPool &thread_pool = context->runtime()->thread_pool();

    thread_pool.Compute2D([=](index_t start0, index_t end0, index_t step0,
                              index_t start1, index_t end1, index_t step1) {
      for (index_t b = start0; b < end0; b += step0) {
        for (index_t h = start1; h < end1; h += step1) {
          for (index_t w = 0; w < out_shape[2]; ++w) {
            for (index_t m = 0; m < out_shape[3]; ++m) {
              const index_t filter_height = filter_shape[0];
              const index_t filter_width = filter_shape[1];
              const index_t in_channels = filter_shape[2];
              const index_t depth_multiplier = filter_shape[3];
              const index_t in_height = in_shape[1];
              const index_t in_width = in_shape[2];
              const index_t out_height = out_shape[1];
              const index_t out_width = out_shape[2];
              const index_t out_channels = out_shape[3];
              index_t out_offset =
                  ((b * out_height + h) * out_width + w) * out_channels + m;
              index_t c = m / depth_multiplier;
              index_t o = m % depth_multiplier;
              index_t ih_base = h * stride_hw[0] - pad_hw[0];
              index_t iw_base = w * stride_hw[1] - pad_hw[1];
              int32_t sum = 0;
              for (index_t kh = 0; kh < filter_height; ++kh) {
                const index_t ih = ih_base + kh * dilation_hw[0];
                for (index_t kw = 0; kw < filter_width; ++kw) {
                  const index_t iw = iw_base + kw * dilation_hw[1];
                  if (ih >= 0 && ih < in_height && iw >= 0 && iw < in_width) {
                    index_t in_offset =
                        ((b * in_height + ih) * in_width + iw) * in_channels
                            + c;
                    index_t filter_offset =
                        ((kh * filter_width + kw) * in_channels + c)
                            * depth_multiplier + o;

                    sum += (input[in_offset] - input_zero) *
                        (filter[filter_offset] - filter_zero);
                  }
                }
              }
              if (bias) {
                sum += bias[m];
              }
              sum = static_cast<int32_t>(std::round(sum * output_multiplier));
              sum += output_zero;
              output[out_offset] =
                  static_cast<uint8_t>(std::min(255, std::max(0, sum)));
            }
          }
        }
      }
    }, 0, out_shape[0], 1, 0, out_shape[1], 1);
  }

  inline tflite::Dims<4> ShapeToTfliteDims(const std::vector<index_t> &shape) {
    tflite::Dims<4> d;
    for (int i = 0; i < 4; ++i) {
      int src = static_cast<int>(shape.size() - i - 1);
      if (src >= 0) {
        d.sizes[i] = shape[src];
      } else {
        d.sizes[i] = 1;
      }
    }
    d.strides[0] = 1;
    for (int i = 1; i < 4; i++) {
      d.strides[i] = d.strides[i - 1] * d.sizes[i - 1];
    }
    return d;
  }

 protected:
  MACE_OP_INPUT_TAGS(INPUT, FILTER, BIAS);
  MACE_OP_OUTPUT_TAGS(OUTPUT);

 private:
  std::vector<int32_t> bias_;
};
#endif  // MACE_ENABLE_QUANTIZE

#ifdef MACE_ENABLE_OPENCL
template<>
class DepthwiseConv2dOp<RuntimeType::RT_OPENCL, float> :
    public DepthwiseConv2dOpBase {
 public:
  explicit DepthwiseConv2dOp(OpConstructContext *context)
      : DepthwiseConv2dOpBase(context) {
    MemoryType mem_type;
    if (context->GetOpMemoryType() == MemoryType::GPU_IMAGE) {
      mem_type = MemoryType::GPU_IMAGE;
      kernel_ = make_unique<opencl::image::DepthwiseConv2dKernel>();
    } else {
      mem_type = MemoryType::GPU_BUFFER;
      kernel_ = make_unique<opencl::buffer::DepthwiseConv2dKernel>();
    }
    Tensor *filter_tensor = context->workspace()->GetTensor(
        operator_def_->input(1));
    if (filter_tensor != nullptr && filter_tensor->is_weight()) {
      // Transform filter tensor to target format
      MACE_CHECK(TransformFilter(
          context,
          operator_def_.get(),
          1,
          BufferContentType::DW_CONV2D_FILTER,
          mem_type) == MaceStatus::MACE_SUCCESS);
    }
    if (operator_def_->input_size() > 2) {
      MACE_CHECK(TransformFilter(context, operator_def_.get(), 2,
                                 BufferContentType::ARGUMENT, mem_type)
                     == MaceStatus::MACE_SUCCESS);
    }
  }
  MaceStatus Run(OpContext *context) override {
    const Tensor *input = this->Input(INPUT);
    const Tensor *filter = this->Input(FILTER);
    const Tensor *bias = nullptr;
    if (this->InputSize() >= 3) {
      bias = this->Input(BIAS);
    }
    Tensor *output = this->Output(OUTPUT);
    return kernel_->Compute(context, input, filter, bias,
                            strides_.data(), padding_type_, paddings_,
                            dilations_.data(), activation_, relux_max_limit_,
                            activation_coefficient_, output);
  }

 private:
  std::unique_ptr<OpenCLDepthwiseConv2dKernel> kernel_;

 protected:
  MACE_OP_INPUT_TAGS(INPUT, FILTER, BIAS);
  MACE_OP_OUTPUT_TAGS(OUTPUT);
};
#endif  // MACE_ENABLE_OPENCL

void RegisterDepthwiseConv2d(OpRegistry *op_registry) {
  MACE_REGISTER_OP(op_registry, "DepthwiseConv2d",
                   DepthwiseConv2dOp, RuntimeType::RT_CPU, float);
  MACE_REGISTER_BF16_OP(op_registry, "DepthwiseConv2d",
                        DepthwiseConv2dOp, RuntimeType::RT_CPU);
  MACE_REGISTER_FP16_OP(op_registry, "DepthwiseConv2d",
                        DepthwiseConv2dOp, RuntimeType::RT_CPU);

#ifdef MACE_ENABLE_QUANTIZE
  MACE_REGISTER_OP(op_registry, "DepthwiseConv2d",
                   DepthwiseConv2dOp, RuntimeType::RT_CPU, uint8_t);
#endif  // MACE_ENABLE_QUANTIZE

  MACE_REGISTER_GPU_OP(op_registry, "DepthwiseConv2d", DepthwiseConv2dOp);

#ifdef MACE_ENABLE_OPENCL
  MACE_REGISTER_OP_CONDITION(
      op_registry,
      OpConditionBuilder("DepthwiseConv2d").SetInputMemoryTypeSetter(
          [](OpConditionContext *context) -> void {
            SetFilterMemoryType(context, BufferContentType::DW_CONV2D_FILTER);
          }));
#endif  // MACE_ENABLE_OPENCL

  RegisterFilterDataFormat(op_registry, "DepthwiseConv2d");
}

}  // namespace ops
}  // namespace mace
