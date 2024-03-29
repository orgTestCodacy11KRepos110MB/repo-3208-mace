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

#if defined(MACE_ENABLE_NEON)
#include <arm_neon.h>
#endif
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "mace/core/future.h"
#include "mace/core/ops/operator.h"
#include "mace/core/registry/ops_registry.h"
#include "mace/core/tensor.h"
#include "mace/ops/activation.h"
#include "mace/ops/conv_pool_2d_base.h"
#include "mace/ops/common/conv_pool_2d_util.h"
#include "mace/ops/delegator/activation.h"
#include "mace/ops/delegator/bias_add.h"
#include "mace/ops/delegator/conv_2d.h"
#include "mace/utils/memory.h"
#include "mace/utils/math.h"

#ifdef MACE_ENABLE_QUANTIZE
#include "mace/ops/common/gemmlowp_util.h"
#include "mace/ops/arm/q8/quantization_util.h"
#include "mace/runtimes/cpu/cpu_runtime.h"
#endif  // MACE_ENABLE_QUANTIZE

#ifdef MACE_ENABLE_OPENCL
#include "mace/ops/opencl/buffer/conv_2d.h"
#include "mace/ops/opencl/image/conv_2d.h"
#include "mace/runtimes/opencl/opencl_runtime.h"
#include "mace/runtimes/opencl/transform/buffer_transformer.h"
#endif  // MACE_ENABLE_OPENCL

namespace mace {
namespace ops {

template<RuntimeType D, class T>
class Conv2dOp;

template<class T>
class Conv2dOp<RuntimeType::RT_CPU, T> : public ConvPool2dOpBase {
 public:
  explicit Conv2dOp(OpConstructContext *context)
      : ConvPool2dOpBase(context),
        activation_delegator_(
            delegator::Activation::Create(
                context->workspace(),
                MACE_DELEGATOR_KEY(Activation, RuntimeType::RT_CPU,
                                   T, kCpuImplType),
                delegator::ActivationParam(
                    ops::StringToActivationType(
                        Operation::GetOptionalArg<std::string>("activation",
                                                               "NOOP")),
                    Operation::GetOptionalArg<float>("max_limit", 0.0f),
                    Operation::GetOptionalArg<float>("activation_coefficient",
                                                     0.0f),
                    Operation::GetOptionalArg<float>("hardsigmoid_alpha", 0.f),
                    Operation::GetOptionalArg<float>("hardsigmoid_beta", 0.f)))),
        bias_add_delegator_(delegator::BiasAdd::Create(
            context->workspace(),
            MACE_DELEGATOR_KEY(BiasAdd, RuntimeType::RT_CPU, T, kCpuImplType),
            DelegatorParam())) {}

