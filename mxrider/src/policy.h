// policy.h
//   The learned part: a small fully-connected network evaluated at frame rate inside the DLL.
//
//   Training happens offline in Python (train/), on the episodes this mod logs; the result is
//   exported as a plain-text weight file. Keeping evaluation in C++ means the control loop adds no
//   IPC, no extra process and no deadline to miss - the network is ~1.3k multiply-adds inside the
//   frame the game is drawing anyway.
//
//   Everything about loading is defensive: a missing file, a truncated line, a NaN, or a shape
//   that doesn't match the observation the DLL builds all leave Loaded() false, and the rider then
//   does nothing (or falls back to the baseline) rather than pressing arbitrary controls.
#pragma once

#include <vector>

namespace mxrider {

class Policy {
public:
    // Reads the weights file. Returns false and leaves the policy unloaded on any problem.
    bool Load(const wchar_t* path);
    void Clear();

    bool Loaded() const { return loaded_; }

    int ObsSize() const { return obs_; }
    int OutSize() const { return out_; }
    int HiddenSize(int layer) const { return (layer >= 0 && layer < 2) ? hidden_[layer] : 0; }
    const wchar_t* Path() const { return path_; }

    // obs must have ObsSize() values; out receives OutSize() values, each in [-1, 1]
    // (tanh output: 0 means "no input", which is the safe default).
    // Returns false if the policy is not usable; out is then zeroed.
    bool Eval(const double* obs, double* out) const;

private:
    std::vector<double> w1_, b1_, w2_, b2_, w3_, b3_;
    int  obs_ = 0;
    int  hidden_[2] = { 0, 0 };
    int  out_ = 0;
    bool loaded_ = false;
    wchar_t path_[1024] = L"";
};

}  // namespace mxrider
