// Стресс-тест API движка: всё, что игра может делать из задач, сразу и вперемешку.
// Спавн/удаление/RemoveBody, KillZ, лучи из ParallelFor, свет, переключение RT/теней/MSAA,
// пауза/убийство задач, исключение в задаче, фоновая Extreme-задача с мешами.
// Запуск: StressTest [секунды] [воркеры]. Удобно гонять под ASan/TSan и с валидацией синхронизации:
//   VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
#include <CustomEngine.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

static float g_seconds = 20;

class Stress : public Game {
public:
    std::vector<Entity> spawned;
    std::vector<Light> lights;
    TaskHandle victim;
    Mesh cube, ball;

    void OnStart() override {
        World.SetBounds({-30, -10, -30}, {30, 60, 30});
        Camera.SetPosition({0, 8, 25});
        Camera.LookAt({0, 2, 0});
        cube = Mesh::Cube();
        ball = Mesh::Sphere(12);
        World.Spawn().SetMesh(Mesh::Plane(60)).AddBody({BodyType::Static, Shape::Plane()});
        for (int i = 0; i < 40; ++i)
            World.Spawn().SetMesh(cube).SetPosition({(i % 8) * 1.1f - 4, 0.5f + (i / 8) * 1.0f, 0})
                .AddBody({BodyType::Dynamic, Shape::Box({0.5f, 0.5f, 0.5f})}).SetOutOfBounds(OutOfBounds::Respawn);
        World.Spawn().SetPosition({0, 1, -5}).AddBody({BodyType::Static, Shape::Box({3, 1, 1}), 0, 0.5f, 0, 0, 0, true});
        Grave.OnContact([](const ContactEvent& e) {
            if (e.trigger && e.a.Valid() && e.b.Valid()) { volatile float x = e.a.Position().x; (void)x; }
        });

        // Стрельба и удаление из задачи
        UniX.CreateTask.NewTask("Spawner", Cost::Low, Priority::Normal, [this](Task& t) {
            auto& rng = t.State<std::mt19937>();
            std::uniform_real_distribution<float> U(-1, 1);
            for (int k = 0; k < 3; ++k) {
                Entity e = World.Spawn().SetMesh(ball).SetScale(0.4f).SetPosition({U(rng) * 5, 6 + U(rng), 10})
                               .AddBody({BodyType::Dynamic, Shape::Sphere(0.2f), 1.0f})
                               .SetVelocity({U(rng) * 20, U(rng) * 10, -25});
                spawned.push_back(e);
            }
            if (spawned.size() > 150) {  // часть удаляем вручную, часть уберёт KillZ
                for (int k = 0; k < 20; ++k) spawned[k].Destroy();
                spawned.erase(spawned.begin(), spawned.begin() + 20);
            }
            if (t.Frame() % 37 == 0 && !spawned.empty()) spawned.back().RemoveBody();
            if (t.Frame() % 41 == 0 && !spawned.empty()) spawned.front().SetMesh(cube).SetVisible(t.Frame() % 2);
        });
        // Лучи параллельно
        UniX.CreateTask.NewTask("Rays", Cost::High, Priority::Normal, [](Task& t) {
            std::atomic<uint32_t> hits{0};
            t.ParallelFor(2048, [&](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) {
                    RaycastHit h;
                    float a = i * 0.01f;
                    if (Grave.Raycast(Camera.Position(), {std::sin(a), -0.3f, -std::cos(a)}, 100, h) && h.entity.Valid())
                        hits++;
                }
            });
        });
        // Свет: добавление/удаление
        UniX.CreateTask.NewTask("Lights", Cost::Low, Priority::Low, [this](Task& t) {
            if (t.Frame() % 5 == 0) lights.push_back(Amper.AddPointLight({std::sin((float)t.Time()) * 8, 3, 0}, {1, 0.5f, 0.2f}, 20, 8, t.Frame() % 2 == 0));
            if (lights.size() > 300) { for (int k = 0; k < 100; ++k) lights[k].Remove(); lights.erase(lights.begin(), lights.begin() + 100); }
            for (auto& l : lights) if (l.Valid()) l.SetIntensity(10 + (t.Frame() % 10));
        });
        // Переключатели рендера
        UniX.CreateTask.NewTask("Toggles", Cost::Low, Priority::High, [](Task& t) {
            const uint64_t f = t.Frame();
            if (f % 90 == 0) Amper.SetRayTracing(!Amper.RayTracing());
            if (f % 70 == 0) Amper.SetShadows(!Amper.Shadows());
            if (f % 110 == 0) { uint32_t n = Amper.Antialiasing() * 2; while (n <= Amper.MaxAntialiasing() && !Amper.AntialiasingSupported(n)) n *= 2; Amper.SetAntialiasing(n > Amper.MaxAntialiasing() ? 1 : n); }
            if (f % 130 == 0) Amper.SetShadowDistance(20 + (f % 3) * 40.0f);
            if (t.Time() > g_seconds) Engine::Quit();
        });
        // Задача, которую убивают/ставят на паузу, и задача, которая бросает исключение
        victim = UniX.CreateTask.NewTask("Victim", Cost::Medium, Priority::Low, [](Task& t) { t.State<int>()++; });
        UniX.CreateTask.NewTask("Killer", Cost::Low, Priority::Low, [this](Task& t) {
            if (t.Frame() % 50 == 10) victim.Pause();
            if (t.Frame() % 50 == 20) victim.Resume();
            if (t.Frame() == 300) victim.Kill();
            if (t.Frame() == 200) UniX.CreateTask.NewTask("Thrower", Cost::Low, Priority::Low, [](Task&) { throw std::runtime_error("test"); });
        });
        // Фоновая Extreme-задача: создаёт меши и сущности, ParallelFor внутри
        UniX.CreateTask.Once("Background", Cost::Extreme, Priority::Low, [this](Task& t) {
            for (int round = 0; round < 6 && !t.Cancelled(); ++round) {
                Mesh m = Mesh::Capsule(0.2f + 0.05f * round, 0.5f, 12);
                std::vector<float> data(10000);
                t.ParallelFor(10000, [&](uint32_t b, uint32_t e) { for (uint32_t i = b; i < e; ++i) data[i] = std::sqrt((float)i); });
                World.Spawn().SetMesh(m).SetPosition({-10.0f + static_cast<float>(round) * 3, 3, -8}).AddBody({BodyType::Dynamic, Shape::Capsule(0.2f, 0.5f)});
                std::vector<Vertex> v = {{{0, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 1, 0}}, {{0, 0, 1}, {0, 1, 0}}};
                std::vector<uint32_t> idx = {0, 2, 1};
                Mesh tri = Mesh::Create(v, idx);
                World.Spawn().SetMesh(tri).SetPosition({0, 0.01f, 3.0f + static_cast<float>(round)});
            }
        });
        // Камера двигается
        UniX.CreateTask.NewTask("Cam", Cost::Low, Priority::High, [](Task& t) {
            float a = (float)t.Time() * 0.3f;
            Camera.SetPosition({std::sin(a) * 25, 8, std::cos(a) * 25});
            Camera.LookAt({0, 2, 0});
        });
    }
};

int main(int argc, char** argv) {
    if (argc > 1) g_seconds = std::atof(argv[1]);
    EngineConfig c;
    c.width = 640; c.height = 360;
    c.rayTracing = true;
    c.configFile = "";
    c.workerThreads = argc > 2 ? std::atoi(argv[2]) : 0;
    return Engine::Run<Stress>(c);
}
