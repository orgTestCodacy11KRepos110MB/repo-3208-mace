// Copyright 2021 The MACE Authors. All Rights Reserved.
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

#ifndef MACE_OPS_OPENCL_BUFFER_TRANSPOSE_H_
#define MACE_OPS_OPENCL_BUFFER_TRANSPOSE_H_

#include <set>
#include <string>
#include <vector>

#include "mace/core/runtime/opencl/opencl_helper.h"
#include "mace/ops/opencl/transpose.h"

namespace mace {
namespace ops {
namespace opencl {
namespace buffer {

class TransposeKernel : public OpenCLTransposeKernel {
 public:
  TransposeKernel() {}

  MaceStatus Compute(OpContext *context,
                     const Tensor *input,
                     const std::vector<int> &dims,
                     Tensor *output) override;
 private:
  cl::Kernel kernel_;
  std::vector<index_t> input_shape_;
};

}  // namespace buffer
}  // namespace opencl
}  // namespace ops
}  // namespace mace

#endif  // MACE_OPS_OPENCL_BUFFER_TRANSPOSE_H_