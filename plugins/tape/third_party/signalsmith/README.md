# Signalsmith Stretch (вендорено)

Независимые pitch и stretch для Eka Tape (этап 8). Лицензия MIT, тексты лицензий рядом.
Файлы скопированы без изменений, только нужные заголовки:

| Файл | Откуда | Версия |
|---|---|---|
| `signalsmith-stretch.h` | https://github.com/Signalsmith-Audio/signalsmith-stretch | тег 1.4.0, a670068d9aeb64913331d5cc29337b19a457a7df |
| `signalsmith-linear/stft.h`, `fft.h` | https://github.com/Signalsmith-Audio/linear | тег 0.6.6, 7bb1c8f36794d6980080ce817c91f8da8fe56a66 (эту версию указывает Stretch 1.4.0) |

БПФ встроенный (без pffft, Accelerate и т. п.), внешних зависимостей нет.
Подключение: `-I third_party/signalsmith`, затем `#include "signalsmith-stretch.h"`.