  MaceStatus Run(OpContext *context) override {
    const Tensor *input = this->Input(INPUT);
    const Tensor *filter = this->Input(FILTER);
    const Tensor *bias = this->InputSize() >= 3 ? this->Input(BIAS) : nullptr;
    Tensor *output = this->Output(OUTPUT);

    if (conv2d_delegator_ == nullptr) {
      auto tag = MACE_DELEGATOR_KEY(Conv2d,
                                    RuntimeType::RT_CPU, T, kCpuImplType);
      if (kCpuImplType == NEON) {
        // the following params are used to decide which conv delegator to use
        const index_t stride_h = strides_[0];
        const index_t stride_w = strides_[1];
        const index_t dilation_h = dilations_[0];
        const index_t dilation_w = dilations_[1];
        const index_t filter_h = filter->dim(2);
        const index_t filter_w = filter->dim(3);
        const index_t input_channels = input->dim(1);
        const index_t channels = filter->dim(0);
        // NOTE: delegator is fixed after first round of running,
        // although winograd depends on input params.
        // We do not support changeable filter for now.
        if (filter_h == 1 && filter_w == 1 && stride_h == 1 && stride_w == 1
            && dilation_h == 1 && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K1x1);
        } else if (filter_h == 3 && filter_w == 3
            && stride_h == 1 && stride_w == 1 && dilation_h == 1
            && dilation_w == 1) {
          if (input_channels >= 8 && channels >= 8) {
            tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                        kCpuImplType, K3x3Winograd);
          } else {
            tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                        kCpuImplType, K3x3S1);
          }
        } else if (filter_h == 3 && filter_w == 3
            && stride_h == 2 && stride_w == 2 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K3x3S2);
        } else if (filter_h == 5 && filter_w == 5
            && stride_h == 1 && stride_w == 1 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K5x5S1);
        } else if (filter_h == 7 && filter_w == 7
            && stride_h == 1 && stride_w == 1 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K7x7S1);
        } else if (filter_h == 7 && filter_w == 7
            && stride_h == 2 && stride_w == 2 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K7x7S2);
        } else if (filter_h == 7 && filter_w == 7
            && stride_h == 3 && stride_w == 3 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K7x7S3);
        } else if (filter_h == 1 && filter_w == 7
            && stride_h == 1 && stride_w == 1 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K1x7S1);
        } else if (filter_h == 7 && filter_w == 1
            && stride_h == 1 && stride_w == 1 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K7x1S1);
        } else if (filter_h == 1 && filter_w == 15
            && stride_h == 1 && stride_w == 1 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K1x15S1);
        } else if (filter_h == 15 && filter_w == 1
            && stride_h == 1 && stride_w == 1 && dilation_h == 1
            && dilation_w == 1) {
          tag = MACE_DELEGATOR_KEY_EX(Conv2d, RuntimeType::RT_CPU, T,
                                      kCpuImplType, K15x1S1);
        }
      }
      delegator::Conv2dParam param(strides_, dilations_,
                                   paddings_, padding_type_);
      conv2d_delegator_ = delegator::Conv2d::Create(context->workspace(),
                                                    tag, param);
    }

    conv2d_delegator_->Compute(context, input, filter, output);
    bias_add_delegator_->Compute(context, output, bias, output);
    activation_delegator_->Compute(context, output, output);

    return MaceStatus::MACE_SUCCESS;
  }

 private:
  std::unique_ptr<delegator::Activation> activation_delegator_;
  std::unique_ptr<delegator::BiasAdd> bias_add_delegator_;
  std::unique_ptr<delegator::Conv2d> conv2d_delegator_;

 private:
  MACE_OP_INPUT_TAGS(INPUT, FILTER, BIAS);
  MACE_OP_OUTPUT_TAGS(OUTPUT);
};

#ifdef MACE_ENABLE_QUANTIZE
template<>
class Conv2dOp<RuntimeType::RT_CPU, uint8_t> : public ConvPool2dOpBase {
 public:
  explicit Conv2dOp(OpConstructContext *context)
      : ConvPool2dOpBase(context),
        activation_(ops::StringToActivationType(
            Operation::GetOptionalArg<std::string>("activation",
                                                   "NOOP"))),
        relux_max_limit_(Operation::GetOptionalArg<float>("max_limit", 0.0f)),
        activation_coefficient_(Operation::GetOptionalArg<float>(
            "activation_coefficient", 0.0f)) {}

