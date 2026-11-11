# zynthian-eka

Набор LV2-плагинов Eka для Zynthian (Raspberry Pi, ARM Linux). Монорепо: у каждого плагина своя папка и своя сборка.

## Плагины (бренд Eka)
| Папка | Название | Что делает | Старый репозиторий |
|---|---|---|---|
| plugins/tape | Eka Tape | audio looper | zynthian-audio-looper |
| plugins/roll | Eka Roll | MIDI looper | zynthian-midi-looper |
| plugins/bloom | Eka Bloom | generative arpeggiator | zynthian-generative-arp |
| plugins/mist | Eka Mist | granular sampler | zynthian-granular-sampler |

- common/: общий код для плагинов (пока пусто)
- Makefile в корне: `make <плагин>` собирает один, `make` все; `make install-<плагин>` / `make uninstall-<плагин>` ставит или удаляет один, `make install` / `make uninstall` все (на Zynthian через sudo)
- В каждом плагине: бандл `eka-<папка>.lv2`, бинарник `eka_<папка>.so`, виджет `zynthian_widget_eka_<папка>.py` (Zynthian находит виджет по doap:name)

## Правила
- Плагины должны собираться независимо. Не пересобирай все, когда меняется один. Правки в common/ могут задевать несколько плагинов, говори об этом.
- В каждом плагине должен быть свой Makefile с целями all и install. Если плагин собирается через CMake, скрипт или gcc вручную, делай тонкий Makefile-обёртку, не переписывай сборку.
- Цель сборки: Zynthian на ARM. Не добавляй зависимости, которых нет на Zynthian, без согласования.
- Старые репозитории не трогай. Репозиторий midi-looper (ESP32) к этому проекту не относится.
- Перед git commit показывай, что меняется. git push только по моей просьбе.
- Одна задача за раз, после каждой показывай итог и жди моего ответа.

## LV2 URI
URI вида `http://dmtry3.github.io/zynthian-eka/<папка>` (tape, roll, bloom, mist), переименование сделано 2026-11-11. URI больше не менять: они хранятся в пресетах и снапшотах пользователей.

## Текущие задачи
Нет. Новые задачи даёт владелец.

Сделано 2026-11-11:
- Переименование в Eka: doap:name, URI, бандлы, бинарники, виджеты, README, docs. install.sh убирает ссылки со старыми именами и переносит данные (слоты roll, свои сэмплы mist).
- Сборка: в каждом плагине Makefile-обёртка над scripts/ (all, install, uninstall, test где есть, clean).

## О владельце
Я новичок в Claude Code и работаю в WSL Ubuntu. Объясняй коротко, что и зачем ты делаешь.
