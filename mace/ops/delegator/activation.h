// Copyright 2020 The MACE Authors. All Rights Reserved.
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

#ifndef MACE_OPS_DELEGATOR_ACTIVATION_H_
#define MACE_OPS_DELEGATOR_ACTIVATION_H_

#include "mace/core/ops/op_context.h"
#include "mace/core/ops/op_delegator.h"
#include "mace/core/registry/op_delegator_registry.h"
#include "mace/ops/common/activation_type.h"

namespace mace {
namespace ops {
namespace delegator {

struct ActivationParam : public DelegatorParam {
  explicit ActivationParam(ActivationType type, const float limit,
                           const float activation_coefficient)
      : type_(type), limit_(limit),
        activation_coefficient_(activation_coefficient),
        hardsigmoid_alpha_(0),
        hardsigmoid_beta_(0) {}
  explicit ActivationParam(ActivationType type, const float limit,
                           const float activation_coefficient,
                           const float hardsigmoid_alpha,
                           const float hardsigmoid_beta)
      : type_(type), limit_(limit),
        activation_coefficient_(activation_coefficient),
        hardsigmoid_alpha_(hardsigmoid_alpha),
        hardsigmoid_beta_(hardsigmoid_beta) {}

  ActivationType type_;
  const float limit_;
  const float activation_coefficient_;
  const float hardsigmoid_alpha_;
  const float hardsigmoid_beta_;
};

class Activation : public OpDelegator {
 public:
  explicit Activation(const ActivationParam &param)
      : OpDelegator(param), type_(param.type_), limit_(param.limit_),
        activation_coefficient_(param.activation_coefficient_),
        hardsigmoid_alpha_(param.hardsigmoid_alpha_),
        hardsigmoid_beta_(param.hardsigmoid_beta_) {}
  virtual ~Activation() = default;

  MACE_DEFINE_DELEGATOR_CREATOR(Activation)

  virtual MaceStatus Compute(const OpContext *context,
                             const Tensor *input,
                             Tensor *output) = 0;

 protected:
  ActivationType type_;
  const float limit_;
  const float activation_coefficient_;
  const float hardsigmoid_alpha_;
  const float hardsigmoid_beta_;
};

}  // namespace delegator
}  // namespace ops
}  // namespace mace

#endif  // MACE_OPS_DELEGATOR_ACTIVATION_H_