  MaceStatus Run(OpContext *context) override {
    const Tensor *input = this->Input(INPUT);
    const Tensor *filter = this->Input(FILTER);
    const Tensor *bias = this->InputSize() >= 3 ? this->Input(BIAS) : nullptr;
    Tensor *output = this->Output(OUTPUT);

    MACE_CHECK(dilations_[0] == 1 && dilations_[1] == 1,
               "Quantization convolution does not support dilation > 1 yet.");

    auto gemm_context = CpuRuntime::Get(context)->GetGemmlowpContext();
    MACE_CHECK_NOTNULL(gemm_context);

    std::vector<index_t> output_shape(4);
    std::vector<int> paddings(2);
    if (paddings_.empty()) {
      CalcPaddingAndOutputSize(input->shape().data(),
                               DataFormat::NHWC,
                               filter->shape().data(),
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
                     filter->shape().data(),
                     DataFormat::OHWI,
                     paddings_.data(),
                     dilations_.data(),
                     strides_.data(),
                     RoundType::FLOOR,
                     output_shape.data());
    }
    MACE_RETURN_IF_ERROR(output->Resize(output_shape));

    index_t batch = output->dim(0);
    index_t height = output->dim(1);
    index_t width = output->dim(2);
    index_t channels = output->dim(3);
    index_t input_batch = input->dim(0);
    index_t input_channels = input->dim(3);
    index_t filter_h = filter->dim(1);
    index_t filter_w = filter->dim(2);
    index_t stride_h = strides_[0];
    index_t stride_w = strides_[1];
    const index_t depth = input_channels * filter_h * filter_w;
    const index_t columns = batch * height * width;

    VLOG(2) << "input scale/zero: " << input->scale() << ", "
            << input->zero_point();
    VLOG(2) << "filter scale/zero: " << filter->scale() << ", "
            << filter->zero_point();
    if (bias) {
      VLOG(2) << "bias scale/zero: " << bias->scale() << ", "
              << bias->zero_point();
    }
    VLOG(2) << "output scale/zero: " << output->scale() << ", "
            << output->zero_point();

    MACE_CHECK(filter->dim(0) == channels, filter->dim(0), " != ", channels);
    MACE_CHECK(filter->dim(3) == input_channels, filter->dim(3), " != ",
               input_channels);
    MACE_CHECK(batch == input_batch, "Input/Output batch size mismatch");

    auto input_data = input->data<uint8_t>();
    auto filter_data = filter->data<uint8_t>();
    auto output_data = output->mutable_data<uint8_t>();
    auto bias_data = GetBiasData(bias,
                                 input->scale(),
                                 filter->scale(),
                                 channels,
                                 &bias_);

    auto gemm_input_data = input_data;
    std::unique_ptr<Tensor> im2col;
    bool im2col_required =
        filter_h != 1 || filter_w != 1 || stride_h != 1 || stride_w != 1;
    if (im2col_required) {
      // prepare im2col
      auto *runtime = context->runtime();
      std::vector<index_t> tensor_shape = {depth, columns};
      im2col = make_unique<Tensor>(runtime, DT_UINT8,
                                   input->memory_type(), tensor_shape);
      runtime->AllocateBufferForTensor(im2col.get(), BufRentType::RENT_SCRATCH);
      uint8_t *im2col_data = im2col->mutable_data<uint8_t>();
      Im2col(context, input_data, input->shape(), filter_h, filter_w, stride_h,
             stride_w, static_cast<uint8_t>(input->zero_point()),
             paddings[0], paddings[1], output->shape(), depth, im2col_data);
      gemm_input_data = im2col_data;
    }

    const int gemm_filter_rows = static_cast<int>(channels);
    const int gemm_filter_cols = static_cast<int>(depth);
    const int gemm_input_rows = static_cast<int>(depth);
    const int gemm_input_cols = static_cast<int>(columns);
    const int gemm_output_rows = static_cast<int>(channels);
    const int gemm_output_cols = static_cast<int>(columns);
    gemmlowp::MatrixMap<const uint8_t, gemmlowp::MapOrder::RowMajor>
        filter_matrix(filter_data, gemm_filter_rows, gemm_filter_cols);
    gemmlowp::MatrixMap<const uint8_t, gemmlowp::MapOrder::ColMajor>
        input_matrix(gemm_input_data, gemm_input_rows, gemm_input_cols);
    gemmlowp::MatrixMap<uint8_t, gemmlowp::MapOrder::ColMajor>
        output_matrix(output_data, gemm_output_rows, gemm_output_cols);

    const auto &output_pipeline = GemmlowpOutputPipeline::Make(
        bias_data, channels, filter->scale(), input->scale(), output->scale(),
        output->zero_point());

    using BitDepthParams = gemmlowp::L8R8WithLhsNonzeroBitDepthParams;
    gemmlowp::GemmWithOutputPipeline<uint8_t, uint8_t, BitDepthParams>(
        gemm_context, filter_matrix, input_matrix, &output_matrix,
        -filter->zero_point(), -input->zero_point(), output_pipeline);

    return MaceStatus::MACE_SUCCESS;
  }

