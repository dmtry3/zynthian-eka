# Eka Grain Flow

Гранулярный эффект для живой игры: LV2-плагин для Zynthian (Raspberry Pi 5), будущий шестой плагин бандла
[zynthian-eka](https://github.com/dmtry3/zynthian-eka). ТЗ: `docs/TZ.md`.

## Правила

- Репозиторий **приватный, только на время разработки**. В публичный `zynthian-eka` ничего не пушим, пока плагин
  не готов. Потом папка целиком переезжает в `zynthian-eka/plugins/grainflow/`, поэтому структура и стиль такие же,
  как у соседей (Mist, Fold): `src/`, `lv2/`, `ui/`, `scripts/`, `test/`, `docs/`, Makefile-обёртка.
- Коммитить и пушить в `main` без вопросов.
- Язык C, чистый LV2, без новых зависимостей. В аудио-потоке нет выделения памяти и блокировок.
- `lv2/*.ttl` генерирует `scripts/gen_ttl.py` (его же вызывает `build.sh`): правим скрипт, не ttl.
  Порядок портов и параметров в скрипте и в `src/eka_grainflow.c` должен совпадать.
- Пользователь новичок в Claude Code: команды для него пишем целиком, по шагам.

## Сборка и тесты

- WSL / Linux: `make` (нужен `lv2-dev`), `make test`; `SAN=1 make test` с санитайзерами.
  Без установленных заголовков: `LV2_CFLAGS=-I/путь/к/lv2/include make test`.
- Тесты пишут wav-файлы в `build_test/` (послушать).
- Zynthian: `sudo make install`, затем `systemctl restart zynthian`. Что проверить на устройстве: `docs/CHECKLIST.md`.

## Устройство

- `src/grain_fx.c` — движок без LV2: кольцевой буфер, 3 потока, пул зёрен, Freeze, фильтр и микс.
- `src/eka_grainflow.c` — LV2-обёртка. Страницы ручек: Main (Stream, Mix, Feedback, Freeze), Grain, Flow и Out — для
  **выбранного** потока. Значения потоков — LV2-параметры `s1_offset` … `s3_level`, `sN_on`, `buffer`, `shape`;
  хранит их плагин, поворот ручки Grain/Flow/Out пишется в выбранный поток и уходит хосту через patch:Set.
  При смене потока виджет переставляет ручки на значения этого потока (плагин свои входные порты двигать не может).
- `ui/zynthian_widget_eka_grain_flow.py` — виджет: буфер, зёрна, вкладки потоков, Freeze.
