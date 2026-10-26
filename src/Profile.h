#pragma once
// Профилирование через Tracy (https://github.com/wolfpld/tracy).
// Включается при сборке: cmake -DCE_PROFILE=ON. Без опции все макросы пустые и ничего не стоят.
//
//   CE_ZONE("Имя")             — зона на текущую область видимости (имя — строковый литерал)
//   CE_ZONE_NAMED(str, len)    — зона с именем, известным только во время выполнения (имя задачи)
//   CE_FRAME_MARK()            — граница кадра
//   CE_THREAD_NAME("Имя")      — подпись потока в Tracy
#if defined(CE_PROFILE) && CE_PROFILE
#include <tracy/Tracy.hpp>
#define CE_ZONE(name) ZoneScopedN(name)
#define CE_ZONE_NAMED(str, len) \
    ZoneScoped;                 \
    ZoneName(str, len)
#define CE_FRAME_MARK() FrameMark
#define CE_THREAD_NAME(name) tracy::SetThreadName(name)
#else
#define CE_ZONE(name) ((void)0)
#define CE_ZONE_NAMED(str, len) ((void)0)
#define CE_FRAME_MARK() ((void)0)
#define CE_THREAD_NAME(name) ((void)0)
#endif