 private:
  template<typename T>
  inline void Im2col(
      const OpContext *context,
      const T *in_data, const std::vector<index_t> &in_shape,
      const index_t filter_h, const index_t filter_w, const index_t stride_h,
      const index_t stride_w, const T zero_point, const int pad_height,
      const int pad_width, const std::vector<index_t> &out_shape,
      const index_t depth, T *im2col_data) {
    const index_t input_row_size = in_shape[2] * in_shape[3];
    const index_t patch_row_size = filter_w * in_shape[3];

    utils::ThreadPool &thread_pool = context->runtime()->thread_pool();

    thread_pool.Compute3D([=](index_t start0, index_t end0, index_t step0,
                              index_t start1, index_t end1, index_t step1,
                              index_t start2, index_t end2, index_t step2) {
      for (index_t b = start0; b < end0; b += step0) {
        for (index_t h = start1; h < end1; h += step1) {
          for (index_t w = start2; w < end2; w += step2) {
            // Reshape a patch of input to column, which is corresponding to
            // a column of output(:, column).
            const index_t ih_begin = h * stride_h - (pad_height >> 1);
            const index_t ih_end = ih_begin + filter_h;
            const index_t iw_begin = w * stride_w - (pad_width >> 1);
            const index_t iw_end = iw_begin + filter_w;
            // gate height and width to separate padding
            const index_t ih_begin_gated = std::max<index_t>(0, ih_begin);
            const index_t ih_end_gated = std::min<index_t>(ih_end, in_shape[1]);
            const index_t iw_begin_gated = std::max<index_t>(0, iw_begin);
            const index_t iw_end_gated = std::min<index_t>(iw_end, in_shape[2]);
            const index_t pad_top = std::max<index_t>(0, -ih_begin);
            const index_t pad_bottom = ih_end - ih_end_gated;
            const index_t pad_left = std::max<index_t>(0, -iw_begin);
            const index_t pad_right = iw_end - iw_end_gated;
            index_t im2col_column_offset =
                ((b * out_shape[1] + h) * out_shape[2] + w) * depth;

            // fill in padding top
            if (pad_top > 0) {
              std::fill_n(im2col_data + im2col_column_offset,
                          pad_top * patch_row_size, zero_point);
            }

            const index_t patch_row_size_gated =
                std::min(filter_w - pad_left,
                         in_shape[2] - iw_begin_gated) * in_shape[3];
            MACE_CHECK(patch_row_size_gated ==
                ((filter_w - (pad_left + pad_right)) * in_shape[3]));
            const index_t pad_left_size = pad_left * in_shape[3];
            const index_t pad_right_size = pad_right * in_shape[3];
            index_t im2col_offset = im2col_column_offset +
                (pad_top * filter_w + pad_left) * in_shape[3];
            index_t
                in_offset = ((b * in_shape[1] + ih_begin_gated) * in_shape[2]
                + iw_begin_gated) * in_shape[3];

            // fill in effective rows
            for (index_t ih = ih_begin_gated; ih < ih_end_gated; ++ih) {
              // fill in padding left
              if (pad_left > 0) {
                const index_t left_offset = im2col_offset - pad_left_size;
                std::fill_n(im2col_data + left_offset,
                            pad_left_size,
                            zero_point);
              }
              // copy effective data
              std::copy_n(in_data + in_offset, patch_row_size_gated,
                          im2col_data + im2col_offset);
              // fill in padding right
              if (pad_right > 0) {
                const index_t
                    right_offset = im2col_offset + patch_row_size_gated;
                std::fill_n(im2col_data + right_offset, pad_right_size,
                            zero_point);
              }
              in_offset += input_row_size;
              im2col_offset += patch_row_size;
            }

            // fill in padding bottom
            if (pad_bottom > 0) {
              const index_t pad_bottom_size = pad_bottom * patch_row_size;
              const index_t bottom_offset =
                  im2col_column_offset + depth - pad_bottom_size;
              std::fill_n(im2col_data + bottom_offset, pad_bottom_size,
                          zero_point);
            }
          }
        }
      }
    }, 0, out_shape[0], 1, 0, out_shape[1], 1, 0, out_shape[2], 1);
  }

 private:
  const ActivationType activation_;
  const float relux_max_limit_;
  const float activation_coefficient_;
  std::vector<int32_t> bias_;

 private:
  MACE_OP_INPUT_TAGS(INPUT, FILTER, BIAS);
  MACE_OP_OUTPUT_TAGS(OUTPUT);
};
#endif  // MACE_ENABLE_QUANTIZE

