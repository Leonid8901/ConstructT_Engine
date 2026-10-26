// Микробенчмарк планировщика UniX без окна и GPU.
//
//   UniXBench [workers] [scenario] [frames]
//     workers  — число воркеров (по умолчанию 6, как у 8-поточного CPU)
//     scenario — tiny | pfor | mixed | all (по умолчанию all)
//     frames   — кадров на замер (по умолчанию 600, плюс 120 на прогрев)
//
// tiny  — 2000 мелких задач Cost::Low (~2 мкс каждая): накладные расходы на задачу
// pfor  — 4 задачи Cost::High с ParallelFor на 200 000 элементов: нарезка и раздача кусков
// mixed — 500 мелких + 2 ParallelFor в Update и Actor-задача с ParallelFor в Simulate
#include "Core.h"
#include "UniX/Scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ce;
using namespace ce::core;

namespace {

std::atomic<uint64_t> g_sink{0};

// Детерминированная «работа», которую компилятор не выбросит
inline uint64_t Spin(uint32_t iters, uint64_t seed) {
    uint64_t x = seed | 1;
    for (uint32_t i = 0; i < iters; ++i)
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return x;
}

struct Result {
    double meanMs = 0, p50Ms = 0, p95Ms = 0;
};

Result Measure(Scheduler& s, uint32_t frames, bool simulate) {
    using clock = std::chrono::steady_clock;
    std::vector<double> samples;
    samples.reserve(frames);
    const uint32_t warmup = 120;
    double t = 0;
    for (uint32_t f = 0; f < warmup + frames; ++f) {
        s.BeginFrame(t, 1.0f / 60.0f, f + 1);
        auto t0 = clock::now();
        s.RunStage(Stage::Update);
        if (simulate)
            s.RunStage(Stage::Simulate);
        auto t1 = clock::now();
        s.EndFrame();
        t += 1.0 / 60.0;
        if (f >= warmup)
            samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    Result r;
    for (double v : samples)
        r.meanMs += v;
    r.meanMs /= static_cast<double>(samples.size());
    std::sort(samples.begin(), samples.end());
    r.p50Ms = samples[samples.size() / 2];
    r.p95Ms = samples[samples.size() * 95 / 100];
    return r;
}

void AddTiny(Scheduler& s, std::vector<std::shared_ptr<TaskRecord>>& keep, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        keep.push_back(s.Create("tiny" + std::to_string(i), Cost::Low, Priority::Normal, Significance::Task,
                                Stage::Update, [i](Task&) { g_sink.fetch_add(Spin(150, i), std::memory_order_relaxed); },
                                false));
}

void AddPFor(Scheduler& s, std::vector<std::shared_ptr<TaskRecord>>& keep, uint32_t n, Significance sig, Stage stage) {
    for (uint32_t i = 0; i < n; ++i)
        keep.push_back(s.Create("pfor" + std::to_string(i), Cost::High, Priority::Normal, sig, stage,
                                [i](Task& t) {
                                    std::atomic<uint64_t> acc{0};
                                    t.ParallelFor(200000, [&](uint32_t b, uint32_t e) {
                                        uint64_t local = 0;
                                        for (uint32_t k = b; k < e; ++k)
                                            local += Spin(2, k + i);
                                        acc.fetch_add(local, std::memory_order_relaxed);
                                    });
                                    g_sink.fetch_add(acc.load(), std::memory_order_relaxed);
                                },
                                false));
}

void Kill(std::vector<std::shared_ptr<TaskRecord>>& keep) {
    for (auto& r : keep)
        r->alive.store(false);
    keep.clear();
}

} // namespace

int main(int argc, char** argv) {
    const uint32_t workers = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 6u;
    const std::string scenario = argc > 2 ? argv[2] : "all";
    const uint32_t frames = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 600u;

    EngineCore ec;
    SetCore(&ec);
    {
        Scheduler s(workers, 60.0f);
        ec.scheduler = &s;
        std::vector<std::shared_ptr<TaskRecord>> keep;

        auto run = [&](const char* name, bool simulate) {
            Result r = Measure(s, frames, simulate);
            std::printf("%-6s workers=%u  mean %.3f ms  p50 %.3f ms  p95 %.3f ms\n", name, workers, r.meanMs, r.p50Ms,
                        r.p95Ms);
            Kill(keep);
            Measure(s, 2, simulate);  // убрать мёртвые задачи
        };

        if (scenario == "tiny" || scenario == "all") {
            AddTiny(s, keep, 2000);
            run("tiny", false);
        }
        if (scenario == "pfor" || scenario == "all") {
            AddPFor(s, keep, 4, Significance::Task, Stage::Update);
            run("pfor", false);
        }
        if (scenario == "mixed" || scenario == "all") {
            AddTiny(s, keep, 500);
            AddPFor(s, keep, 2, Significance::Task, Stage::Update);
            AddPFor(s, keep, 1, Significance::Actor, Stage::Simulate);
            run("mixed", true);
        }
        ec.scheduler = nullptr;
    }
    SetCore(nullptr);
    std::printf("sink %llu\n", static_cast<unsigned long long>(g_sink.load() & 0xFF));
    return 0;
}
