#pragma once
#include <random>
#include <torch/torch.h>

// this file contains some pre-defined environments useful for debugging RL systems
class Environment {
public:
  Environment(const Environment&) = delete;

  // default constructor
  Environment(unsigned int num_steps_per_episode) : num_steps_per_episode_(num_steps_per_episode), num_steps_(0) {}

  virtual std::tuple<torch::Tensor, float, bool> step(torch::Tensor) = 0;
  virtual std::tuple<torch::Tensor, float> initialize() = 0;
  
protected:
  unsigned int num_steps_per_episode_;
  unsigned int num_steps_;
};

// this is the simplest of the environments, it always emits the same reward no matter what
class ConstantRewardEnvironment : public Environment, public std::enable_shared_from_this<Environment> {
public:

  ConstantRewardEnvironment(unsigned int num_steps_per_episode,
			    torch::IntArrayRef state_shape,
			    torch::IntArrayRef action_shape,
			    float default_reward) : default_reward_(default_reward),
						    state_shape_(state_shape), action_shape_(action_shape),
						    Environment(num_steps_per_episode) {}
  
  std::tuple<torch::Tensor, float, bool> step(torch::Tensor action) {
    torch::NoGradGuard no_grad;
    num_steps_++;
    return std::make_tuple(torch::zeros(state_shape_, torch::kFloat32), default_reward_, (num_steps_ % num_steps_per_episode_ == 0));
  }

  std::tuple<torch::Tensor, float> initialize() {
    return std::make_tuple(torch::zeros(state_shape_, torch::kFloat32), default_reward_);
  }
  
private:
  float default_reward_;
  torch::IntArrayRef state_shape_;
  torch::IntArrayRef action_shape_;
};

// this env emits the reward final_reward at the end of the episode but 0 before. Useful to debug reward discounting
class DelayedRewardEnvironment : public Environment, public std::enable_shared_from_this<Environment> {
public:

  DelayedRewardEnvironment(unsigned int num_steps_per_episode,
                           torch::IntArrayRef state_shape,
                           torch::IntArrayRef action_shape,
                           float final_reward) : final_reward_(final_reward),
						 state_shape_(state_shape), action_shape_(action_shape),
						 Environment(num_steps_per_episode) {}

  std::tuple<torch::Tensor, float, bool> step(torch::Tensor action) {
    torch::NoGradGuard no_grad;
    num_steps_++;
    float reward = ((num_steps_ % num_steps_per_episode_ == 0) ? final_reward_ : 0.);
    return std::make_tuple(torch::zeros(state_shape_, torch::kFloat32), reward, (num_steps_ % num_steps_per_episode_ == 0));
  }

  std::tuple<torch::Tensor, float> initialize() {
    return std::make_tuple(torch::zeros(state_shape_, torch::kFloat32), 0.);
  }

private:
  float final_reward_;
  torch::IntArrayRef state_shape_;
  torch::IntArrayRef action_shape_;
};

// this env emits a random reward -1 or +1 and the same value for the state, so it is easy to predict
class PredictableRewardEnvironment : public Environment, public std::enable_shared_from_this<Environment> {
public:
  PredictableRewardEnvironment(unsigned int num_steps_per_episode,
			       torch::IntArrayRef state_shape,
			       torch::IntArrayRef action_shape) : state_shape_(state_shape), action_shape_(action_shape),
								  udist_(0,1), Environment(num_steps_per_episode) {
    
    std::random_device dev;
    rngptr_ = std::make_shared<std::mt19937>(dev());
    reward_ = 2 * static_cast<float>(udist_(*rngptr_)) - 1.;
    state_ = torch::empty(state_shape_, torch::kFloat32);
    state_.fill_(reward_);
  }

  std::tuple<torch::Tensor, float> initialize() {
    return std::make_tuple(state_.clone(), 1.);
  }
  
  std::tuple<torch::Tensor, float, bool> step(torch::Tensor action) {
    torch::NoGradGuard no_grad;
    num_steps_++;

    // backup the current reward
    float reward = reward_;
    
    // compute next reward
    reward_ = 2 * static_cast<float>(udist_(*rngptr_)) - 1.;
    state_.fill_(reward_);
    return std::make_tuple(state_.clone(), reward, (num_steps_ % num_steps_per_episode_ == 0));
  }
  
private:
  std::shared_ptr<std::mt19937> rngptr_;
  std::uniform_int_distribution<std::mt19937::result_type> udist_;
  float default_reward_;
  torch::IntArrayRef state_shape_;
  torch::IntArrayRef action_shape_;
  torch::Tensor state_;
  float reward_;
};
