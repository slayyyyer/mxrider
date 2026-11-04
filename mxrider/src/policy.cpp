// policy.cpp
//   Reads the exported network and evaluates it. The file format is whitespace-separated tokens
//   with labels, so it can be read, diffed and hand-checked (line breaks are free):
//
//     1
//     obs 16
//     hidden 32 32
//     out 6
//     w1 <32*16 numbers>
//     b1 <32 numbers>
//     w2 <32*32 numbers>
//     b2 <32 numbers>
//     w3 <6*32 numbers>
//     b3 <6 numbers>
//
//   w1 is row-major: w1[o * obs + i] weights observation i into hidden unit o. Layer order is
//   tanh, tanh, tanh - the last tanh keeps every channel inside [-1, 1] so a wild weight can only
//   produce a full-scale control, never a nonsensical one; Action::Sanitized() maps the negative
//   half to "released" for the pedals.

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "policy.h"

#if defined(_MSC_VER)
#define MXRIDER_STRTOK strtok_s
#define MXRIDER_FOPEN _wfopen_s
#else
#define MXRIDER_STRTOK strtok_r
#endif

namespace mxrider {

void Policy::Clear()
{
    w1_.clear(); b1_.clear(); w2_.clear(); b2_.clear(); w3_.clear(); b3_.clear();
    obs_ = hidden_[0] = hidden_[1] = out_ = 0;
    loaded_ = false;
}

bool Policy::Load(const wchar_t* path)
{
    Clear();
    if (!path || !path[0]) {
        Log("policy: no weights file configured");
        return false;
    }
    wcscpy_s(path_, path);

    FILE* file = nullptr;
    if (MXRIDER_FOPEN(&file, path, L"r") != 0 || !file) {
        Log("policy: %ls not found; the rider will not use the network", path);
        return false;
    }

    std::vector<char> buffer(1 << 20);  // 1 MiB is far more than this network can need
    const size_t read = fread(buffer.data(), 1, buffer.size() - 1, file);
    fclose(file);
    buffer[read] = '\0';
    if (read == 0 || read >= buffer.size() - 1) {
        Log("policy: %ls is empty or implausibly large", path);
        return false;
    }

    // Tokenise the whole file: the format is whitespace-separated, one tag followed by its values.
    std::vector<char*> tokens;
    {
        char* cursor = buffer.data();
        char* context = nullptr;
        for (char* token = MXRIDER_STRTOK(cursor, " \t\r\n", &context); token;
             token = MXRIDER_STRTOK(nullptr, " \t\r\n", &context))
            tokens.push_back(token);
    }

    size_t index = 0;
    auto Next = [&](const char*& out) {
        if (index >= tokens.size())
            return false;
        out = tokens[index++];
        return true;
    };
    auto Expect = [&](const char* text) {
        const char* token = nullptr;
        if (!Next(token) || _stricmp(token, text) != 0) {
            Log("policy: %ls is malformed: expected '%s'", path, text);
            return false;
        }
        return true;
    };
    auto ReadInt = [&](int& value) {
        const char* token = nullptr;
        if (!Next(token))
            return false;
        char* end = nullptr;
        const long parsed = strtol(token, &end, 10);
        if (end == token || *end != '\0')
            return false;
        value = static_cast<int>(parsed);
        return true;
    };
    auto ReadValues = [&](std::vector<double>& values, size_t count) {
        values.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const char* token = nullptr;
            if (!Next(token))
                return false;
            char* end = nullptr;
            const double parsed = strtod(token, &end);
            if (end == token || *end != '\0' || !std::isfinite(parsed))
                return false;
            values[i] = parsed;
        }
        return true;
    };

    int version = 0;
    if (!ReadInt(version) || version != 1) {
        Log("policy: %ls is not a version 1 MXRider policy", path);
        Clear();
        return false;
    }
    if (!Expect("obs") || !ReadInt(obs_) ||
        !Expect("hidden") || !ReadInt(hidden_[0]) || !ReadInt(hidden_[1]) ||
        !Expect("out") || !ReadInt(out_)) {
        Log("policy: %ls is missing its shape header", path);
        Clear();
        return false;
    }
    if (obs_ < 1 || obs_ > 256 || hidden_[0] < 1 || hidden_[0] > 512 ||
        hidden_[1] < 1 || hidden_[1] > 512 || out_ < 1 || out_ > 32) {
        Log("policy: %ls has an implausible shape (%d, %d, %d, %d)", path, obs_, hidden_[0], hidden_[1], out_);
        Clear();
        return false;
    }

    const size_t n1 = static_cast<size_t>(obs_) * hidden_[0];
    const size_t n2 = static_cast<size_t>(hidden_[0]) * hidden_[1];
    const size_t n3 = static_cast<size_t>(hidden_[1]) * out_;
    const bool ok = Expect("w1") && ReadValues(w1_, n1) &&
                    Expect("b1") && ReadValues(b1_, hidden_[0]) &&
                    Expect("w2") && ReadValues(w2_, n2) &&
                    Expect("b2") && ReadValues(b2_, hidden_[1]) &&
                    Expect("w3") && ReadValues(w3_, n3) &&
                    Expect("b3") && ReadValues(b3_, out_);
    if (!ok) {
        Log("policy: %ls ends early or has a bad number; not using it", path);
        Clear();
        return false;
    }

    loaded_ = true;
    Log("policy: loaded %ls (%d -> %d -> %d -> %d)", path, obs_, hidden_[0], hidden_[1], out_);
    return true;
}

bool Policy::Eval(const double* obs, double* out) const
{
    if (!out)
        return false;
    if (!loaded_ || !obs) {
        for (int i = 0; i < (out_ > 0 ? out_ : 0); ++i)
            out[i] = 0.0;
        return false;
    }

    std::vector<double> a(static_cast<size_t>(hidden_[0]));
    std::vector<double> b(static_cast<size_t>(hidden_[1]));

    for (int o = 0; o < hidden_[0]; ++o) {
        const double* row = &w1_[static_cast<size_t>(o) * obs_];
        double sum = b1_[static_cast<size_t>(o)];
        for (int i = 0; i < obs_; ++i)
            sum += row[i] * obs[i];
        a[static_cast<size_t>(o)] = std::tanh(sum);
    }
    for (int o = 0; o < hidden_[1]; ++o) {
        const double* row = &w2_[static_cast<size_t>(o) * hidden_[0]];
        double sum = b2_[static_cast<size_t>(o)];
        for (int i = 0; i < hidden_[0]; ++i)
            sum += row[i] * a[static_cast<size_t>(i)];
        b[static_cast<size_t>(o)] = std::tanh(sum);
    }
    for (int o = 0; o < out_; ++o) {
        const double* row = &w3_[static_cast<size_t>(o) * hidden_[1]];
        double sum = b3_[static_cast<size_t>(o)];
        for (int i = 0; i < hidden_[1]; ++i)
            sum += row[i] * b[static_cast<size_t>(i)];
        const double value = std::tanh(sum);
        out[o] = std::isfinite(value) ? value : 0.0;  // a broken net idles, it doesn't slam
    }
    return true;
}

}  // namespace mxrider
