// Сессия лупера (этап 7а): описание дорожек и звук в 24-битных WAV.
// Всё здесь — не для аудиопотока: файлы, строки, выделение памяти.
// Сохранение и загрузку по шагам ведёт Looper (session_* в looper.h), здесь — формат и файлы.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "pool.h"

namespace alp {

constexpr int kSessionTracks = 36;  // = kTracks (сетка до 6×6)

// Состояние сохранения и загрузки для виджета. Ошибки — от 10.
enum SessStatus : int {
  SS_IDLE = 0,
  SS_WAIT = 1,     // Save ждёт, пока закроются запись и овердаб
  SS_SAVING = 2,
  SS_SAVED = 3,
  SS_LOADING = 4,
  SS_LOADED = 5,
  SS_E_MEMORY = 10,   // сессия не помещается в пул
  SS_E_RATE = 11,     // другая частота дискретизации
  SS_E_MISSING = 12,  // нет файла звука
  SS_E_FORMAT = 13,   // файл не тот (не наш WAV или другая длина)
  SS_E_WRITE = 14,    // не удалось записать (место на карте?)
  SS_E_READ = 15,     // ошибка чтения посреди загрузки
  SS_E_TIMEOUT = 16,  // запись или овердаб так и не закрылись: звук не сохранён
};

struct SessionTrack {
  bool used = false;      // есть звук
  bool locked = false;
  bool muted = false;
  bool reversed = false;
  bool synced = false;    // записана в Quantize: фаза привязана к сетке
  uint64_t len = 0;       // длина цикла, кадров
  float gain = 1.0f;      // файл записан с громкостью 1/gain (сумма слоёв была громче 0 дБ)
  double loop_beats = 0;  // длина цикла в долях
  double rec_bpm = 0;     // темп при записи (для следования темпу)
  double b0 = 0;          // доля начала цикла на сетке сохранённой сессии
  int32_t buf = -1;       // во время работы: буфер в пуле (в описание не пишется)
  char file[64] = {};     // имя файла в папке сессии
};

struct SessionDesc {
  uint64_t id = 0;   // своё у каждого сохранения
  double sr = 0;
  int grid = 4, part = 0, active = 0;
  SessionTrack t[kSessionTracks];
  int used_tracks() const;
};

// Описание сессии — текст (хранится в LV2-состоянии пресета).
std::string session_to_text(const SessionDesc& d);
bool session_from_text(const char* txt, size_t n, SessionDesc& d);

// Имя файла дорожки k при сетке grid: «part1 track2.wav».
void session_file_name(int k, int grid, char* out, size_t n);
uint64_t session_new_id();

// Можно ли загрузить: файлы на месте, та же частота, длины совпадают, помещается в пул.
// SS_IDLE — можно, иначе код ошибки.
int session_check(const SessionDesc& d, const char* dir, double sr, uint32_t pool_chunks);
// Сколько кусков пула займёт сессия.
uint32_t session_chunks(const SessionDesc& d);

// Самый громкий сэмпл буфера.
float buf_peak(const Pool& p, int32_t buf, uint64_t len);
// Стерео WAV 24 бит; сэмплы делятся на gain. progress: прибавляет записанные кадры.
bool wav24_write(const char* path, const Pool& p, int32_t buf, uint64_t len, double sr, float gain,
                 std::atomic<uint64_t>* progress);
// Заголовок WAV: SS_IDLE и частота, кадры, начало данных — или код ошибки.
int wav24_probe(const char* path, double& sr, uint64_t& frames, uint64_t& data_off);
// Прочитать len кадров в буфер пула (куски выделяются здесь), умножив на gain.
// cancelled() проверяется на каждом куске: true — бросить. SS_IDLE или код ошибки.
int wav24_read(const char* path, Pool& p, int32_t buf, uint64_t len, float gain, const std::function<bool()>& cancelled,
               std::atomic<uint64_t>* progress);

}  // namespace alp
