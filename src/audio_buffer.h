#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace audio {
struct Stereo { float left = 0, right = 0; };

// Single-threaded bounded FIFO. Linear interpolation only makes the tiny clock
// correction; Windows performs the full sample-rate / endpoint conversion.
class Buffer {
    std::vector<Stereo> data_;
    size_t head_ = 0, size_ = 0;
    double phase_ = 0, ratio_ = 1;
    double target_;
    bool primed_ = false;
public:
    Buffer(size_t capacity, size_t target) : data_(capacity), target_(static_cast<double>(target)) {}
    size_t size() const { return size_; }
    double ratio() const { return ratio_; }
    void clear() { head_ = size_ = 0; phase_ = 0; ratio_ = 1; primed_ = false; }
    void push(Stereo frame) {
        if (size_ == data_.size()) {
            // Emergency overrun only: bound latency, then re-prime. This is not
            // the normal clock synchronization mechanism.
            clear();
        }
        data_[(head_ + size_) % data_.size()] = frame;
        ++size_;
    }
    void render(float* out, size_t frames, unsigned sampleRate) {
        if (!primed_ && static_cast<double>(size_) >= target_ + 2) primed_ = true;
        // Low-pass the fill error and slew the ratio over seconds. +/-0.5%
        // accommodates independent endpoint clocks without accumulating delay.
        const double desired = 1 + std::clamp((static_cast<double>(size_) - target_) /
                                             (sampleRate * 5.0), -0.005, 0.005);
        const double smoothing = 1 - std::exp(-static_cast<double>(frames) / (sampleRate * 2.0));
        ratio_ += (desired - ratio_) * smoothing;
        for (size_t i = 0; i < frames; ++i) {
            Stereo result{};
            if (primed_ && size_ >= 2) {
                const auto a = data_[head_];
                const auto b = data_[(head_ + 1) % data_.size()];
                const float fraction = static_cast<float>(phase_);
                result = { a.left + (b.left - a.left) * fraction,
                           a.right + (b.right - a.right) * fraction };
                phase_ += ratio_;
                size_t consumed = std::min(static_cast<size_t>(phase_), size_);
                head_ = (head_ + consumed) % data_.size();
                size_ -= consumed;
                phase_ -= static_cast<double>(consumed);
            } else if (primed_) {
                // Silence rather than replaying stale samples. Require a short
                // refill after starvation (also covers source idle periods).
                clear();
            }
            out[2 * i] = result.left;
            out[2 * i + 1] = result.right;
        }
    }
};
}
