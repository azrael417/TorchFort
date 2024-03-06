/*
 * SPDX-FileCopyrightText: Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include <cuda_runtime.h>

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <torch/torch.h>

#include "internal/defines.h"
#include "internal/logging.h"

namespace torchfort {
namespace rl {

// generic training system
class RLOffPolicySystem {
  template <typename T> friend void wandb_log_system(const char*, const char*, int64_t, T);

public:
  // disable copy constructor
  RLOffPolicySystem(const RLOffPolicySystem&) = delete;

  // empty constructor:
  RLOffPolicySystem() : train_step_count_(0) {}

  // some important functions which have to be implemented by the base class
  virtual void updateReplayBuffer(torch::Tensor, torch::Tensor, torch::Tensor, float, bool) = 0;
  virtual bool isReady() = 0;

  // these have to be implemented
  virtual torch::Tensor explore(torch::Tensor action) = 0;
  virtual torch::Tensor predict(torch::Tensor state) = 0;
  virtual torch::Tensor predictExplore(torch::Tensor state) = 0;
  virtual torch::Tensor evaluate(torch::Tensor state, torch::Tensor action) = 0;
  virtual void trainStep(float&, float&) = 0;
  virtual void printInfo() const = 0;
  virtual void initSystemComm(MPI_Comm mpi_comm) = 0;
  virtual void saveCheckpoint(const std::string& checkpoint_dir) const = 0;
  virtual void loadCheckpoint(const std::string& checkpoint_dir) = 0;

protected:
  virtual std::shared_ptr<ModelState> getSystemState_() = 0;
  virtual std::shared_ptr<Comm> getSystemComm_() = 0;
  size_t train_step_count_;
};

// Declaration of external global variables
extern std::unordered_map<std::string, std::shared_ptr<RLOffPolicySystem>> rl_systems;

// some convenience wrappers
template <MemoryLayout L, typename T>
static void update_replay_buffer(const char* name, T* state_old, T* state_new, size_t state_dim, int64_t* state_shape,
                                 T* action_old, size_t action_dim, int64_t* action_shape, T reward, bool final_state,
                                 cudaStream_t ext_stream) {

  // device stuff
  int device_id;
  CHECK_CUDA(cudaGetDevice(&device_id));
  auto stream = c10::cuda::getStreamFromExternal(ext_stream, device_id);
  c10::cuda::CUDAStreamGuard guard(stream);

  // no grad
  torch::NoGradGuard no_grad;

  // get tensors and copy:
  auto state_old_tensor = get_tensor<L>(state_old, state_dim, state_shape)
                              .to(torch::kFloat32, /* non_blocking = */ false, /* copy = */ true);
  auto state_new_tensor = get_tensor<L>(state_new, state_dim, state_shape)
                              .to(torch::kFloat32, /* non_blocking = */ false, /* copy = */ true);
  auto action_old_tensor = get_tensor<L>(action_old, action_dim, action_shape)
                               .to(torch::kFloat32, /* non_blocking = */ false, /* copy = */ true);

  rl_systems[name]->updateReplayBuffer(state_old_tensor, action_old_tensor, state_new_tensor,
                                       static_cast<float>(reward), final_state);
  return;
}

template <MemoryLayout L, typename T>
static void predict_explore(const char* name, T* state, size_t state_dim, int64_t* state_shape, T* action,
                            size_t action_dim, int64_t* action_shape, cudaStream_t ext_stream) {

  // device and stream handling
  int device_id;
  CHECK_CUDA(cudaGetDevice(&device_id));
  auto stream = c10::cuda::getStreamFromExternal(ext_stream, device_id);
  c10::cuda::CUDAStreamGuard guard(stream);

  // create tensors
  auto state_tensor = get_tensor<L>(state, state_dim, state_shape)
                          .to(torch::kFloat32, /* non_blocking = */ false, /* copy = */ true);
  auto action_tensor = get_tensor<L>(action, action_dim, action_shape);

  // fwd pass
  auto tmpaction = rl_systems[name]->predictExplore(state_tensor).to(action_tensor.dtype());
  action_tensor.copy_(tmpaction);

  return;
}

template <MemoryLayout L, typename T>
static void predict(const char* name, T* state, size_t state_dim, int64_t* state_shape, T* action, size_t action_dim,
                    int64_t* action_shape, cudaStream_t ext_stream) {

  // device and stream handling
  int device_id;
  CHECK_CUDA(cudaGetDevice(&device_id));
  auto stream = c10::cuda::getStreamFromExternal(ext_stream, device_id);
  c10::cuda::CUDAStreamGuard guard(stream);

  // create tensors
  auto state_tensor = get_tensor<L>(state, state_dim, state_shape)
                          .to(torch::kFloat32, /* non_blocking = */ false, /* copy = */ true);
  auto action_tensor = get_tensor<L>(action, action_dim, action_shape);

  // fwd pass
  auto tmpaction = rl_systems[name]->predict(state_tensor).to(action_tensor.dtype());
  action_tensor.copy_(tmpaction);

  return;
}

template <MemoryLayout L, typename T>
static void evaluate(const char* name, T* state, size_t state_dim, int64_t* state_shape, T* action, size_t action_dim,
                     int64_t* action_shape, T* reward, size_t reward_dim, int64_t* reward_shape,
                     cudaStream_t ext_stream) {

  // device and stream handling
  int device_id;
  CHECK_CUDA(cudaGetDevice(&device_id));
  auto stream = c10::cuda::getStreamFromExternal(ext_stream, device_id);
  c10::cuda::CUDAStreamGuard guard(stream);

  // create tensors
  auto state_tensor = get_tensor<L>(state, state_dim, state_shape)
                          .to(torch::kFloat32, /* non_blocking = */ false, /* copy = */ true);
  auto action_tensor = get_tensor<L>(action, action_dim, action_shape)
                           .to(torch::kFloat32, /* non_blocking = */ false, /* copy = */ true);
  auto reward_tensor = get_tensor<L>(reward, reward_dim, reward_shape);

  // fwd pass
  auto tmpreward = rl_systems[name]->evaluate(state_tensor, action_tensor).to(reward_tensor.dtype());
  reward_tensor.copy_(tmpreward);

  return;
}

// logging helpers
template <typename T> void wandb_log_system(const char* name, const char* metric_name, int64_t step, T value) {
  auto system = rl_systems[name];

  wandb_log(system->getSystemState_(), system->getSystemComm_(), name, metric_name, step, value);
}

} // namespace rl
} // namespace torchfort
