# zynthian-sono

Набор LV2-плагинов Sono для Zynthian (Raspberry Pi, ARM Linux). Монорепо: у каждого плагина своя папка и своя сборка.

## Плагины (бренд Sono)
| Папка | Название | Что делает | Старый репозиторий |
|---|---|---|---|
| plugins/tape | Sono Tape | audio looper | zynthian-audio-looper |
| plugins/roll | Sono Roll | MIDI looper | zynthian-midi-looper |
| plugins/bloom | Sono Bloom | generative arpeggiator | zynthian-generative-arp |
| plugins/mist | Sono Mist | granular sampler | zynthian-granular-sampler |

- common/: общий код для плагинов (пока пусто)
- Makefile в корне: `make <плагин>` собирает один, `make` все, `make install-<плагин>` ставит один

## Правила
- Плагины должны собираться независимо. Не пересобирай все, когда меняется один. Правки в common/ могут задевать несколько плагинов, говори об этом.
- В каждом плагине должен быть свой Makefile с целями all и install. Если плагин собирается через CMake, скрипт или gcc вручную, делай тонкий Makefile-обёртку, не переписывай сборку.
- Цель сборки: Zynthian на ARM. Не добавляй зависимости, которых нет на Zynthian, без согласования.
- Старые репозитории не трогай. Репозиторий midi-looper (ESP32) к этому проекту не относится.
- Перед git commit показывай, что меняется. git push только по моей просьбе.
- Одна задача за раз, после каждой показывай итог и жди моего ответа.

## LV2 URI
Плагины переименовываются один раз, сейчас. Новые URI вида `http://dmtry3.github.io/zynthian-sono/<папка>` (tape, roll, bloom, mist). После переименования URI больше не менять: они хранятся в пресетах пользователей.

## Текущие задачи (по порядку)
1. Переименование. В каждом плагине заменить старые имена на новые: doap:name ("Sono Tape" и т.д.), LV2 URI, имя бандла (sono-tape.lv2 и т.д.), имя бинарника, ссылки в manifest.ttl, README и комментариях. Перед правками покажи список всех мест, где встречается старое имя (grep), и жди моего подтверждения.
2. Сборка. В plugins/tape, roll, bloom, mist нет Makefile (make выдаёт "No targets specified and no makefile found"). Выясни, как собирается каждый плагин, и добавь Makefile, чтобы `make tape`, `make roll`, `make bloom`, `make mist` из корня работали.

## О владельце
Я новичок в Claude Code и работаю в WSL Ubuntu. Объясняй коротко, что и зачем ты делаешь.
