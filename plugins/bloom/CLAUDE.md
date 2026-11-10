# zynthian-generative-arp

## Цель
Генеративный арпеджиатор: LV2 MIDI-плагин для Zynthian (Vangelis 2610) на Raspberry Pi 5.
Общие настройки: через контроллеры Zynthian, по 4 на страницу.
Шаги паттерна (этап 4+): редактируются в своём виджете на тачскрине (как у лупера, файл в `widgets_user`),
значения шагов — LV2-параметры (`patch:writable` + `notOnGUI`, приходят как `patch:Set` в `midi_in`),
их сохраняет снапшот Zynthian. НЕ скрытые порты: jalv в Zynthian не регистрирует порты с `notOnGUI`,
и значения до плагина не доходят (проверено на устройстве).

## Образец
`../zynthian-midi-looper`: мой рабочий LV2-плагин на том же стеке.
Перед тем как писать новое, смотри, как там сделано: обработка тиков и транспорта,
.ttl-файлы, порты и контроллеры, Makefile, скрипт установки. Копируй подход, не изобретай заново.
`../midi-looper` к проекту НЕ относится (это прошивка для ESP32), не смотри туда.

## Правила
- Язык C, LV2.
- Разрешение: 960 тиков на долю.
- Синхронизация с транспортом через `time:Position`.
- В аудио-потоке (`run()`): никаких malloc/free, блокировок, printf, файлового ввода-вывода.
- Логика арпеджиатора в отдельных файлах (`arp_core.c/.h`) без зависимостей от LV2,
  чтобы её можно было тестировать в WSL обычной программой.
- Ничего не менять в файлах самого Zynthian на устройстве, ставить только свой плагин.
- Работать маленькими шагами. Не делать следующий этап, пока текущий не проверен.

## Сборка и установка
По образцу zynthian-midi-looper: без Makefile, сборка скриптами в `scripts/`.

- Зависимости: `gcc`, `pkg-config`, `lv2-dev` (в WSL ещё `lilv-utils` для `lv2ls`/`lv2info`).
- Плагин: URI `urn:generativearp:arp`, бандл `generative_arp.lv2`, имя в Zynthian «Generative Arp».
- Исходники: `src/generative_arp.c` (обёртка LV2, транспорт), `src/arp_core.c/.h` (логика без LV2),
  `lv2/manifest.ttl`, `lv2/generative_arp.ttl`, виджет `ui/zynthian_widget_generative_arp.py`
  (Zynthian находит его по имени плагина: «Generative Arp» → `zynthian_widget_generative_arp.py`).
- Сборка: `./scripts/build.sh` → `build/generative_arp.lv2/` (ttl + `generative_arp.so`).
- Тесты логики (WSL, без LV2): `./scripts/test.sh` → собирает `build_test/test_arp` из `test/test_arp.c` + `arp_core.c`.
- Виджет без Zynthian (WSL, нужен `python3-tk`): `python3 test/widget_preview.py` — сценарий касаний с проверками;
  `--show` — открыть окно и потрогать руками (заглушки модулей zyngui внутри скрипта).
- Установка на Zynthian (собирается прямо на устройстве):
  ```bash
  git clone <repo> && cd zynthian-generative-arp
  sudo ./scripts/install.sh        # build + symlinks: /usr/local/lib/lv2/generative_arp.lv2 -> build/generative_arp.lv2,
                                   # /zynthian/zynthian-ui/zyngui/widgets_user/zynthian_widget_generative_arp.py -> ui/...
  sudo systemctl restart zynthian
  ```
  Каталог можно переопределить: `LV2_DIR=... ./scripts/install.sh`.
  После пересборки (`./scripts/build.sh`) достаточно перезапустить zynthian: ссылка уже указывает на `build/`.
- Пресеты: стандартные LV2 (список и «Save preset» в Zynthian). Плагин хранит шаги паттерна через LV2 State
  и после загрузки отвечает на `patch:Get` значениями шагов, чтобы Zynthian и виджет их увидели.
  Заводские пресеты генерирует `scripts/gen_presets.py` → `lv2/presets.ttl` и `lv2/manifest.ttl` (руками не править).
  После изменения пресетов или первой установки нужен `sudo ./scripts/install.sh`: он обновляет кэш списка
  пресетов Zynthian для этого плагина (штатная `zynthian_lv2.py presets <URI>`).
  Свои пресеты Zynthian (jalv) пишет в `zynthian-my-data/presets/lv2/Generative_Arp.presets.lv2/`.
  «New bank» в Zynthian дописывает банк в его `manifest.ttl`, но папку не создаёт (её создаёт только jalv
  при первом сохранении), поэтому без неё сохранение в новый банк молча падает. `install.sh` создаёт папку
  и manifest с префиксами, если их нет.
- Проверка: `lv2ls | grep generativearp`, `lv2info urn:generativearp:arp`.
- В Zynthian: добавить в цепочку как MIDI tool перед синтом.
- Удаление: `sudo ./scripts/uninstall.sh` (удаляет только ссылки).

## План этапов
1. Пустой плагин: собирается, ставится, пропускает MIDI насквозь.
2. Синхронизация с транспортом + простое арпеджио вверх.
3. Глобальные параметры: скорость, gate, порядок нот.
4. Вероятности и сдвиги по шагам.
5. Полиритмия.
6. Мутация и гамма.

## Отладка
Логи на устройстве: `journalctl -u zynthian`.
Тест логики без железа: программа в `test/`, которая подаёт ноты и тики в arp_core
и печатает выходящие события.
