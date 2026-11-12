#include "pitch.h"

#include <cmath>
#include <new>

#include "signalsmith-stretch.h"

namespace alp {

struct PitchShifter::Impl {
  signalsmith::stretch::SignalsmithStretch<float> st{20261112L};  // свой seed: без random_device
  float semis = 0.0f;
};

PitchShifter::~PitchShifter() { delete p_; }

bool PitchShifter::init(double sample_rate) {
  if (!p_) p_ = new (std::nothrow) Impl();
  if (!p_) return false;
  p_->st.presetDefault(2, (float)sample_rate, true);
  p_->semis = 0.0f;
  p_->st.setTransposeSemitones(0.0f);
  return true;
}

int PitchShifter::latency() const { return p_ ? p_->st.outputSeekLength(1.0f) : 0; }
int PitchShifter::seek_len() const { return p_ ? p_->st.seekLength() : 0; }
void PitchShifter::reset() {
  if (p_) p_->st.reset();
}

void PitchShifter::set_semitones(float s) {
  if (!p_ || s == p_->semis) return;
  p_->semis = s;
  p_->st.setTransposeSemitones(s);
}

int PitchShifter::warm_len() const { return p_ ? p_->st.outputLatency() : 0; }

void PitchShifter::start(const float* const* hist, const float* const* pre, float* const* tmp) {
  if (!p_) return;
  p_->st.reset();
  p_->st.seek(hist, p_->st.seekLength(), 1.0);
  const int w = p_->st.outputLatency();
  p_->st.process(pre, w, tmp, w);
}

void PitchShifter::seek(const float* const* in, int n) {
  if (p_) p_->st.seek(in, n, 1.0);
}

void PitchShifter::process(const float* const* in, float* const* out, int n) {
  if (p_) p_->st.process(in, n, out, n);
}

}  // namespace alp
