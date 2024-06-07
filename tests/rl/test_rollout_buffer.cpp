#include <torch/torch.h>
#include "internal/rl/rollout_buffer.h"

using namespace torchfort;
using namespace torch::indexing;

// helper functions
std::shared_ptr<rl::GAELambdaRolloutBuffer> getTestRolloutBuffer(int buffer_size, float gamma=0.95, float lambda=0.99) {

  torch::NoGradGuard no_grad;
  
  auto rbuff = std::make_shared<rl::GAELambdaRolloutBuffer>(buffer_size, gamma, lambda, -1);

  // initialize rng
  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> dist(1,5);
  std::normal_distribution<float> normal(1.0, 1.0);

  // fill the buffer
  float	reward, log_p, q;
  bool done;
  torch::Tensor state = torch::zeros({1}, torch::kFloat32), action;
  for (unsigned int i=0; i<buffer_size+1; ++i) {
    action = torch::ones({1}, torch::kFloat32) * static_cast<float>(dist(rng));
    reward = action.item<float>();
    q = reward;
    log_p = normal(rng);
    done = false;
    rbuff->update(state, action, reward, q, log_p, done);
    state = state + action;
  }

  return rbuff;
}

void print_buffer(std::shared_ptr<rl::GAELambdaRolloutBuffer> buffp) {
  torch::Tensor stens, atens;
  float reward, q, log_p;
  bool done;
  for(unsigned int i=0; i<buffp->getSize(); ++i) {
    std::tie(stens, atens, reward, q, log_p, done) = buffp->get(i);
    std::cout << "entry " << i << ": s = " << stens.item<float>() << " a = " << atens.item<float>()
	      << " r = " << reward << " q = " << q << " log_p = " << log_p << " d = " << done << std::endl;
  }
}

// check if entries are consistent
bool TestEntryConsistency() {
  // some parameters
  unsigned int batch_size = 2;
  unsigned int buffer_size = 4 * batch_size;
  unsigned int n_iters = 4;

  // get replay buffer
  auto rbuff = getTestRolloutBuffer(buffer_size, 0.95, 1);

  print_buffer(rbuff);

  // sample
  torch::Tensor stens, atens, qtens, log_p_tens, advtens, rettens;
  float q_diff = 0.;
  float reward_diff = 0.;
  for (unsigned int i=0; i<n_iters; ++i) {
    std::tie(stens, atens, qtens, log_p_tens, advtens, rettens) = rbuff->sample(batch_size);

    // compute differences:
    q_diff += torch::sum(torch::abs(qtens - (rettens - advtens))).item<float>() / static_cast<float>(n_iters);
    //reward_diff += torch::sum(torch::abs(atens - rtens)).item<float>();
  }
  // make sure values are consistent:
  std::cout << "TestEntryConsistency: q-diff " << q_diff << std::endl;

  return (q_diff < 1e-7) && (reward_diff < 1e-7);
}

#if 0
// check if ordering between entries are consistent
bool TestTrajectoryConsistency() {
  // some parameters
  unsigned int batch_size = 32;
  unsigned int buffer_size = 4 * batch_size;

  // get replay buffer
  auto rbuff = getTestReplayBuffer(buffer_size, 0.95, 1);
  
  // get a few items and their successors:
  torch::Tensor stens, atens, sptens, sptens_tmp;
  float reward;
  bool done;
  // get item ad index 
  std::tie(stens, atens, sptens, reward, done) = rbuff->get(0);
  // get next item
  float state_diff = 0.;
  for (unsigned int i=1; i<buffer_size; ++i) {
    std::tie(stens, atens, sptens_tmp, reward, done) = rbuff->get(i);
    state_diff += torch::sum(torch::abs(stens - sptens)).item<float>();
    sptens.copy_(sptens_tmp);
  }
  std::cout << "TestTrajectoryConsistency: state-diff " << state_diff << std::endl;

  return (state_diff < 1e-7);
}

// check if nstep reward calculation is correct
bool TestNstepConsistency() {
  // some parameters
  unsigned int batch_size = 32;
  unsigned int buffer_size = 8 * batch_size;
  unsigned int nstep = 4;
  float gamma = 0.95;

  // get replay buffer
  auto rbuff = getTestReplayBuffer(buffer_size, gamma, nstep);
  
  // sample a batch
  torch::Tensor stens, atens, sptens, rtens, dtens;
  float state_diff = 0;
  float reward_diff = 0.;
  std::tie(stens, atens, sptens, rtens, dtens) = rbuff->sample(batch_size);

  // iterate over samples in batch
  torch::Tensor stemp, atemp, sptemp, sstens;
  float rtemp, reward, gamma_eff, rdiff, sdiff, sstens_val;
  bool dtemp;

  // init differences:
  rdiff = 0.;
  sdiff = 0.;
  for (int64_t s=0; s<batch_size; ++s) {
    sstens = stens.index({s, "..."});
    sstens_val = sstens.item<float>();
    
    // find the corresponding state
    for (unsigned int i=0; i<buffer_size; ++i) {
      std::tie(stemp, atemp, sptemp, rtemp, dtemp) = rbuff->get(i);
      if (std::abs(stemp.item<float>() - sstens_val) < 1e-7) {
	
	// found the right state
	gamma_eff = 1.;
	reward = rtemp;
	for(unsigned int k=1; k<nstep; k++) {
	  std::tie(stemp, atemp, sptemp, rtemp, dtemp) = rbuff->get(i+k);
	  gamma_eff *= gamma;
	  reward += rtemp * gamma_eff;
	}
	break;
      }
    }
    rdiff += std::abs(reward - rtens.index({s, "..."}).item<float>());
    sdiff += torch::sum(torch::abs(sptemp - sptens.index({s, "..."}))).item<float>();
    //std::cout << "reward = " << reward << " sp = " << sptemp.item<float>() << std::endl;
    //std::cout << "reward-reference = " << rtens.index({s, "..."}).item<float>()<< " sp-reference = " << sptens.index({s, "..."}).item<float>() << std::endl;
    //std::cout << "rdiff = " << rdiff << " sdiff = " << sdiff << std::endl;
  }
  // make sure values are consistent:
  std::cout << "TestEntryConsistency: state-diff " << sdiff << " reward-diff " << rdiff << std::endl;
  
  return ((rdiff < 1e-7) && (sdiff < 1e-7));
}
#endif

int main(int argc, char* argv[]){

  TestEntryConsistency();
  
  //TestTrajectoryConsistency();

  //TestNstepConsistency();
}