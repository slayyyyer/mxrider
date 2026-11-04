// policy_test.cpp
//   A console harness around the mod's own Policy class, so the weights format can be checked
//   against the Python exporter without launching the game.
//
//   It prints the network's shape and its output for one fixed observation, which tests/parity.py
//   reproduces with the numpy implementation. If the two disagree, the DLL would drive differently
//   from what was trained, which is exactly the failure this catches.
//
//   Build and run (from mxrider/):  tests\run_policy_test.bat path\to\mxrider_policy.txt

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "policy.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: policy_test <mxrider_policy.txt>\n");
        return 2;
    }

    wchar_t path[1024];
    const int length = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path,
                                          static_cast<int>(sizeof path / sizeof path[0]));
    if (length <= 0) {
        std::printf("bad path\n");
        return 2;
    }

    mxrider::Policy policy;
    if (!policy.Load(path)) {
        // The loader has already said why in the log; the caller only needs the verdict.
        std::printf("LOAD_FAILED\n");
        return 1;
    }

    // The same fixed observation parity.py feeds its numpy copy: mixed signs and magnitudes, so a
    // transposed weight matrix or a swapped layer order cannot pass by luck.
    const double obs[16] = {
        0.70, -0.30, 0.25, -0.15, 0.05, -0.40, 0.30, -0.20,
        0.10, 1.00, 0.60, 0.00, 0.20, -0.50, 0.15, 0.33,
    };
    double out[6] = { 0, 0, 0, 0, 0, 0 };
    if (!policy.Eval(obs, out)) {
        std::printf("EVAL_FAILED\n");
        return 1;
    }

    std::printf("SHAPE %d %d %d\n", policy.ObsSize(), policy.OutSize(), policy.HiddenSize(0));
    std::printf("OUT");
    for (int i = 0; i < policy.OutSize(); ++i)
        std::printf(" %.9g", out[i]);
    std::printf("\n");
    return 0;
}