#ifdef MACE_ENABLE_OPENCL
template<>
class Conv2dOp<RuntimeType::RT_OPENCL, float> : public ConvPool2dOpBase {
 public:
  explicit Conv2dOp(OpConstructContext *context)
      : ConvPool2dOpBase(context),
        activation_(ops::StringToActivationType(
            Operation::GetOptionalArg<std::string>("activation",
                                                   "NOOP"))),
        relux_max_limit_(Operation::GetOptionalArg<float>("max_limit", 0.0f)),
        activation_coefficient_(Operation::GetOptionalArg<float>(
              "activation_coefficient", 0.0f)),
        wino_block_size_(Operation::GetOptionalArg<int>("wino_block_size", 0)) {
    MemoryType mem_type;
    if (context->GetOpMemoryType() == MemoryType::GPU_IMAGE) {
      mem_type = MemoryType::GPU_IMAGE;
      kernel_ = make_unique<opencl::image::Conv2dKernel>();
    } else {
      mem_type = MemoryType::GPU_BUFFER;
      kernel_ = make_unique<opencl::buffer::Conv2dKernel>();
    }
    // Transform input tensor to target format
    auto *input_tensor =
        context->workspace()->GetTensor(operator_def_->input(INPUT));
    if (input_tensor != nullptr && input_tensor->is_weight()) {
        MACE_CHECK(TransformFilter(
            context, operator_def_.get(), 0,
            BufferContentType::IN_OUT_CHANNEL, mem_type)
                       == MaceStatus::MACE_SUCCESS);
    }
    // Transform filter tensor to target format
    auto *filter_tensor =
        context->workspace()->GetTensor(operator_def_->input(FILTER));
    if (filter_tensor != nullptr && filter_tensor->is_weight()) {
      if ((wino_block_size_ == 2 || wino_block_size_ == 4) &&
          (kernel_->CheckUseWinograd(
            OpenclRuntime::Get(context)->GetOpenclExecutor(),
            filter_tensor->shape(),
            std::vector<index_t>(operator_def_->output_shape(0).dims().begin(),
                                 operator_def_->output_shape(0).dims().end()),
            strides_.data(),
            dilations_.data(),
            &wino_block_size_))) {
        MACE_CHECK(TransformFilter(
            context, operator_def_.get(), 1,
            BufferContentType::WINOGRAD_FILTER, mem_type, wino_block_size_)
                       == MaceStatus::MACE_SUCCESS);
      } else {
        wino_block_size_ = 0;
        MACE_CHECK(TransformFilter(
            context, operator_def_.get(), 1,
            BufferContentType::CONV2D_FILTER, mem_type)
                       == MaceStatus::MACE_SUCCESS);
      }
    } else {
      // we don't know whether the kernal support winograd, so disable it.
      wino_block_size_ = 0;
    }

    if (operator_def_->input_size() > 2) {
      auto ret = TransformFilter(context, operator_def_.get(), 2,
                                 BufferContentType::ARGUMENT, mem_type);
      MACE_CHECK(ret == MaceStatus::MACE_SUCCESS);
    }
  }

  MaceStatus Run(OpContext *context) override {
    const Tensor *input = this->Input(INPUT);
    const Tensor *filter = this->Input(FILTER);
    const Tensor *bias = this->InputSize() >= 3 ? this->Input(BIAS) : nullptr;
    Tensor *output = this->Output(OUTPUT);
    return kernel_->Compute(context, input, filter, bias,
                            strides_.data(), padding_type_, paddings_,
                            dilations_.data(), activation_, relux_max_limit_,
                            activation_coefficient_, wino_block_size_, output);
  }

 protected:
  BufferContentType GetInputTensorContentType(size_t idx) const override {
    if (idx == FILTER) {
      if (wino_block_size_ == 0) {
        return BufferContentType::CONV2D_FILTER;
      } else {
        return BufferContentType::WINOGRAD_FILTER;
      }
    }
    return Operation::GetInputTensorContentType(idx);
  }

 private:
  const ActivationType activation_;
  const float relux_max_limit_;
  const float activation_coefficient_;
  std::unique_ptr<OpenCLConv2dKernel> kernel_;
  int wino_block_size_;

 private:
  MACE_OP_INPUT_TAGS(INPUT, FILTER, BIAS);
  MACE_OP_OUTPUT_TAGS(OUTPUT);
};
#endif  // MACE_ENABLE_OPENCL

void RegisterConv2D(OpRegistry *op_registry) {
  MACE_REGISTER_OP(op_registry, "Conv2D", Conv2dOp, RuntimeType::RT_CPU, float);
  MACE_REGISTER_BF16_OP(op_registry, "Conv2D", Conv2dOp, RuntimeType::RT_CPU);
  MACE_REGISTER_FP16_OP(op_registry, "Conv2D", Conv2dOp, RuntimeType::RT_CPU);

#ifdef MACE_ENABLE_QUANTIZE
  MACE_REGISTER_OP(op_registry, "Conv2D", Conv2dOp,
                   RuntimeType::RT_CPU, uint8_t);
#endif  // MACE_ENABLE_QUANTIZE

  MACE_REGISTER_GPU_OP(op_registry, "Conv2D", Conv2dOp);

#ifdef MACE_ENABLE_OPENCL
  MACE_REGISTER_OP_CONDITION(
      op_registry,
      OpConditionBuilder("Conv2D").SetInputMemoryTypeSetter(
          [](OpConditionContext *context) -> void {
            SetFilterMemoryType(context, BufferContentType::CONV2D_FILTER);
          }));
#endif  // MACE_ENABLE_OPENCL

  RegisterFilterDataFormat(op_registry, "Conv2D");
}

}  // namespace ops
}  // namespace mace
