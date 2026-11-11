# Eka Probe: можно ли хранить звук сессии Eka Tape в пресетах Zynthian

Временный эксперимент, не плагин набора. После проверки папку удаляем.

Eka Probe пропускает звук насквозь. При сохранении пресета или снапшота он пишет тестовый WAV
(размер задаёт крутилка **Test MB**, по умолчанию 100 МБ), при загрузке читает его обратно и сверяет.
Всё, что произошло, он пишет в `/tmp/eka-probe.log` (лог стирается при перезагрузке Pi).

## Итоги пробы 1 (2026-11-11)
- jalv даёт `makePath`, файл пишется прямо в `presets/lv2/Eka_Probe.presets.lv2/`, без копии.
- Сохранение идёт в отдельном потоке, звук не прерывается; 1000 МБ за 37 с, интерфейс не висит.
- Загрузка останавливает звук целиком (`run() calls during restore=0`): 80 мс на 100 МБ, 780 мс на 1000 МБ
  (и это из кэша в памяти).
- Все пресеты плагина в одной папке: файл с одним именем затирался другими пресетами.
- Zynthian загружает пресет сам сразу после сохранения и, похоже, при листании списка.

## Проба 2: что выясняем
1. С отметкой `threadSafeRestore` jalv больше не останавливает звук при загрузке?
   Ждём `run() calls during restore` больше 0 и `longest audio gap` около 5 мс.
2. У каждого сохранения свой файл (`eka-probe-<время>-<номер>.wav`): пресеты больше не затирают друг друга?
3. Попадает ли файл в снапшот Zynthian.
4. Сколько длится чтение с SD-карты, а не из памяти.

## Установка (на Pi)
```bash
cd /root/zynthian-eka && git pull
cd experiments/state-probe && sudo make install
sudo systemctl restart zynthian
```
После перезапуска добавьте Eka Probe аудиоэффектом в цепочку, где идёт звук.
Проверка, что он загружен: в `ps aux | grep -i [j]alv` есть строка с `experiments/state-probe`.

## Проверка
Перед началом очистите лог: `: > /tmp/eka-probe.log`

**А. Загрузка без остановки звука**
1. Test MB = 100. Сохраните новый пресет Eka Probe `p100`.
2. Test MB = 1000. Сохраните новый пресет `p1000`.
3. Загрузите `p100`, потом `p1000`, потом снова `p100`. Слышны ли провалы звука?

**Б. Чтение с SD-карты**
1. Сбросьте кэш файлов в памяти (безопасно, только замедлит следующие чтения):
   `sync; echo 3 > /proc/sys/vm/drop_caches`
2. Загрузите `p1000`. Слышен ли провал?

**В. Снапшот**
1. Test MB = 100. Сохраните снапшот (Save snapshot), имя `probe-snap`.
2. Перезапустите Zynthian: `sudo systemctl restart zynthian`
3. Загрузите снапшот `probe-snap`.

## Что прислать мне
```bash
cat /tmp/eka-probe.log
ls -la /zynthian/zynthian-my-data/presets/lv2/Eka_Probe.presets.lv2/
ls -lat /zynthian/zynthian-my-data/snapshots/ | head -5
```
И коротко словами: были ли провалы звука в А и Б.

## Удаление после проверки
```bash
cd /root/zynthian-eka/experiments/state-probe && sudo make uninstall
rm -rf /zynthian/zynthian-my-data/presets/lv2/Eka_Probe.presets.lv2
sudo systemctl restart zynthian
```
