// Нагрузочная сцена: сетка из N кубов на большой площади, камера смотрит на её часть.
// Печатает среднее CPU-время кадра (UniX), FPS и число draw/инстансов, затем выходит.
//
//   SceneBench [side] [seconds] [rt|nort] [shadows|noshadows]
//     side    — сторона сетки (по умолчанию 100 → 10 000 кубов)
//     seconds — длительность замера (по умолчанию 6, первые 2 — прогрев)
#include <CustomEngine.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int g_side = 100;
double g_seconds = 6.0;
bool g_rt = false;
bool g_shadows = true;
} // namespace

class SceneBench : public Game {
public:
    void OnStart() override {
        Amper.SetRayTracing(g_rt);
        Amper.SetShadows(g_shadows);
        Camera.SetPosition({0, 6, 0});
        Camera.SetAngles(0.0f, -0.25f);

        World.Spawn().SetMesh(Mesh::Plane(2000)).SetMaterial({{0.3f, 0.32f, 0.34f}, 0.9f});
        const Mesh cube = Mesh::Cube();
        const float step = 4.0f;
        const float half = g_side * step * 0.5f;
        for (int z = 0; z < g_side; ++z)
            for (int x = 0; x < g_side; ++x)
                World.Spawn()
                    .SetMesh(cube)
                    .SetPosition({x * step - half, 0.5f, z * step - half})
                    .SetColor({0.4f + 0.5f * (x % 3) / 2.0f, 0.5f, 0.4f + 0.5f * (z % 3) / 2.0f});

        UniX.CreateTask.NewTask("Bench", Cost::Low, Priority::High, [](Task& t) {
            struct S {
                double sumMs = 0;
                uint32_t frames = 0;
                double t0 = -1;
            };
            S& s = t.State<S>();
            if (t.Time() < 2.0)
                return;
            if (s.t0 < 0)
                s.t0 = t.Time();
            s.sumMs += UniX.FrameWorkMs();
            ++s.frames;
            if (t.Time() - s.t0 >= g_seconds - 2.0) {
                std::printf("[bench] entities %u | draws %u | instances %u | CPU %.3f ms | FPS %.1f\n", World.Count(),
                            Amper.DrawCalls(), Amper.Instances(), s.sumMs / s.frames,
                            s.frames / (t.Time() - s.t0));
                Engine::Quit();
            }
        });
    }
};

int main(int argc, char** argv) {
    if (argc > 1) g_side = std::atoi(argv[1]);
    if (argc > 2) g_seconds = std::atof(argv[2]);
    if (argc > 3) g_rt = std::strcmp(argv[3], "rt") == 0;
    if (argc > 4) g_shadows = std::strcmp(argv[4], "noshadows") != 0;
    EngineConfig cfg;
    cfg.title = "SceneBench";
    cfg.vsync = false;
    cfg.rayTracing = g_rt;
    return Engine::Run<SceneBench>(cfg);
}
