#pragma once
// Сэмплинг анимаций glTF на CPU: поза = translation/rotation/scale каждого узла.
// Работает в стадии Prepare параллельно по аниматорам (ParallelFor в Amper.Prepare).
#include "Amper/ModelRegistry.h"

#include <vector>

namespace ce::core {

struct Pose {
    std::vector<Vec3> t;
    std::vector<Quat> r;
    std::vector<Vec3> s;
};

void RestPose(const ModelData& model, Pose& out);                         // поза по умолчанию
void SampleClip(const AnimClip& clip, float time, Pose& inOut);           // перезаписывает анимированные каналы
void BlendPose(Pose& a, const Pose& b, float w);                          // a = mix(a, b, w)
void ComputeGlobals(const ModelData& model, const Pose& pose, std::vector<Mat4>& globals);

} // namespace ce::core
