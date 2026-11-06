#include "../src/audio_buffer.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void drift(double ppm) {
    constexpr unsigned rate = 48000;
    audio::Buffer fifo(12000, 1440);
    std::vector<float> out(960);
    double fraction = 0;
    size_t silentAfterWarmup = 0;
    // Ten minutes of independent clocks at each tested offset, no wall-clock wait.
    for (int tick = 0; tick < 60000; ++tick) {
        fraction += 480 * (1 + ppm / 1000000);
        const auto frames = static_cast<size_t>(fraction);
        fraction -= static_cast<double>(frames);
        for (size_t i = 0; i < frames; ++i) fifo.push({0.25f, -0.25f});
        fifo.render(out.data(), 480, rate);
        for (size_t i = 0; i < out.size(); i += 2) {
            require(std::isfinite(out[i]) && std::isfinite(out[i + 1]), "nonfinite output");
            if (tick > 1000 && out[i] == 0) ++silentAfterWarmup;
            if (out[i] != 0) require(out[i] == 0.25f && out[i + 1] == -0.25f, "stereo signal changed");
        }
        require(fifo.size() < 12000, "FIFO exceeded capacity");
    }
    require(silentAfterWarmup == 0, "clock drift caused starvation or overrun");
    require(fifo.size() > 400 && fifo.size() < 3000, "clock drift accumulated latency");
    require(std::abs(fifo.ratio() - (1 + ppm / 1000000)) < 0.00015, "ratio did not track clock drift");
}
int main() {
    try {
        audio::Buffer fifo(64, 8);
        float out[32];
        std::fill(std::begin(out), std::end(out), 42.0f);
        fifo.render(out, 16, 48000);
        for (float f : out) require(f == 0, "empty FIFO must produce silence");
        for (int i = 0; i < 16; ++i) fifo.push({0.5f, -0.5f});
        fifo.render(out, 4, 48000);
        require(out[0] == 0.5f && out[1] == -0.5f, "channels crossed");
        fifo.clear();
        require(fifo.size() == 0 && fifo.ratio() == 1, "clear did not reset state");
        for (int i = 0; i < 10000; ++i) fifo.push({1, 1});
        require(fifo.size() <= 64, "unbounded overrun");
        fifo.clear();
        for (int i = 0; i < 10; ++i) fifo.push({0.5f, -0.5f});
        fifo.render(out, 16, 48000);
        require(out[30] == 0 && out[31] == 0, "underrun did not emit silence");
        for (double ppm : {-2000.0, -500.0, 0.0, 500.0, 2000.0}) drift(ppm);
        std::cout << "FIFO bounds, silence, stereo, reset, underrun, and 50 simulated minutes of clock drift passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
