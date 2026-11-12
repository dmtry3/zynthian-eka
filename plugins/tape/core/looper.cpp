#include "looper.h"

#include <chrono>
#include <cmath>
#include <new>

#include "pitch.h"

namespace alp {

namespace {
constexpr uint32_t kReserve = 4;     // кусков про запас, чтобы запись не упёрлась в ноль
constexpr uint64_t kNoSeam = ~0ull;
constexpr double kLatePressS = 0.2;
constexpr double kRecColS = 0.02;  // волна записи: секунд в колонке до первого слияния
constexpr double kMixSmoothS = 0.02;   // сглаживание уровня и панорамы
constexpr float kLimThr = 0.891f;      // ограничитель мастера: −1 дБ
constexpr float kLimRelS = 0.15f;      // отпускание ограничителя
constexpr float kPeakFallDbS = 20.0f;  // спад пиковых индикаторов, дБ/с

inline float db_gain(float db) { return db <= kMinDb ? 0.0f : std::pow(10.0f, db / 20.0f); }

// Мягкое ограничение у 0 дБ: подстраховка после ограничителя.
inline float soft_clip(float y) {
  const float k = 0.95f, a = std::fabs(y);
  if (a <= k) return y;
  const float v = k + (1.0f - k) * std::tanh((a - k) / (1.0f - k));
  return y < 0 ? -v : v;
}  // опоздание нажатия после черты, которое ещё считается «на черте»

inline uint64_t wrap_index(int64_t i, uint64_t L) {
  int64_t m = i % (int64_t)L;
  return (uint64_t)(m < 0 ? m + (int64_t)L : m);
}

inline double wrap_pos(double p, double L) {
  if (L <= 0.0) return 0.0;
  p = std::fmod(p, L);
  return p < 0.0 ? p + L : p;
}

constexpr uint32_t kSeekMax = 8192;     // кадров: история и прогон запуска Stretch (до 7200 и 4320)
constexpr double kPitchJumpS = 0.02;    // позиция для Stretch ушла дальше — перескок (seek)
constexpr float kPitchXfS = 0.02f;      // включение и выключение сдвига: кроссфейд 20 мс
constexpr uint32_t kOseekGap = 256;     // запуск Stretch (прогон вперёд) не чаще раза в столько кадров
}  // namespace

// Банк Stretch: память выделяется в init(), в аудиопотоке только берётся и отдаётся.
// Воспроизведение: до kPitchEng дорожек сразу (текущий парт и затихающий), овердаб: до kDubEng.
struct Looper::PitchBank {
  PitchShifter play[kPitchEng];
  PitchShifter dub[kDubEng];
  bool used[kPitchEng] = {};
  bool dused[kDubEng] = {};
  int lat = 0, slen = 0, wlen = 0;
  float wet[kPitchEng][2][kPitchBlock];
  float dout[kDubEng][2][kPitchBlock];
  float ain[2][kPitchBlock];
  float din[2][kPitchBlock];
  float sbuf[2][kSeekMax];  // история перед точкой запуска
  float pbuf[2][kSeekMax];  // прогон вперёд
  float tbuf[2][kSeekMax];  // его выход (выбрасывается)
};

Looper::~Looper() { delete pb_; }

bool Looper::init(double sample_rate, size_t pool_bytes) {
  sr_ = sample_rate > 0 ? sample_rate : 48000.0;
  xf_ = (uint32_t)std::lround(sr_ * 0.010);
  if (xf_ < 16) xf_ = 16;
  for (auto& t : tr_) t.seam_t = kNoSeam;
  default_locks();
  for (int k = 0; k < kTracks; ++k) mix_matrix(mix_[k], mcur_[k]);
  mix_matrix(mix_[0], dcur_);
  // Stretch для сдвига высоты: не вышло — дорожки просто звучат без сдвига
  if (!pb_) {
    pb_ = new (std::nothrow) PitchBank();
    bool ok = pb_ != nullptr;
    for (int e = 0; ok && e < kPitchEng; ++e) ok = pb_->play[e].init(sr_);
    for (int e = 0; ok && e < kDubEng; ++e) ok = pb_->dub[e].init(sr_);
    if (ok) {
      pb_->lat = pb_->play[0].latency();
      pb_->slen = pb_->play[0].seek_len();
      pb_->wlen = pb_->play[0].warm_len();
      ok = pb_->lat > 0 && pb_->slen > 0 && pb_->slen <= (int)kSeekMax && pb_->wlen > 0 &&
           pb_->wlen <= (int)kSeekMax && pb_->wlen <= pb_->lat && pb_->dub[0].latency() == pb_->lat;
    }
    if (!ok) {
      delete pb_;
      pb_ = nullptr;
    }
  }
  // Если столько памяти не дали, пробуем меньше: плагин должен загрузиться.
  for (size_t b = pool_bytes; b >= ((size_t)32 << 20); b /= 2)
    if (pool_.init(b, 4096)) {
      pool_chunks_.store(pool_.total_chunks(), std::memory_order_relaxed);
      return true;
    }
  return false;
}

// ---------------------------------------------------------------- команды

void Looper::command(Cmd c) {
  if (!pool_.ok()) return;
  if (pool_state_ != PS_IDLE && c == Cmd::Rec) return;  // меняется пул: запись подождёт
  // идёт сохранение сессии: всё, что меняет записанный звук, молча игнорируется
  if (frozen_.load(std::memory_order_relaxed) &&
      (c == Cmd::Rec || c == Cmd::Undo || c == Cmd::Redo || c == Cmd::ResetTrack || c == Cmd::ResetAll))
    return;
  const int a = idx(active_);
  Track& t = tr_[a];
  switch (c) {
    case Cmd::Rec:
      // переход ждёт границы: запись взводится в новом парте (повторное нажатие — отмена)
      if (pend_part_ >= 0) {
        if (rec_after_switch_ || !rec_blocked()) rec_after_switch_ = !rec_after_switch_;
      } else {
        rec_press(a);
      }
      break;
    case Cmd::Undo: undo(a); break;
    case Cmd::Redo: redo(a); break;
    case Cmd::ResetTrack: reset_track(a); break;
    case Cmd::ResetAll:
      if (job_pending_ || job_running_) reset_all_pending_ = true;
      else reset_all_now();
      break;
    case Cmd::Mute: mute_slot(active_); break;
    case Cmd::Reverse: {
      if ((t.st != TState::Play && t.st != TState::Dub) || t.len == 0) break;
      if (t.rev_wait >= 0) {  // повторное нажатие до такта отменяет разворот
        t.rev_wait = -1;
        break;
      }
      double n = set_.reverse_ms * sr_ / 1000.0;
      if (n < sr_ * 0.005) n = sr_ * 0.005;  // не короче 5 мс
      if (!set_.quantize) {
        start_reverse(t, n);
        break;
      }
      // Quantize: такт доигрывается. Лента останавливается ровно на тактовой черте:
      // разворот начинается за n/2 до неё, и после него позиция та же, что при мгновенном
      // развороте на черте (вперёд уходим на n/4 и столько же возвращаемся).
      const double bf = bpb_ / (bpf_ > 1e-12 ? bpf_ : 2.0 / sr_);
      const double next_bar = (std::floor(beats_ / bpb_) + 1.0) * bpb_;
      double start = (next_bar - beats_) / (bpf_ > 1e-12 ? bpf_ : 2.0 / sr_) - n / 2.0;
      if (start < 0.0) start += bf;  // до черты меньше половины разворота: следующий такт
      t.rev_wait = (int64_t)std::llround(start);
      t.rev_n = n;
      break;
    }
    case Cmd::PlayStop: {
      if (ps_pending_) {  // повторное нажатие до границы отменяет
        ps_pending_ = 0;
        break;
      }
      const bool stop = !user_stop_;
      if (!set_.quantize || set_.mute_q == 0 || hold_) apply_playstop(stop);
      else ps_pending_ = stop ? 1 : 2;
      break;
    }
    case Cmd::TrackNext:
    case Cmd::TrackPrev:
      leave_track(a);
      active_ = (active_ + (c == Cmd::TrackNext ? 1 : ns_ - 1)) % ns_;
      break;
    case Cmd::PartNext:
    case Cmd::PartPrev: {
      const int d = c == Cmd::PartNext ? 1 : ns_ - 1;
      if (pend_part_ >= 0) {  // переход ждёт границы: цель дальше или назад
        const int np = (pend_part_ + d) % ns_;
        pend_part_ = np == cur_part_ ? -1 : np;
        if (pend_part_ < 0) rec_after_switch_ = false;
        break;
      }
      request_part((cur_part_ + d) % ns_);
      break;
    }
    case Cmd::Lock: t.locked = !t.locked; break;
  }
}

void Looper::request_part(int target) {
  // сразу: текущий парт пустой, PART AT = сразу, или Free без «конца цикла» (тактов нет);
  // транспорт стоит и лупы затихли: ждать черты нечего
  if (!part_has_content(cur_part_) || set_.part_q == 0 || (!set_.quantize && set_.part_q != 3) || hold_)
    switch_part(target);
  else
    pend_part_ = target;
}

void Looper::select_slot(int slot) {
  if (!pool_.ok() || slot < 0 || slot >= ns_ || slot == active_) return;
  leave_track(idx(active_));
  active_ = slot;
}

void Looper::select_part(int p) {
  if (!pool_.ok() || p < 0 || p >= ns_) return;
  if (pend_part_ >= 0) {  // переход ждёт границы
    if (p == pend_part_ || p == cur_part_) {
      pend_part_ = -1;
      rec_after_switch_ = false;
    } else {
      pend_part_ = p;
    }
    return;
  }
  if (p != cur_part_) request_part(p);
}

void Looper::rec_press(int ti) {
  Track& t = tr_[ti];
  if (t.busy) return;
  switch (t.st) {
    case TState::Empty:
      // запись только при запущенном транспорте Zynthian (решение пользователя 2026-11-08)
      // и не во время сохранения сессии (Save ждёт, пока закроется начатое)
      if (!rolling_ || rec_blocked()) break;
      // счёт перед записью (этап 8): в Quantize — с ближайшей черты, в Free — сразу от нажатия
      t.count_n = (uint8_t)(set_.count_in > 0 ? set_.count_in : 0);
      t.count_end = -1.0;
      if (!set_.quantize) {
        if (t.count_n == 0 && set_.rec_thresh_db < 0) {  // порог: ждём сигнала
          t.st = TState::Armed;
          t.rec_wait = 0;
        } else if (t.count_n == 0) {
          start_rec(ti);
        } else {
          t.st = TState::Armed;
          t.rec_wait = 0;
          t.count_end = beats_ + (double)t.count_n * bpb_;
        }
      } else {
        t.st = TState::Armed;
        t.rec_wait = 0;
      }
      break;
    case TState::Armed: t.st = TState::Empty; break;
    case TState::Rec: request_close(ti); break;
    case TState::Closing: break;
    case TState::Play:
      if (!t.stopped && !t.ustop && !rec_blocked()) start_dub(ti);  // остановлена: молча игнорируем
      break;
    case TState::Dub:
      if (t.dub_end < 0) end_dub(ti);
      break;
  }
}

void Looper::start_rec(int ti) {
  Track& t = tr_[ti];
  if (!reserve(2, ti)) {
    t.st = TState::Empty;
    return;
  }
  int32_t b = pool_.alloc_buf();
  if (b < 0) {
    t.st = TState::Empty;
    return;
  }
  t.rec = b;
  t.rec_frames = 0;
  t.rec_target = 0;
  t.rec_no_dub = false;
  t.rec_lat = 0;
  const int len_bars = set_.length[ti % ns_];  // длина задаётся по месту дорожки в парте
  if (set_.quantize && len_bars > 0) t.rec_target = (uint64_t)std::llround(len_bars * (double)bar_frames());
  // Quantize: запись начинается на черте такта, запоминаем её долю для подстройки фазы
  t.rec_q = set_.quantize;
  t.b0 = std::round(beats_ / bpb_) * bpb_;
  t.rec_bpf = bpf_;
  t.tspd = 1.0;
  t.pit = 0.0f;  // новая основа — без сдвига высоты (обёртка сообщит хосту)
  t.pcur = 0.0;
  t.st = TState::Rec;
  rec_peaks_start(ti);
}

// Старт по порогу: запись начинается с кадра, где вход перешёл порог; чтобы атака не обрезалась,
// в начало кладём вход за 10 мс до него (с fade in начала записи).
void Looper::prefill_rec(int ti) {
  Track& t = tr_[ti];
  if (t.st != TState::Rec || t.rec < 0) return;
  uint32_t p = (uint32_t)std::lround(sr_ * 0.010);
  if (p > kPreMax - 1) p = kPreMax - 1;
  if (p > pre_n_) p = pre_n_;
  if (p == 0) return;
  Chunk* c = pool_.ensure(t.rec, 0, false);
  if (!c) return;
  for (uint32_t j = 0; j < p; ++j) {
    const uint32_t q = (pre_w_ + kPreMax - p + j) % kPreMax;
    const float g = j < xf_ ? (float)j / (float)xf_ : 1.0f;
    c->l[j] = pre_l_[q] * g;
    c->r[j] = pre_r_[q] * g;
    rec_peaks(ti, c->l[j], c->r[j]);
  }
  t.rec_frames = p;
}

// Волна записи основы. При фиксированной длине вся полоса — эта длина;
// при свободной колонка сначала 20 мс, а когда колонки кончаются, они сливаются попарно.
void Looper::rec_peaks_start(int ti) {
  Track& t = tr_[ti];
  t.rpk_step = t.rec_target ? (t.rec_target + kPeakCols - 1) / kPeakCols
                            : (uint64_t)std::llround(sr_ * kRecColS);
  if (t.rpk_step < 1) t.rpk_step = 1;
  t.rpk_n = 0;
  t.rpk_mn = t.rpk_mx = 0.0f;
  if (!peaks_ || !in_cur(ti)) return;
  PeakTrack& pk = peaks_->t[ti % ns_];
  clear_wave(pk.rec);
  pk.rec_col_s = (float)((double)t.rpk_step / sr_);
  __atomic_store_n(&pk.rec_cols, 0u, __ATOMIC_RELEASE);
  t.plive = true;
}

void Looper::rec_peaks(int ti, float l, float r) {
  Track& t = tr_[ti];
  if (l < t.rpk_mn) t.rpk_mn = l;
  if (r < t.rpk_mn) t.rpk_mn = r;
  if (l > t.rpk_mx) t.rpk_mx = l;
  if (r > t.rpk_mx) t.rpk_mx = r;
  if (++t.rpk_n < t.rpk_step) return;
  const float mn = t.rpk_mn, mx = t.rpk_mx;
  t.rpk_n = 0;
  t.rpk_mn = t.rpk_mx = 0.0f;
  if (!peaks_ || !in_cur(ti)) return;
  PeakTrack& pk = peaks_->t[ti % ns_];
  uint32_t n = pk.rec_cols;
  if (n >= kPeakCols) {  // колонки кончились: вдвое крупнее (512 сравнений раз в несколько секунд)
    for (uint32_t c = 0; c < kPeakCols / 2; ++c) {
      const int8_t a = pk.rec.mn[2 * c], b = pk.rec.mn[2 * c + 1];
      const int8_t x = pk.rec.mx[2 * c], y = pk.rec.mx[2 * c + 1];
      pk.rec.mn[c] = a < b ? a : b;
      pk.rec.mx[c] = x > y ? x : y;
    }
    for (uint32_t c = kPeakCols / 2; c < kPeakCols; ++c) pk.rec.mn[c] = pk.rec.mx[c] = 0;
    n = kPeakCols / 2;
    t.rpk_step *= 2;
    pk.rec_col_s = (float)((double)t.rpk_step / sr_);
  }
  pk.rec.mn[n] = peak_q(mn);
  pk.rec.mx[n] = peak_q(mx);
  __atomic_store_n(&pk.rec_cols, n + 1, __ATOMIC_RELEASE);
  t.plive = true;
}

// Запись с черты (Quantize). Вход приходит в плагин позже, чем музыкант его сыграл, на задержку
// аудиоинтерфейса: запись начинается на столько же позже — и сыгранное «на раз» ляжет на начало цикла.
void Looper::rec_on_boundary(int ti) {
  Track& t = tr_[ti];
  t.count_n = 0;  // счёт кончился (или его не было)
  t.count_end = -1.0;
  if (set_.quantize && lat_ > 0) {
    t.st = TState::Armed;
    t.rec_wait = lat_;
    rec_waiting_ = true;
  } else {
    start_rec(ti);
  }
}

void Looper::cancel_rec(int ti) {
  Track& t = tr_[ti];
  if (t.rec >= 0) pool_.free_buf(t.rec);
  t.rec = -1;
  t.rec_frames = t.rec_target = 0;
  t.st = TState::Empty;
  if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].rec_cols, 0u, __ATOMIC_RELEASE);
}

void Looper::request_close(int ti) {
  Track& t = tr_[ti];
  if (!set_.quantize) {
    close_loop(ti, t.rec_frames);
    return;
  }
  if (t.rec_target) return;  // фиксированная длина: закроется сама
  // Free-длина в Quantize: такт доигрывается до конца. Нажатие чуть позже черты
  // (нога опоздала, не больше kLatePressS) закрывает цикл на этой черте.
  double bf = (double)bar_frames();
  int64_t bars = (int64_t)std::floor((double)t.rec_frames / bf);
  const double late = (double)t.rec_frames - (double)bars * bf;
  if (bars < 1 || late > kLatePressS * sr_) ++bars;
  uint64_t L = (uint64_t)std::llround(bars * bf);
  if (L <= t.rec_frames) {
    close_loop(ti, L);
  } else {
    t.rec_target = L;
    t.st = TState::Closing;
  }
}

void Looper::close_loop(int ti, uint64_t L) {
  Track& t = tr_[ti];
  if (L < 2ull * xf_ + 2) return;  // слишком коротко: пишем дальше
  uint64_t d = t.rec_frames - L;   // уже записано после конца цикла (граница такта прошла)
  if (d >= L) d = L - 1;
  int32_t base = t.rec;
  ++edit_epoch_;
  t.rec = -1;
  t.rec_frames = t.rec_target = 0;
  if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].rec_cols, 0u, __ATOMIC_RELEASE);
  clear_redo(t);
  t.sum = base;
  t.len = L;
  push_undo(t, Entry{U_SNAP, -1});
  t.pos = 0.0;
  t.rate = 1.0;
  t.dir = 1;
  t.rate_step = 0.0;
  t.muted = t.mute_pending = false;
  t.gain = 1.0f;
  t.seam_t = 0;
  t.synced = t.rec_q;
  t.loop_beats = std::fmax(1.0, std::round((double)L * t.rec_bpf));
  t.st = TState::Play;
  if (tstop_) t.stopped = true;  // транспорт остановили во время записи: луп затихает и ждёт пуска
  else if (set_.rop && !t.rec_no_dub) start_dub(ti);
  t.rec_no_dub = false;
  // Хвост, записанный после конца цикла, ложится на начало: стык и (в ROP) овердаб.
  for (uint64_t k = 0; k < d; ++k) {
    float l, r;
    pool_.read(pool_.buf(base), L + k, l, r);
    if (t.seam_t < xf_) seam_write(t, l, r);
    if (t.st == TState::Dub && t.layer >= 0) dub_write(ti, k, l, r);
  }
  pool_.set_len(base, L);
  // запись начата позже черты на компенсацию задержки: позиция на столько же дальше (в фазе с сеткой)
  t.pos = (double)((d + t.rec_lat) % L);
  t.rec_lat = 0;
  // Дорожка начинает звучать: короткий fade in, иначе вход хвоста на полной громкости щёлкнет.
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = -1;
  t.xf_owned = false;
  t.xf_pos = 0;
  t.xpos = t.pos;
  t.xf_len = xf_;
  mark_peaks(t);
}

void Looper::start_dub(int ti) {
  Track& t = tr_[ti];
  if (t.busy || t.sum < 0) return;
  if (!reserve(2, ti)) return;
  int32_t b = pool_.alloc_buf();
  if (b < 0) return;
  pool_.set_len(b, t.len);
  clear_redo(t);
  t.layer = b;
  t.dub_t = 0;
  t.dub_end = -1;
  t.wfirst = true;
  t.undo_after_dub = t.reset_after_dub = false;
  t.st = TState::Dub;
  // сдвинутая дорожка: вход в Stretch с обратным сдвигом, чтобы овердаб звучал как сыгран
  dub_release(t);
  t.ddrain = false;
  t.dgate = 1.0f;
  if (pb_ && t.pit != 0.0f && !pitch_over_) {
    for (int e = 0; e < kDubEng; ++e)
      if (!pb_->dused[e]) {
        pb_->dused[e] = true;
        t.deng = (int8_t)e;
        t.dlat = (uint32_t)pb_->lat;
        pb_->dub[e].reset();
        break;
      }
  }
  if (peaks_ && in_cur(ti)) {  // волна нового слоя растёт вживую
    PeakTrack& pk = peaks_->t[ti % ns_];
    clear_wave(pk.dub);
    __atomic_store_n(&pk.dub_active, 1u, __ATOMIC_RELEASE);
    t.plive = true;
  }
}

void Looper::end_dub(int ti) {
  Track& t = tr_[ti];
  if (t.st != TState::Dub || t.dub_end >= 0) return;
  if (t.deng >= 0) {  // вход идёт через Stretch: сначала дописать то, что в нём (его задержка и блок)
    if (!t.ddrain) {
      t.ddrain = true;
      t.ddrain_n = t.dlat + xf_ + last_n_;  // вход до конца этого блока ещё попал в Stretch
    }
    return;
  }
  t.dub_end = 0;
}

void Looper::end_dub_now(int ti) {
  Track& t = tr_[ti];
  if (t.st == TState::Dub && t.dub_end < 0) t.dub_end = 0;
}

void Looper::finish_dub(int ti) {
  Track& t = tr_[ti];
  ++edit_epoch_;
  if (t.layer >= 0) push_undo(t, Entry{U_DELTA, t.layer});
  t.layer = -1;
  t.dub_end = -1;
  t.st = TState::Play;
  dub_release(t);
  t.ddrain = false;
  mark_peaks(t);
  if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].dub_active, 0u, __ATOMIC_RELEASE);
  if (t.lock_to >= 0) {  // Lock-копия ждала конца овердаба, чтобы затухание попало и в неё;
    t.lock_ready = true;  // делаем её в начале следующего кадра (этот кадр уже прозвучал)
    clones_pending_ = true;
  }
  if (t.undo_after_dub) {
    t.undo_after_dub = false;
    undo(ti);
  } else if (t.reset_after_dub) {
    t.reset_after_dub = false;
    reset_track(ti);
  }
}

void Looper::undo(int ti) {
  Track& t = tr_[ti];
  if (t.busy) return;
  switch (t.st) {
    case TState::Armed: t.st = TState::Empty; return;
    case TState::Rec:
    case TState::Closing: cancel_rec(ti); return;
    case TState::Dub:
      end_dub(ti);
      t.undo_after_dub = true;
      return;
    default: break;
  }
  if (t.undo_n == 0) return;
  uint32_t top = (t.undo_lo + t.undo_n - 1) % kMaxUndo;
  Entry e = t.undo[top];
  if (e.type == U_SNAP) {
    --t.undo_n;
    int32_t old = t.sum;
    if (t.redo_n < kMaxUndo) t.redo[t.redo_n++] = Entry{U_SNAP, old};
    swap_to(ti, e.buf, false);
  } else {
    post_job(ti, 1, e.buf);
  }
}

void Looper::redo(int ti) {
  Track& t = tr_[ti];
  if (t.busy || (t.st != TState::Play && t.st != TState::Empty) || t.redo_n == 0) return;
  Entry e = t.redo[t.redo_n - 1];
  if (e.type == U_SNAP) {
    --t.redo_n;
    push_undo(t, Entry{U_SNAP, t.sum});
    swap_to(ti, e.buf, false);
  } else {
    post_job(ti, 2, e.buf);
  }
}

void Looper::reset_track(int ti) {
  Track& t = tr_[ti];
  if (t.busy) return;
  if (t.st == TState::Armed) {
    t.st = TState::Empty;
    return;
  }
  if (t.st == TState::Rec || t.st == TState::Closing) {
    cancel_rec(ti);
    return;
  }
  if (t.st == TState::Dub) {  // сначала плавно закрыть овердаб (слой сохранится для undo), потом reset
    end_dub(ti);
    t.reset_after_dub = true;
    return;
  }
  if (t.sum < 0) return;
  clear_redo(t);
  push_undo(t, Entry{U_SNAP, t.sum});
  swap_to(ti, -1, false);
}

void Looper::set_pool_size(size_t bytes) {
  if (bytes < ((size_t)32 << 20)) return;
  if (pool_state_ == PS_IDLE && bytes == pool_.bytes()) return;
  if (pool_state_ >= PS_DRAIN && bytes == pool_req_) return;
  pool_req_ = bytes;
  if (pool_state_ == PS_IDLE) pool_state_ = PS_REQ;
}

// Смена пула по шагам, в начале каждого блока (RT).
void Looper::pool_step() {
  if (pool_state_ == PS_REQ) {
    if (job_pending_ || job_running_) return;  // undo/redo дочитает старый пул
    // сессия сохраняется или грузится: пул сменится после
    if (frozen_.load(std::memory_order_relaxed) || freeze_req_.load(std::memory_order_relaxed) ||
        load_state_ != LS_IDLE)
      return;
    reset_all_now(false);                      // лупы затихают по кроссфейду, микшер остаётся
    pool_state_ = PS_DRAIN;
  }
  if (pool_state_ == PS_DRAIN) {
    for (const auto& t : tr_)
      if (t.st != TState::Empty || t.xf_len > 0 || t.pinflight) return;
    if (job_pending_ || job_running_) return;
    pool_.swap(spare_);  // пул без памяти: лупер молчит, сухой сигнал идёт
    pool_state_ = PS_ALLOC;
    pool_msg_ = true;
  }
}

// Смена сетки: если рабочий поток считает undo/redo, ждём его ответа (как удержание Reset).
void Looper::set_grid(int n) {
  if (!valid_grid(n) || (n == ns_ && grid_pending_ < 0)) return;
  grid_pending_ = n;
  // сохранение читает звук: сетка сменится после него (session_step)
  if (!job_pending_ && !job_running_ && !frozen_.load(std::memory_order_relaxed)) apply_grid();
}

void Looper::apply_grid() {
  const int n = grid_pending_;
  grid_pending_ = -1;
  if (!valid_grid(n) || n == ns_) return;
  ns_ = n;
  reset_all_now(false);  // звучащие лупы затухают; дорожки за краем новой сетки тоже доигрывают затухание
}

void Looper::reset_all_now(bool full) {
  ++edit_epoch_;
  entry_pending_ = rebase_pending_ = false;
  user_stop_ = false;
  ps_pending_ = 0;
  pend_part_ = -1;
  rec_after_switch_ = false;
  for (int ti = 0; ti < kTracks; ++ti) {
    Track& t = tr_[ti];
    mark_peaks(t);
    if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].dub_active, 0u, __ATOMIC_RELEASE);
    if (t.rec >= 0) pool_.free_buf(t.rec);
    if (t.layer >= 0) pool_.free_buf(t.layer);
    t.rec = t.layer = -1;
    clear_redo(t);
    while (t.undo_n) {
      drop_entry_bufs(t, t.undo[t.undo_lo]);
      t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
      --t.undo_n;
    }
    t.undo_lo = 0;
    // звучащее затухает, потом уходит в пул
    if (t.sum >= 0) {
      if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
      t.xf_from = t.sum;
      t.xf_owned = true;
      t.xf_pos = 0;
      t.xpos = t.pos;
      t.xf_len = xf_;
    }
    t.sum = -1;
    t.len = 0;
    t.st = TState::Empty;
    t.muted = t.mute_pending = false;
    t.gain = 1.0f;
    t.dir = 1;
    t.rate = 1.0;
    t.rate_step = 0.0;
    t.rev_wait = -1;
    t.synced = false;
    t.stopped = t.ustop = false;
    t.sgain = 1.0f;
    t.locked = false;
    t.lock_to = -1;
    t.lock_ready = false;
    if (full) t.mspd = 1.0f;
    t.mcur = t.spd = t.tspd = 1.0;
    pitch_release(t);
    dub_release(t);
    t.ddrain = false;
    t.pw = 0.0f;
    if (full) t.pit = 0.0f;
    t.pcur = t.pit;
    t.phase_reset = t.phase_wait = false;
    t.dub_end = -1;
    t.undo_after_dub = t.reset_after_dub = false;
    t.seam_t = kNoSeam;
  }
  default_locks();
  if (full) reset_mixer();
  // курсор — на первую дорожку первого парта
  cur_part_ = 0;
  active_ = 0;
}

void Looper::reset_mixer() {
  for (auto& m : mix_) m = Mix();
  master_db_ = 0.0f;
  lim_on_ = true;
  ++mix_epoch_;
}

// Матрица дорожки: выход L = in L * LL + in R * RL, выход R = in L * LR + in R * RR.
void Looper::mix_matrix(const Mix& m, float out[4]) {
  const float g = db_gain(m.level_db);
  const float p = m.pan < -1.0f ? -1.0f : (m.pan > 1.0f ? 1.0f : m.pan);
  if (m.mode == 0) {  // стерео: баланс с постоянной мощностью, в центре как записано
    const float th = (p + 1.0f) * 0.7853982f;
    out[0] = g * std::cos(th) * 1.4142136f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = g * std::sin(th) * 1.4142136f;
  } else {  // моно L или моно R: играет один вход, панорама ставит его с постоянной мощностью
    const float th = (p + 1.0f) * 0.7853982f;
    const float a = g * std::cos(th), b = g * std::sin(th);
    const bool left = m.mode == 1;
    out[0] = left ? a : 0.0f;  // L -> левый выход
    out[1] = left ? 0.0f : a;  // R -> левый
    out[2] = left ? b : 0.0f;  // L -> правый
    out[3] = left ? 0.0f : b;  // R -> правый
  }
}

void Looper::start_reverse(Track& t, double n) {
  t.dir = -t.dir;
  t.rate_step = 2.0 / n * t.dir;
}

void Looper::mute_slot(int slot) {
  if (slot < 0 || slot >= ns_) return;
  const int k = idx(slot);
  Track& t = tr_[k];
  if (t.st != TState::Play && t.st != TState::Dub) return;
  if (!set_.quantize || set_.mute_q == 0) apply_mute(k);
  else t.mute_pending = !t.mute_pending;
}

void Looper::apply_mute(int ti) {
  Track& t = tr_[ti];
  t.muted = !t.muted;
  t.mute_pending = false;
}

void Looper::leave_track(int ti) {
  Track& t = tr_[ti];
  if (t.st == TState::Armed) t.st = TState::Empty;
  else if (t.st == TState::Rec || t.st == TState::Closing) {
    t.rec_no_dub = true;  // ROP: дорожку оставили — цикл закрывается и просто играет
    if (t.st == TState::Rec) request_close(ti);
  } else if (t.st == TState::Dub) end_dub(ti);
}

// ---------------------------------------------------------------- undo, redo, память

void Looper::push_undo(Track& t, Entry e) {
  if (t.undo_n == kMaxUndo) {  // самый старый слой вливается в основу
    drop_entry_bufs(t, t.undo[t.undo_lo]);
    t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
    --t.undo_n;
  }
  t.undo[(t.undo_lo + t.undo_n) % kMaxUndo] = e;
  ++t.undo_n;
}

void Looper::clear_redo(Track& t) {
  for (uint32_t i = 0; i < t.redo_n; ++i) drop_entry_bufs(t, t.redo[i]);
  t.redo_n = 0;
}

void Looper::free_buf_safe(Track& t, int32_t b) {
  if (b < 0) return;
  if (b == t.xf_from) t.xf_owned = true;  // ещё звучит в кроссфейде: освободится после него
  else pool_.free_buf(b);
}

void Looper::drop_entry_bufs(Track& t, const Entry& e) { free_buf_safe(t, e.buf); }

bool Looper::reserve(uint32_t need, int prefer) {
  while (pool_.free_chunks() < need + kReserve || pool_.free_bufs() < 4) {
    bool freed = false;
    for (int i = 0; i < kTracks && !freed; ++i) {  // сначала то, что можно только вернуть (redo)
      Track& t = tr_[i];
      if (!t.busy && t.redo_n) {
        clear_redo(t);
        freed = true;
      }
    }
    for (int k = 0; k < kTracks && !freed; ++k) {  // потом самые старые слои, своя дорожка первой
      int i = (prefer + k) % kTracks;
      Track& t = tr_[i];
      if (t.busy || t.undo_n == 0) continue;
      drop_entry_bufs(t, t.undo[t.undo_lo]);
      t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
      --t.undo_n;
      freed = true;
    }
    if (!freed) return false;
  }
  return true;
}

void Looper::swap_to(int ti, int32_t new_sum, bool free_old) {
  Track& t = tr_[ti];
  ++edit_epoch_;
  mark_peaks(t);
  int32_t old = t.sum;
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = old;
  t.xf_owned = free_old;
  t.xf_pos = 0;
  t.xpos = t.pos;
  t.xf_len = xf_;
  t.sum = new_sum;
  if (new_sum < 0) {
    t.rev_wait = -1;
    t.synced = false;
    t.stopped = t.ustop = false;
    t.sgain = 1.0f;
    t.st = TState::Empty;
    t.len = 0;
    t.muted = t.mute_pending = false;
    t.gain = 1.0f;
    t.seam_t = kNoSeam;
    return;
  }
  t.len = pool_.buf(new_sum).len;
  if (t.synced && bpf_ > 0) t.loop_beats = std::fmax(1.0, std::round((double)t.len * t.rec_bpf));
  if (old < 0) {
    t.pos = 0.0;
    t.dir = 1;
    t.rate = 1.0;
    t.rate_step = 0.0;
  } else {
    t.pos = std::fmod(t.pos, (double)t.len);
  }
  t.st = TState::Play;
}

bool Looper::post_job(int ti, uint8_t op, int32_t layer) {
  Track& t = tr_[ti];
  if (job_pending_ || job_running_ || t.sum < 0) return false;
  // запись снимаем со стека заранее, чтобы reserve() её не освободил
  if (op == 1) --t.undo_n;
  else --t.redo_n;
  uint64_t L = t.len;
  uint32_t need = chunks_for(L) + 1;
  int32_t dst = -1;
  bool ok = reserve(need, ti) && (dst = pool_.alloc_buf()) >= 0 && pool_.set_len(dst, L);
  for (uint32_t ci = 0; ok && ci < chunks_for(L); ++ci) ok = pool_.ensure(dst, ci, false) != nullptr;
  if (!ok) {
    if (dst >= 0) pool_.free_buf(dst);
    if (op == 1) push_undo(t, Entry{U_DELTA, layer});
    else t.redo[t.redo_n++] = Entry{U_DELTA, layer};
    return false;
  }
  job_ = WorkMsg{};
  job_.op = op;
  job_.track = (uint8_t)ti;
  job_.src = t.sum;
  job_.layer = layer;
  job_.dst = dst;
  job_.len = L;
  job_pending_ = true;
  t.busy = true;
  return true;
}

bool Looper::take_work(WorkMsg& m) {
  if (pool_msg_) {
    pool_msg_ = false;
    m = WorkMsg{};
    m.op = 4;
    m.len = pool_req_;
    return true;
  }
  if (load_msg_) {
    load_msg_ = false;
    m = WorkMsg{};
    m.op = 5;
    m.gen = load_seen_;
    return true;
  }
  // пул вот-вот уйдёт (смена размера или загрузка сессии): заданий, читающих его, не даём
  if (pool_state_ >= PS_DRAIN || load_state_ >= LS_DRAIN) return false;
  if (job_pending_) {
    m = job_;
    job_pending_ = false;
    job_running_ = true;
    return true;
  }
  if (!peaks_) return false;
  // пики: сумма и последние слои овердабов (их номера с начала дорожки — для цвета)
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (!t.pdirty || t.pinflight || t.busy) continue;
    m = WorkMsg{};
    m.op = 3;
    m.track = (uint8_t)k;
    m.slot = (uint8_t)slot_of(k);
    m.src = t.sum;
    m.layer = m.dst = -1;
    m.len = t.sum >= 0 ? t.len : 0;
    m.gen = t.pgen;
    for (uint32_t j = t.undo_n; j > 0 && m.nl < kPeakLayers; --j) {
      const Entry& e = t.undo[(t.undo_lo + j - 1) % kMaxUndo];
      if (e.type != U_DELTA) break;  // ниже — основа или предыдущая запись
      m.lay[m.nl] = e.buf;
      m.lno[m.nl] = j - 1;
      ++m.nl;
    }
    t.pdirty = false;
    t.pinflight = true;
    return true;
  }
  return false;
}

void Looper::cancel_work(const WorkMsg& m) {
  if (m.op == 4) {  // хост не принял: отдадим ещё раз
    pool_msg_ = true;
    return;
  }
  if (m.op == 5) {
    load_msg_ = true;
    return;
  }
  if (m.op == 3) {
    Track& t = tr_[m.track];
    t.pinflight = false;
    t.pdirty = true;
    return;
  }
  if (!job_running_) return;
  job_running_ = false;
  Track& t = tr_[job_.track];
  t.busy = false;
  pool_.free_buf(job_.dst);
  if (job_.op == 1) push_undo(t, Entry{U_DELTA, job_.layer});
  else if (t.redo_n < kMaxUndo) t.redo[t.redo_n++] = Entry{U_DELTA, job_.layer};
  if (reset_all_pending_) {
    reset_all_pending_ = false;
    reset_all_now();
  }
  if (grid_pending_ > 0) apply_grid();
}

void Looper::do_work(const WorkMsg& m, WorkResp& r) {
  r.op = m.op;
  r.track = m.track;
  r.gen = m.gen;
  if (m.op == 4) {  // рабочий поток: старый пул освободить, новый выделить (не вышло — вдвое меньше)
    spare_.release();
    for (size_t b = (size_t)m.len; b >= ((size_t)32 << 20); b /= 2)
      if (spare_.init(b, 4096)) break;
    return;
  }
  if (m.op == 5) {  // рабочий поток: пул целиком здесь (аудиопоток молчит) — заполнить звуком сессии
    SessionDesc d;
    std::string dir;
    uint32_t gen;
    {
      std::lock_guard<std::mutex> lk(load_mx_);
      d = load_desc_;
      dir = load_dir_;
      gen = load_slot_gen_;  // самый свежий запрос
    }
    spare_.reset();
    LoadResult& res = load_res_;
    res.gen = gen;
    res.status = SS_IDLE;
    uint64_t total = 0;
    for (const auto& s : d.t) total += s.used ? s.len : 0;
    sess_total_.store(total, std::memory_order_relaxed);
    sess_done_.store(0, std::memory_order_relaxed);
    auto cancelled = [this, gen] { return load_gen_.load(std::memory_order_acquire) != gen; };
    for (int k = 0; k < kTracks && res.status == SS_IDLE && !cancelled(); ++k) {
      SessionTrack& s = d.t[k];
      s.buf = -1;
      if (!s.used || k >= d.grid * d.grid) continue;
      const int32_t b = spare_.alloc_buf();
      if (b < 0) {
        res.status = SS_E_MEMORY;
        break;
      }
      std::string path = dir;
      if (!path.empty() && path.back() != '/') path += '/';
      path += s.file;
      res.status = wav24_read(path.c_str(), spare_, b, s.len, s.gain, cancelled, &sess_done_);
      s.buf = b;
    }
    if (res.status != SS_IDLE || cancelled()) spare_.reset();  // не вышло или отменили: пул пустой
    res.desc = d;
    r.gen = gen;
    return;
  }
  if (m.op == 3) {
    if (!peaks_) return;
    PeakTrack& pk = peaks_->t[m.slot];
    seq_begin(pk);
    compute_wave(pool_, m.src, m.len, pk.sum);
    for (uint32_t i = 0; i < m.nl; ++i) {
      compute_wave(pool_, m.lay[i], m.len, pk.layer[i]);
      pk.layer_no[i] = m.lno[i];
    }
    pk.nlayers = m.nl;
    seq_end(pk);
    return;
  }
  const Buf& src = pool_.buf(m.src);
  const Buf& lay = pool_.buf(m.layer);
  const Buf& dst = pool_.buf(m.dst);
  const float sign = m.op == 1 ? -1.0f : 1.0f;
  const uint32_t nc = chunks_for(m.len);
  for (uint32_t ci = 0; ci < nc; ++ci) {
    Chunk* d = pool_.chunk(pool_.entry(dst, ci));
    const Chunk* s = pool_.chunk(pool_.entry(src, ci));
    uint32_t lid = pool_.entry(lay, ci);
    if (lid == kNone) {
      for (uint32_t i = 0; i < kChunkFrames; ++i) {
        d->l[i] = s->l[i];
        d->r[i] = s->r[i];
      }
    } else {
      const Chunk* x = pool_.chunk(lid);
      for (uint32_t i = 0; i < kChunkFrames; ++i) {
        d->l[i] = s->l[i] + sign * x->l[i];
        d->r[i] = s->r[i] + sign * x->r[i];
      }
    }
  }
}

void Looper::work_done(const WorkResp& r) {
  if (r.op == 5) {  // звук сессии прочитан (или нет): пул обратно в аудиопоток
    pool_.swap(spare_);
    if (load_res_.gen != load_gen_.load(std::memory_order_acquire)) {
      // пока читали, пришёл новый запрос: читать заново (пул пустой, лупы молчат)
      load_seen_ = load_gen_.load(std::memory_order_acquire);
      load_state_ = LS_DRAIN;
      return;
    }
    load_state_ = LS_IDLE;
    if (load_res_.status != SS_IDLE) {
      sess_status_.store(load_res_.status, std::memory_order_relaxed);
      cur_id_.store(0, std::memory_order_relaxed);
      for (auto& t : tr_) mark_peaks(t);
      return;
    }
    install_session(load_res_.desc);
    sess_status_.store(SS_LOADED, std::memory_order_relaxed);
    return;
  }
  if (r.op == 4) {  // новый пул готов
    if (spare_.ok()) pool_.swap(spare_);
    pool_state_ = PS_IDLE;
    for (auto& t : tr_) mark_peaks(t);
    return;
  }
  if (r.op == 3) {  // пики готовы; если дорожка успела измениться — посчитать ещё раз
    Track& t = tr_[r.track];
    t.pinflight = false;
    if (r.gen != t.pgen) t.pdirty = true;
    return;
  }
  job_running_ = false;
  ++edit_epoch_;
  Track& t = tr_[r.track];
  t.busy = false;
  mark_peaks(t);
  // новая сумма готова: кроссфейд со старой, старая уходит в пул
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = t.sum;
  t.xf_owned = true;
  t.xf_pos = 0;
  t.xpos = t.pos;
  t.xf_len = xf_;
  t.sum = job_.dst;
  if (r.op == 1) {
    if (t.redo_n < kMaxUndo) t.redo[t.redo_n++] = Entry{U_DELTA, job_.layer};
    else pool_.free_buf(job_.layer);
  } else {
    push_undo(t, Entry{U_DELTA, job_.layer});
  }
  if (reset_all_pending_) {
    reset_all_pending_ = false;
    reset_all_now();
  }
  if (grid_pending_ > 0) apply_grid();
}

// ---------------------------------------------------------------- звук

bool Looper::has_content() const {
  for (const auto& t : tr_)
    if (t.sum >= 0 || t.st == TState::Rec || t.st == TState::Closing) return true;
  return false;
}

uint64_t Looper::bar_frames() const {
  double bpf = bpf_ > 1e-9 ? bpf_ : 2.0 / sr_;
  return (uint64_t)std::llround(bpb_ / bpf);
}

void Looper::read_sum(const Track& t, int32_t b, double pos, float& l, float& r) const {
  (void)t;
  if (b < 0) {
    l = r = 0.0f;
    return;
  }
  const Buf& x = pool_.buf(b);
  const uint64_t L = x.len;
  if (L == 0) {
    l = r = 0.0f;
    return;
  }
  const double fl = std::floor(pos);
  if (fl == pos) {  // ровно 1x или -1x: прямое чтение
    pool_.read(x, wrap_index((int64_t)fl, L), l, r);
    return;
  }
  // кубическая интерполяция Эрмита (Catmull-Rom)
  const int64_t i = (int64_t)fl;
  const float f = (float)(pos - fl);
  float l0, r0, l1, r1, l2, r2, l3, r3;
  pool_.read(x, wrap_index(i - 1, L), l0, r0);
  pool_.read(x, wrap_index(i, L), l1, r1);
  pool_.read(x, wrap_index(i + 1, L), l2, r2);
  pool_.read(x, wrap_index(i + 2, L), l3, r3);
  auto cr = [f](float y0, float y1, float y2, float y3) {
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
  };
  l = cr(l0, l1, l2, l3);
  r = cr(r0, r1, r2, r3);
}

void Looper::write_add(int32_t b, uint64_t p, float l, float r) {
  Chunk* c = pool_.writable(b, (uint32_t)(p >> kChunkShift), true);
  if (!c) return;
  c->l[p & kChunkMask] += l;
  c->r[p & kChunkMask] += r;
}

// Хвост после закрытия цикла: затухает поверх начала цикла (стык без щелчка).
void Looper::seam_write(Track& t, float l, float r) {
  const uint64_t k = t.seam_t++;
  if (t.sum < 0 || t.len == 0) return;
  const float fo = 1.0f - (float)k / (float)xf_;
  write_add(t.sum, k % t.len, l * fo, r * fo);
}

// Огибающая овердаба на этот кадр входа (вход и выход по 10 мс).
float Looper::dub_env(Track& t) {
  float env = t.dub_t < xf_ ? (float)t.dub_t / (float)xf_ : 1.0f;
  if (t.dub_end >= 0) {
    float e2 = 1.0f - (float)t.dub_end / (float)xf_;
    if (e2 < env) env = e2;
  }
  ++t.dub_t;
  return env;
}

// Прибавить к слою и сумме в позиции p. false — кончилась память.
bool Looper::dub_put(int ti, uint64_t p, float l, float r) {
  Track& t = tr_[ti];
  Chunk* c = pool_.ensure(t.layer, (uint32_t)(p >> kChunkShift), true);
  if (!c && reserve(1, ti)) c = pool_.ensure(t.layer, (uint32_t)(p >> kChunkShift), true);
  if (!c) return false;
  c->l[p & kChunkMask] += l;
  c->r[p & kChunkMask] += r;
  write_add(t.sum, p, l, r);
  if (peaks_ && in_cur(ti)) live_peaks(ti, p);
  return true;
}

void Looper::dub_write(int ti, uint64_t p, float l, float r) {
  Track& t = tr_[ti];
  const float env = dub_env(t);
  if (!dub_put(ti, p, l * env, r * env)) end_dub_now(ti);  // память кончилась: овердаб закрывается
  t.wprev = (double)p;
  t.wl_prev = l;
  t.wr_prev = r;
  t.wfirst = false;
  if (t.dub_end >= 0 && ++t.dub_end >= (int64_t)xf_) finish_dub(ti);
}

// Овердаб на скорости не 1× (как на ленте): головка прошла от прошлой позиции до текущей,
// в каждую пройденную позицию лупа пишем вход, интерполированный между прошлым и текущим кадром.
// Куда в цикле пишется вход овердаба: он опоздал на задержку аудиоинтерфейса, а музыкант играл
// под то, что слышал, — на lat_ кадров раньше, это lat_ × шаг головки назад по циклу.
double Looper::dub_pos(const Track& t, double step, uint32_t extra) const {
  const uint32_t lat = lat_ + extra;  // extra: вход прошёл через Stretch овердаба, опоздал ещё на его задержку
  if (!lat || !t.len) return t.pos;
  const double L = (double)t.len;
  double w = std::fmod(t.pos - (double)lat * step, L);
  if (w < 0.0) w += L;
  return w;
}

void Looper::dub_resampled(int ti, float il, float ir, double step) {
  Track& t = tr_[ti];
  const double w1 = dub_pos(t, step, t.deng >= 0 ? t.dlat : 0);
  if (step == 1.0 && w1 == std::floor(w1)) {  // ровно 1×: как раньше, кадр в кадр
    dub_write(ti, wrap_index((int64_t)w1, t.len), il, ir);
    return;
  }
  const double L = (double)t.len;
  double d = t.wfirst ? step : w1 - t.wprev;  // сколько прошла головка за кадр
  if (d > L / 2) d -= L;
  else if (d < -L / 2) d += L;
  const double w0 = w1 - d;
  const float env = dub_env(t);
  bool ok = true;
  if (d > 0) {
    for (int64_t n = (int64_t)std::floor(w0) + 1; n <= (int64_t)std::floor(w1) && ok; ++n) {
      const float a = (float)(((double)n - w0) / d);
      ok = dub_put(ti, wrap_index(n, t.len), (t.wl_prev + (il - t.wl_prev) * a) * env,
                   (t.wr_prev + (ir - t.wr_prev) * a) * env);
    }
  } else if (d < 0) {
    for (int64_t n = (int64_t)std::ceil(w1); n < (int64_t)std::ceil(w0) && ok; ++n) {
      const float a = (float)((w0 - (double)n) / -d);
      ok = dub_put(ti, wrap_index(n, t.len), (t.wl_prev + (il - t.wl_prev) * a) * env,
                   (t.wr_prev + (ir - t.wr_prev) * a) * env);
    }
  }
  if (!ok) end_dub_now(ti);
  t.wprev = w1;
  t.wl_prev = il;
  t.wr_prev = ir;
  t.wfirst = false;
  if (t.dub_end >= 0 && ++t.dub_end >= (int64_t)xf_) finish_dub(ti);
}

// Аудиопоток дорисовывает колонку под позицией: волна суммы и текущего слоя растёт вживую.
void Looper::live_peaks(int ti, uint64_t p) {
  Track& t = tr_[ti];
  if (!t.len) return;
  PeakTrack& pk = peaks_->t[ti % ns_];
  const uint32_t c = (uint32_t)(p * kPeakCols / t.len);
  float l, r;
  pool_.read(pool_.buf(t.sum), p, l, r);
  int8_t lo = peak_q(l < r ? l : r), hi = peak_q(l > r ? l : r);
  if (lo < pk.sum.mn[c]) pk.sum.mn[c] = lo;
  if (hi > pk.sum.mx[c]) pk.sum.mx[c] = hi;
  pool_.read(pool_.buf(t.layer), p, l, r);
  lo = peak_q(l < r ? l : r);
  hi = peak_q(l > r ? l : r);
  if (lo < pk.dub.mn[c]) pk.dub.mn[c] = lo;
  if (hi > pk.dub.mx[c]) pk.dub.mx[c] = hi;
  t.plive = true;
}

void Looper::on_grid(bool beat, bool bar) {
  for (int ti = 0; ti < kTracks; ++ti) {
    Track& t = tr_[ti];
    if (t.st == TState::Armed && bar && t.rec_wait == 0) {
      if (t.count_n > 0 && t.count_end < 0) {
        t.count_end = std::floor(beats_ + 1e-9) + (double)t.count_n * bpb_;  // черта: начался счёт
      } else if (t.count_n == 0 || beats_ >= t.count_end - 0.5) {
        rec_on_boundary(ti);
      }
    }
    if (t.mute_pending && ((set_.mute_q == 1 && beat) || (set_.mute_q == 2 && bar))) apply_mute(ti);
    if (t.phase_wait && ((set_.mute_q == 1 && beat) || (set_.mute_q >= 2 && bar))) {
      t.phase_wait = false;
      resync(ti);
    }
  }
  if (ps_pending_ && ((set_.mute_q == 1 && beat) || (set_.mute_q >= 2 && bar))) {
    apply_playstop(ps_pending_ == 1);
    ps_pending_ = 0;
  }
  if (pend_part_ >= 0 && ((set_.part_q == 1 && beat) || (set_.part_q == 2 && bar))) switch_part(pend_part_);
}

void Looper::track_sample(int ti, uint32_t i, float il, float ir, float& ol, float& orr) {
  Track& t = tr_[ti];
  // Дорожка ушедшего парта: затихает по fade out, потом не обрабатывается.
  // Lock-копия ждёт конца овердаба — до тех пор старая дорожка звучит.
  const bool away = !in_cur(ti) && t.lock_to < 0;
  if (away && t.sgain <= 0.0f && t.xf_len == 0 && t.st != TState::Rec && t.st != TState::Closing &&
      t.st != TState::Dub)
    return;

  if (t.st == TState::Rec || t.st == TState::Closing) {
    const uint64_t f = t.rec_frames;
    Chunk* c = pool_.ensure(t.rec, (uint32_t)(f >> kChunkShift), false);
    if (!c && reserve(1, ti)) c = pool_.ensure(t.rec, (uint32_t)(f >> kChunkShift), false);
    if (!c) {  // память кончилась: закрываем как есть
      if (f >= 2ull * xf_ + 2) close_loop(ti, f);
      else cancel_rec(ti);
    } else {
      const float g = f < xf_ ? (float)f / (float)xf_ : 1.0f;  // fade in начала записи
      c->l[f & kChunkMask] = il * g;
      c->r[f & kChunkMask] = ir * g;
      rec_peaks(ti, il * g, ir * g);
      ++t.rec_frames;
      if (t.rec_target && t.rec_frames >= t.rec_target) {
        close_loop(ti, t.rec_target);
        return;  // этот кадр уже последний в цикле; следующий вход — первый кадр хвоста
      }
    }
  }

  const bool playing = t.st == TState::Play || t.st == TState::Dub;
  const bool fading = t.xf_len > 0;
  if (!playing && !fading) return;

  // чтение (с кроссфейдом со старой суммы)
  float l = 0.0f, r = 0.0f;
  if (playing) read_sum(t, t.sum, t.pos, l, r);
  // вход из тишины (старт цикла, переход в парт): то же нарастание и для сдвинутого звука
  const float fade_in = fading && t.xf_from < 0 ? (float)t.xf_pos / (float)t.xf_len : 1.0f;
  if (fading) {
    float fl, fr;
    read_sum(t, t.xf_from, t.xpos, fl, fr);
    const float g = (float)t.xf_pos / (float)t.xf_len;
    l = fl * (1.0f - g) + l * g;
    r = fr * (1.0f - g) + r * g;
    if (++t.xf_pos >= t.xf_len) {
      if (t.xf_owned && t.xf_from >= 0) pool_.free_buf(t.xf_from);
      t.xf_from = -1;
      t.xf_owned = false;
      t.xf_len = t.xf_pos = 0;
    }
  }
  // сдвиг высоты: выход Stretch (посчитан в начале блока) вместо прямого чтения, вход и выход — кроссфейдом
  if (t.eng >= 0 || t.pw > 0.0f) {
    const bool want = t.pit != 0.0f && !pitch_over_;
    const float ps = 1.0f / (kPitchXfS * (float)sr_);
    if (!want) t.pw = std::fmax(0.0f, t.pw - ps);
    else if (t.eng >= 0) t.pw = std::fmin(1.0f, t.pw + ps);
    float wl = 0.0f, wr = 0.0f;
    if (t.eng >= 0 && pblock_ && pb_) {
      wl = pb_->wet[t.eng][0][i] * fade_in;
      wr = pb_->wet[t.eng][1][i] * fade_in;
    }
    l = l * (1.0f - t.pw) + wl * t.pw;
    r = r * (1.0f - t.pw) + wr * t.pw;
  }
  // mute: плавно по fade out / fade in
  const float target = t.muted ? 0.0f : 1.0f;
  if (t.gain != target) {
    const float ms = t.muted ? set_.fade_out_ms : set_.fade_in_ms;
    const float step = 1000.0f / ((ms > 1.0f ? ms : 1.0f) * (float)sr_);
    t.gain = t.gain < target ? std::fmin(target, t.gain + step) : std::fmax(target, t.gain - step);
  }
  // остановка по транспорту: затухает по fade out, входит обратно по fade in
  const float starget = (t.stopped || t.ustop || away) ? 0.0f : 1.0f;
  if (t.sgain != starget) {
    const float ms = starget == 0.0f ? set_.fade_out_ms : set_.fade_in_ms;
    const float step = 1000.0f / ((ms > 1.0f ? ms : 1.0f) * (float)sr_);
    t.sgain = t.sgain < starget ? std::fmin(starget, t.sgain + step) : std::fmax(starget, t.sgain - step);
  }
  const float g = t.gain * t.sgain;
  const float* c = mcur_[ti];
  const float* d = mstep_[ti];
  const float fi = (float)i;
  const float yl = (l * (c[0] + d[0] * fi) + r * (c[1] + d[1] * fi)) * g;
  const float yr = (l * (c[2] + d[2] * fi) + r * (c[3] + d[3] * fi)) * g;
  ol += yl;
  orr += yr;
  const float a = std::fmax(std::fabs(yl), std::fabs(yr));
  if (a > bmax_[ti]) bmax_[ti] = a;

  // шаг головки за кадр: направление (разворот reverse) × скорость
  const double step = t.rate * t.spd;
  // запись: хвост на стыке и овердаб
  if (playing) {
    if (t.seam_t < xf_) seam_write(t, il, ir);
    if (t.st == TState::Dub && t.layer >= 0) {
      if (t.deng >= 0) {  // вход с обратным сдвигом (из Stretch овердаба)
        const bool have = pblock_ && pb_;
        dub_resampled(ti, have ? pb_->dout[t.deng][0][i] : 0.0f, have ? pb_->dout[t.deng][1][i] : 0.0f, step);
        if (t.st == TState::Dub && t.ddrain && t.dub_end < 0 && (t.ddrain_n == 0 || --t.ddrain_n == 0))
          t.dub_end = 0;  // хвост Stretch дописан: обычное затухание овердаба
      } else {
        dub_resampled(ti, il, ir, step);
      }
    }
  }

  // транспорт стоит и всё затихло: курсор в начале цикла до пуска (wake поставит его заново)
  if (hold_) {
    t.pos = t.xpos = t.dir > 0 ? 0.0 : (double)(t.len ? t.len - 1 : 0);
    return;
  }
  // движение: reverse разворачивается как лента (скорость плавно проходит через 0)
  uint64_t L = t.len;
  if (L == 0 && t.xf_from >= 0) L = pool_.buf(t.xf_from).len;
  if (L == 0) return;
  if (t.rev_wait >= 0 && t.rev_wait-- == 0) start_reverse(t, t.rev_n);
  if (t.rate_step != 0.0) {
    t.rate += t.rate_step;
    if ((t.rate_step > 0 && t.rate >= 1.0) || (t.rate_step < 0 && t.rate <= -1.0)) {
      t.rate = t.rate_step > 0 ? 1.0 : -1.0;
      t.rate_step = 0.0;
      if (t.spd == 1.0) t.pos = std::round(t.pos);  // на 1× дальше снова прямое чтение
    }
  }
  if (t.xf_len > 0 && t.xf_from >= 0) {
    const double XL = (double)pool_.buf(t.xf_from).len;
    t.xpos += step;
    if (XL > 0) {
      if (t.xpos >= XL) t.xpos -= XL;
      else if (t.xpos < 0.0) t.xpos += XL;
    }
  }
  t.pos += step;
  bool wrapped = false;
  if (t.pos >= (double)L) {
    t.pos -= (double)L;
    wrapped = true;
  } else if (t.pos < 0.0) {
    t.pos += (double)L;
    wrapped = true;
  }
  if (wrapped && t.mute_pending && set_.mute_q == 3) apply_mute(ti);
  if (wrapped && ti == master_ && pend_part_ >= 0 && set_.part_q == 3) part_due_ = true;
}

void Looper::set_time(const TimeInfo& ti) {
  bpf_ = ti.beats_per_frame;
  bpb_ = ti.bpb > 0 ? ti.bpb : 4;
  const bool started = ti.rolling && !rolling_;
  const bool jumped = ti.rolling && rolling_ && std::fabs(ti.beats - beats_) > 0.05;
  const bool stopped_now = !ti.rolling && rolling_;
  rolling_ = ti.rolling;
  if (rolling_) beats_ = ti.beats;  // сетка Zynthian; иначе продолжаем свой счёт
  // Транспорт пошёл или перескочил: лупы в Quantize встают в фазу с позицией Zynthian.
  // Пуск или перескок на начало (такт 1, доля 1): каждый луп Quantize — со своего начала цикла
  // (решения пользователя 2026-11-12); с другого места — в фазе с позицией, как раньше.
  const bool at_top = rolling_ && beats_ < 0.5;
  if (time_seen_ && pool_.ok()) {
    if (started) {
      if (at_top) {
        rebase_pending_ = false;  // загруженная сессия тоже с начала
        for (auto& t : tr_)
          if (t.synced) t.b0 = 0.0;
      }
      transport_start();
    } else if (jumped) {
      if (at_top)
        for (auto& t : tr_)
          if (t.synced) t.b0 = 0.0;
      for (int k = idx(0); k < idx(0) + ns_; ++k) resync(k);
    } else if (stopped_now) {
      transport_stop();
    }
  }
  time_seen_ = true;
}

// Транспорт Zynthian остановлен: играющие дорожки плавно затихают (овердаб закрывается).
// Запись основы не прерывается: лупер считает такты сам, пока она не закроется.
// Взведённая запись отменяется, ждущие черты Play/Stop и смена парта выполняются сразу:
// когда всё затихнет, счёт тактов и курсоры встают в 0 (hold_).
void Looper::transport_stop() {
  tstop_ = true;
  if (entry_pending_) {  // загруженная сессия ещё не вступила: вступит при пуске
    entry_pending_ = false;
    rebase_pending_ = true;
  }
  rec_after_switch_ = false;
  bool recording = false;
  for (auto& t : tr_) {
    if (t.st == TState::Armed) t.st = TState::Empty;
    t.rec_wait = 0;
    if (t.st == TState::Rec || t.st == TState::Closing) recording = true;
  }
  if (ps_pending_ == 1) apply_playstop(true);
  if (ps_pending_ == 2) {  // ждал старта: пойдёт вместе с транспортом
    user_stop_ = false;
    for (int k = idx(0); k < idx(0) + ns_; ++k) tr_[k].ustop = false;
  }
  ps_pending_ = 0;
  if (pend_part_ >= 0 && !recording) switch_part(pend_part_);
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (t.st != TState::Play && t.st != TState::Dub) continue;
    if (t.st == TState::Dub) end_dub(k);
    t.rev_wait = -1;
    t.stopped = true;
  }
}

// Транспорт пошёл: остановленные дорожки входят обратно в фазе с Zynthian
// (записанные в Free — с начала цикла), остальные встают в фазу.
void Looper::transport_start() {
  tstop_ = false;
  if (rebase_pending_) {  // загруженная сессия: фаза от точки пуска (черта такта)
    rebase_pending_ = false;
    session_rebase(std::floor(beats_ / bpb_ + 1e-9) * bpb_);
  }
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (!t.stopped) {
      if (!t.ustop) resync(k);
      continue;
    }
    t.stopped = false;
    if (!t.ustop) wake(k);  // остановленные педалью Play/Stop ждут педали
  }
}

// Дорожка снова звучит: в фазе с сеткой (записанные в Free — с начала цикла).
void Looper::wake(int ti) {
  Track& t = tr_[ti];
  if (t.sgain > 0.001f) {  // не успела затихнуть: перескок через кроссфейд
    resync(ti);
    return;
  }
  // тишина: просто ставим позицию, огибающая сама нарастёт
  if (t.synced && t.len && t.loop_beats > 0 && t.dir > 0 && t.rate_step == 0.0) {
    double ph = std::fmod((beats_ - t.b0) / t.loop_beats, 1.0);
    if (ph < 0) ph += 1.0;
    t.pos = std::round(ph * (double)t.len);
    if (t.pos >= (double)t.len) t.pos = 0.0;
  } else {
    t.pos = t.dir > 0 ? 0.0 : (double)(t.len ? t.len - 1 : 0);
  }
}

// Play/Stop: стоп глушит играющие дорожки (овердаб закрывается), старт запускает их
// с начала такта в фазе с сеткой. Действует только на лупер, транспорт Zynthian не трогает.
// Явный старт запускает и дорожки, остановленные транспортом.
void Looper::apply_playstop(bool stop) {
  user_stop_ = stop;
  if (!stop) tstop_ = false;
  if (!stop && (entry_pending_ || rebase_pending_)) {  // явный старт загруженной сессии: сейчас
    entry_pending_ = rebase_pending_ = false;
    session_rebase(beats_);
  }
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (t.st != TState::Play && t.st != TState::Dub) continue;
    if (stop) {
      if (t.st == TState::Dub) end_dub(k);
      t.rev_wait = -1;
      t.ustop = true;
    } else if (t.ustop || t.stopped) {
      t.ustop = t.stopped = false;
      wake(k);
    }
  }
}

// ---------------------------------------------------------------- скорость и следование темпу

float Looper::snap_speed(float s) const {
  if (set_.spd_snap == 0) return s;
  const double st = 12.0 * std::log2((double)s);
  const double q = set_.spd_snap == 2 ? 12.0 : 1.0;  // октавы или полутоны
  return (float)std::exp2(std::round(st / q) * q / 12.0);
}

void Looper::speed_reset(int k) {
  Track& t = tr_[k];
  t.mspd = 1.0f;
  t.phase_reset = true;  // доедет до 1× — встанет в фазу с тактом
}

// Раз в блок: плавный ход ручной скорости и следование темпу, итоговая скорость дорожек.
// Следование темпу (Quantize) — как у ленты: скорость = темп сейчас / темп при записи.
// Пока дорожка может стоять в фазе с Zynthian (ручная 1×, вперёд, играет), скорость и фаза
// ведутся одним критически задемпфированным звеном второго порядка: при смене темпа скорость
// плавно (без излома) идёт к новой, чуть переходит её (≈13 % от шага темпа) ровно настолько,
// чтобы отставание по фазе ушло само. Раньше скорость шла за фильтром темпа, а отставание
// догонялось отдельной подстройкой ±2 % — высота уходила выше цели почти на секунду («плывёт»).
void Looper::update_speeds(uint32_t n) {
  const double glide = std::fmax(1.0, (double)set_.tempo_glide_ms) / 1000.0 * sr_;  // кадров
  const double w = 4.0 / glide;                                 // собственная частота, 1/кадр
  const double kt = 1.0 - std::exp(-(double)n * w);             // без фазы: первый порядок, τ = glide/4
  const double emax = 0.01 * sr_;                               // ошибка фазы в расчёте не больше 10 мс
  const double sg = (double)set_.spd_glide_ms / 1000.0 / 4.0 * sr_;
  const double ks = sg < 1.0 ? 1.0 : 1.0 - std::exp(-(double)n / sg);
  for (int k = 0; k < kTracks; ++k) {
    Track& t = tr_[k];
    // ручная: плавный ход в логарифмах (равномерно по высоте)
    const double target = (double)snap_speed(t.mspd);
    if (t.mcur != target) {
      double lc = std::log2(t.mcur), lt = std::log2(target);
      lc += (lt - lc) * ks;
      t.mcur = std::fabs(lt - lc) < 1e-5 ? target : std::exp2(lc);
    }
    const bool follow = t.rec_q && set_.quantize && t.rec_bpf > 0.0 && bpf_ > 0.0;
    const bool can_lock = follow && t.synced && t.len && t.loop_beats > 0 && t.dir > 0 && t.rate_step == 0.0 &&
                          t.mcur == 1.0 && (t.st == TState::Play || t.st == TState::Dub) &&
                          in_cur(k) && !t.stopped && !t.ustop && t.xf_len == 0;
    if (!follow) {
      t.tspd = 1.0;
    } else if (!can_lock) {
      t.tspd += (bpf_ / t.rec_bpf - t.tspd) * kt;
    } else {
      const double L = (double)t.len;
      double ph = std::fmod((beats_ - t.b0) / t.loop_beats, 1.0);
      if (ph < 0) ph += 1.0;
      double err = ph * L - t.pos;  // > 0: дорожка отстаёт
      if (err > L / 2) err -= L;
      if (err < -L / 2) err += L;
      if (std::fabs(err) > 0.05 * sr_) {
        resync(k);  // далеко (больше 50 мс): перескок через кроссфейд
        t.tspd = bpf_ / t.rec_bpf;
      } else {
        const double r = bpf_ * L / t.loop_beats;  // скорость сетки в кадрах дорожки за кадр
        // шагами не длиннее 0,1/w: устойчиво при любом размере блока
        const int m = 1 + (int)((double)n * w / 0.1);
        const double dt = (double)n / m;
        for (int j = 0; j < m; ++j) {
          const double e = err > emax ? emax : (err < -emax ? -emax : err);
          t.tspd += (w * w * e + 2.0 * w * (r - t.tspd)) * dt;
          err += (r - t.tspd) * dt;
        }
      }
    }
    const double sp = t.mcur * t.tspd;
    t.spd = sp;
    // после сброса скорости — в фазу с тактом
    if (t.phase_reset && t.mcur == 1.0) {
      t.phase_reset = false;
      if (t.synced) {
        if (!set_.quantize || set_.mute_q == 0) resync(k);
        else t.phase_wait = true;
      }
    }
  }
}

// ---------------------------------------------------------------- сдвиг высоты (этап 8)

void Looper::set_pitch(int k, float st) {
  if (k < 0 || k >= kTracks) return;
  st = st < -kMaxPitch ? -kMaxPitch : (st > kMaxPitch ? kMaxPitch : st);
  if (std::fabs(st) < 1e-3f) st = 0.0f;
  if (st == tr_[k].pit) return;
  tr_[k].pit = st;
  if (pitch_over_) {  // перегрузка: новое значение — новая попытка
    pitch_over_ = false;
    pitch_load_ = 0.0;
  }
}

int Looper::pitch_latency() const { return pb_ ? pb_->lat : 0; }

void Looper::pitch_release(Track& t) {
  if (t.eng >= 0 && pb_) pb_->used[t.eng] = false;
  t.eng = -1;
  t.axf = false;
}

void Looper::dub_release(Track& t) {
  if (t.deng >= 0 && pb_) pb_->dused[t.deng] = false;
  t.deng = -1;
}

// n кадров суммы дорожки с позиции from шагом step (для запуска и перескока Stretch).
void Looper::pitch_fill(Track& t, float* l, float* r, uint32_t n, double from, double step) {
  const double L = (double)t.len;
  double p = wrap_pos(from, L);
  for (uint32_t j = 0; j < n; ++j) {
    read_sum(t, t.sum, p, l[j], r[j]);
    p = wrap_pos(p + step, L);
  }
}

// Раз в блок, до кадров: Stretch сдвинутых дорожек и овердаба считают весь блок.
// В Stretch идёт звук дорожки на его задержку D впереди по циклу (тем же шагом, что головка),
// поэтому выход совпадает с позицией. Скорость менялась и позиция ушла — перескок (seek) без обрыва.
void Looper::pitch_block(const float* in_l, const float* in_r, uint32_t n) {
  pblock_ = false;
  if (!pb_) return;
  PitchBank& b = *pb_;
  if (oseek_gap_ < (1u << 20)) oseek_gap_ += n;
  const bool ok = pool_.ok() && n > 0 && n <= kPitchBlock;
  const auto t0 = std::chrono::steady_clock::now();
  bool any = false;
  const double sg = (double)set_.spd_glide_ms / 1000.0 / 4.0 * sr_;
  const double ks = sg < 1.0 ? 1.0 : 1.0 - std::exp(-(double)n / sg);
  const int D = b.lat;
  const double jump = kPitchJumpS * sr_;
  for (int k = 0; k < kTracks; ++k) {
    Track& t = tr_[k];
    const bool want = t.pit != 0.0f && !pitch_over_;
    const bool has = (t.st == TState::Play || t.st == TState::Dub) && t.len > 0 && t.sum >= 0;
    const bool away = !in_cur(k) && t.lock_to < 0;
    const bool silent = t.sgain <= 0.0f && (t.stopped || t.ustop || away);
    if (!ok || !has || hold_ || silent) {
      // не звучит: Stretch не нужен; когда зазвучит — сразу со сдвигом (запуск в начале блока)
      pitch_release(t);
      t.pw = ok && has && want ? 1.0f : 0.0f;
      t.pcur = t.pit;
      continue;
    }
    if (!want && t.pw <= 0.0f) {
      pitch_release(t);
      t.pcur = 0.0;
      continue;
    }
    // плавный ход высоты, пока сдвинутый звук слышен; к нулю не едем — он уходит кроссфейдом
    if (t.pw <= 0.0f || t.eng < 0) {
      if (want) t.pcur = t.pit;
    } else if (want && t.pcur != t.pit) {
      t.pcur += ((double)t.pit - t.pcur) * ks;
      if (std::fabs((double)t.pit - t.pcur) < 1e-3) t.pcur = t.pit;
    }
    const double L = (double)t.len;
    const double step0 = t.rate * t.spd;
    if (t.eng < 0) {
      if (!want) {
        t.pw = 0.0f;
        continue;
      }
      if (oseek_gap_ < kOseekGap) continue;  // очередь: запуск дорогой, не больше одного за раз
      int e = -1;
      for (int j = 0; j < kPitchEng && e < 0; ++j)
        if (!b.used[j]) e = j;
      if (e < 0) {  // все заняты: дорожка звучит без сдвига
        t.pw = 0.0f;
        continue;
      }
      b.used[e] = true;
      t.eng = (int8_t)e;
      PitchShifter& sh = b.play[e];
      sh.set_semitones((float)t.pcur);
      // запуск с готовым выходом: история и прогон так, чтобы выход сейчас был звуком под головкой
      const int S = b.slen, W = b.wlen;
      const double X = t.pos + (double)(D - W) * step0;
      pitch_fill(t, b.sbuf[0], b.sbuf[1], (uint32_t)S, X - (double)S * step0, step0);
      pitch_fill(t, b.pbuf[0], b.pbuf[1], (uint32_t)W, X, step0);
      const float* hist[2] = {b.sbuf[0], b.sbuf[1]};
      const float* pre[2] = {b.pbuf[0], b.pbuf[1]};
      float* tmp[2] = {b.tbuf[0], b.tbuf[1]};
      sh.start(hist, pre, tmp);
      t.apos = wrap_pos(t.pos + (double)D * step0, L);
      t.axf = false;
      oseek_gap_ = 0;
    } else if (t.rate_step == 0.0) {  // во время разворота не прыгаем: после него — один перескок
      const double target = wrap_pos(t.pos + (double)D * step0, L);
      double err = target - t.apos;
      if (err > L / 2) err -= L;
      else if (err < -L / 2) err += L;
      if (std::fabs(err) > jump) {
        const int S = b.slen;
        pitch_fill(t, b.sbuf[0], b.sbuf[1], (uint32_t)S, target - (double)S * step0, step0);
        const float* in[2] = {b.sbuf[0], b.sbuf[1]};
        b.play[t.eng].seek(in, S);
        t.apos = target;
        t.axf = false;
      }
    }
    PitchShifter& sh = b.play[t.eng];
    sh.set_semitones((float)t.pcur);
    // undo, redo, reset дорожки: кроссфейд старой суммы и в Stretch (успевает до конца основного)
    const bool swap = t.xf_len > 0 && t.xf_from >= 0 && t.xf_from != t.sum;
    double XL = 0.0;
    if (swap) {
      XL = (double)pool_.buf(t.xf_from).len;
      if (!t.axf) {
        t.axf = true;
        t.ag = 0.0f;
        t.axpos = wrap_pos(t.xpos + (double)D * step0, XL);
      }
    } else {
      t.axf = false;
    }
    const uint32_t rem = swap ? t.xf_len - t.xf_pos : 0;
    const float ginc = rem ? (1.0f - t.ag) / (float)rem : 1.0f;
    double rt = t.rate, rs = t.rate_step;
    float* al = b.ain[0];
    float* ar = b.ain[1];
    for (uint32_t i = 0; i < n; ++i) {
      float l, r;
      read_sum(t, t.sum, t.apos, l, r);
      const double st = rt * t.spd;
      if (t.axf) {
        if (t.ag < 1.0f && XL > 0.0) {
          float ol, orr;
          read_sum(t, t.xf_from, t.axpos, ol, orr);
          l = ol * (1.0f - t.ag) + l * t.ag;
          r = orr * (1.0f - t.ag) + r * t.ag;
        }
        t.ag = std::fmin(1.0f, t.ag + ginc);
        t.axpos = wrap_pos(t.axpos + st, XL);
      }
      al[i] = l;
      ar[i] = r;
      t.apos = wrap_pos(t.apos + st, L);
      if (rs != 0.0) {
        rt += rs;
        if ((rs > 0 && rt >= 1.0) || (rs < 0 && rt <= -1.0)) {
          rt = rs > 0 ? 1.0 : -1.0;
          rs = 0.0;
        }
      }
    }
    const float* in[2] = {al, ar};
    float* out[2] = {b.wet[t.eng][0], b.wet[t.eng][1]};
    sh.process(in, out, (int)n);
    any = true;
  }
  // овердаб на сдвинутой дорожке: вход с обратным сдвигом
  for (int k = 0; ok && k < kTracks; ++k) {
    Track& t = tr_[k];
    if (t.deng < 0) continue;
    if (t.st != TState::Dub) {
      dub_release(t);
      continue;
    }
    PitchShifter& sh = b.dub[t.deng];
    sh.set_semitones(t.pit != 0.0f ? -(float)t.pcur : 0.0f);
    const float gs = 1.0f / (float)xf_;
    for (uint32_t i = 0; i < n; ++i) {
      if (t.ddrain) t.dgate = std::fmax(0.0f, t.dgate - gs);  // после закрытия вход плавно уходит
      b.din[0][i] = (in_l ? in_l[i] : 0.0f) * t.dgate;
      b.din[1][i] = (in_r ? in_r[i] : 0.0f) * t.dgate;
    }
    const float* in[2] = {b.din[0], b.din[1]};
    float* out[2] = {b.dout[t.deng][0], b.dout[t.deng][1]};
    sh.process(in, out, (int)n);
    any = true;
  }
  pblock_ = ok;
  // перегрузка: Stretch в среднем дольше заданной доли блока — сдвиг выключается (звук не рвётся)
  if (any) {
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    pitch_load_ += (el * sr_ / (double)n - pitch_load_) * 0.1;
    if (pitch_load_ > (double)pitch_budget_) pitch_over_ = true;
  } else {
    pitch_load_ *= 0.9;
  }
}

// ---------------------------------------------------------------- парты и Lock

bool Looper::part_has_content(int p) const {
  for (int k = p * ns_; k < (p + 1) * ns_; ++k) {
    const Track& t = tr_[k];
    if (t.sum >= 0 || t.st == TState::Rec || t.st == TState::Closing) return true;
  }
  return false;
}

uint32_t Looper::parts_mask() const {
  uint32_t m = 0;
  for (int p = 0; p < ns_; ++p)
    if (part_has_content(p)) m |= 1u << p;
  return m;
}

// По умолчанию первая дорожка каждого парта с Lock (решение пользователя).
void Looper::default_locks() {
  for (int p = 0; p < ns_; ++p) tr_[p * ns_].locked = true;
}

// Дорожка становится пустой и без истории (под Lock-копию в пустом парте).
void Looper::clear_track(int k) {
  Track& t = tr_[k];
  ++edit_epoch_;
  if (t.rec >= 0) pool_.free_buf(t.rec);
  if (t.layer >= 0) pool_.free_buf(t.layer);
  clear_redo(t);
  while (t.undo_n) {
    drop_entry_bufs(t, t.undo[t.undo_lo]);
    t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
    --t.undo_n;
  }
  if (t.sum >= 0) free_buf_safe(t, t.sum);
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  const uint32_t pgen = t.pgen;
  const bool inflight = t.pinflight;
  pitch_release(t);
  dub_release(t);
  t = Track();
  t.seam_t = kNoSeam;
  t.locked = k % ns_ == 0;
  t.pgen = pgen + 1;
  t.pdirty = true;
  t.pinflight = inflight;
}

// Lock-копия: новая дорожка делит куски памяти с оригиналом (copy-on-write) и продолжает
// с той же позиции в тот же сэмпл, а оригинал замолкает — шва нет. История копии с нуля.
void Looper::clone_into(int from, int to) {
  Track& o = tr_[from];
  if (o.sum < 0 || tr_[to].busy) return;
  ++edit_epoch_;
  clear_track(to);
  const int32_t b = pool_.clone_buf(o.sum);
  if (b < 0) return;
  Track& n = tr_[to];
  n.st = TState::Play;
  n.sum = b;
  n.len = o.len;
  n.pos = o.pos;
  n.rate = o.rate;
  n.dir = o.dir;
  n.rate_step = o.rate_step;
  n.rev_wait = o.rev_wait;
  n.rev_n = o.rev_n;
  n.synced = o.synced;
  n.rec_q = o.rec_q;
  n.b0 = o.b0;
  n.loop_beats = o.loop_beats;
  n.rec_bpf = o.rec_bpf;
  n.muted = o.muted;
  n.mute_pending = o.mute_pending;
  n.gain = o.gain;
  n.stopped = o.stopped;
  n.ustop = o.ustop;
  n.sgain = o.sgain;
  n.locked = true;
  n.mspd = o.mspd;
  n.mcur = o.mcur;
  n.spd = o.spd;
  n.tspd = o.tspd;
  // Stretch переходит к копии вместе с позицией: сдвинутый звук без шва
  n.pit = o.pit;
  n.pcur = o.pcur;
  n.pw = o.pw;
  n.eng = o.eng;
  n.apos = o.apos;
  n.axf = false;
  o.eng = -1;
  mix_[to] = mix_[from];
  for (int j = 0; j < 4; ++j) mcur_[to][j] = mcur_[from][j];
  ++mix_epoch_;
  o.sgain = 0.0f;  // звук продолжает копия
  o.rev_wait = -1;
  o.mute_pending = false;
}

// Переход в парт p. Пустой парт: дорожки с Lock продолжают там же в той же фазе,
// остальные затихают, активной становится первая свободная. Записанный парт:
// его лупы начинают с начала цикла, старый парт затихает. Запись и овердаб закрываются.
void Looper::switch_part(int p) {
  if (p == cur_part_ || p < 0 || p >= ns_) return;
  const int old = cur_part_;
  pend_part_ = -1;
  for (int i = 0; i < ns_; ++i) {
    const int k = old * ns_ + i;
    Track& t = tr_[k];
    if (t.st == TState::Armed) t.st = TState::Empty;
    else if (t.st == TState::Rec) request_close(k);
    else if (t.st == TState::Dub) end_dub(k);
    t.mute_pending = false;
  }
  const bool empty_target = !part_has_content(p);
  cur_part_ = p;
  if (empty_target) {
    int first_free = -1;
    for (int i = 0; i < ns_; ++i) {
      const int o = old * ns_ + i, n = p * ns_ + i;
      Track& ot = tr_[o];
      if (ot.locked && ot.sum >= 0 && (ot.st == TState::Play || ot.st == TState::Dub)) {
        if (ot.st == TState::Dub) ot.lock_to = n;  // копия после затухания овердаба
        else clone_into(o, n);
      } else if (first_free < 0) {
        first_free = i;
      }
    }
    active_ = first_free >= 0 ? first_free : 0;
  } else {
    // лупы парта — с начала цикла, и сетка Quantize считается от этого момента: иначе подстройка
    // фазы сразу вернула бы дорожку в фазу со старой сеткой (с середины цикла)
    double e = beats_;
    if (std::fabs(e - std::round(e)) < 0.02) e = std::round(e);  // переход на доле или черте
    for (int i = 0; i < ns_; ++i) {
      const int k = p * ns_ + i;
      Track& t = tr_[k];
      if (t.sum < 0 || (t.st != TState::Play && t.st != TState::Dub)) continue;
      if (t.synced) t.b0 = e;
      t.pos = t.dir > 0 ? 0.0 : (double)(t.len - 1);
      t.rate = (double)t.dir;
      t.rate_step = 0.0;
      t.rev_wait = -1;
      t.mute_pending = false;
      t.stopped = tstop_;
      t.ustop = user_stop_;
      t.sgain = (t.stopped || t.ustop) ? 0.0f : 1.0f;
      if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
      t.xf_from = -1;  // короткий fade in от начала цикла
      t.xf_owned = false;
      t.xf_pos = 0;
      t.xf_len = xf_;
      t.xpos = t.pos;
    }
  }
  for (int k = p * ns_; k < (p + 1) * ns_; ++k) mark_peaks(tr_[k]);
  if (peaks_)
    for (int i = 0; i < ns_; ++i) __atomic_store_n(&peaks_->t[i].dub_active, 0u, __ATOMIC_RELEASE);
  if (rec_after_switch_) {  // запись с границы перехода на активной дорожке нового парта
    rec_after_switch_ = false;
    const int k = idx(active_);
    Track& t = tr_[k];
    if (rec_blocked()) {
      // идёт сохранение сессии: новой записи нет
    } else if (t.st == TState::Empty) {
      if (rolling_) rec_on_boundary(k);  // запись только при запущенном транспорте
    } else if (t.st == TState::Play && !t.stopped && !t.ustop) {
      start_dub(k);
    }
  }
}

void Looper::resync(int ti) {
  Track& t = tr_[ti];
  if (!t.synced || t.len == 0 || t.loop_beats <= 0 || t.dir < 0 || t.rate_step != 0.0) return;
  if (t.st != TState::Play && t.st != TState::Dub) return;
  double ph = std::fmod((beats_ - t.b0) / t.loop_beats, 1.0);
  if (ph < 0) ph += 1.0;
  const double L = (double)t.len;
  const double want = std::round(ph * L);
  double diff = std::fabs(want - t.pos);
  if (diff > L / 2) diff = L - diff;
  if (diff < sr_ * 0.001) return;  // и так в фазе (меньше 1 мс)
  // перескок через кроссфейд: старая позиция затухает, новая нарастает
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = t.sum;
  t.xf_owned = false;
  t.xf_pos = 0;
  t.xf_len = xf_;
  t.xpos = t.pos;
  t.pos = want >= L ? 0.0 : want;
}

void Looper::process(const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t n,
                     const TimeInfo& ti) {
  set_time(ti);
  last_n_ = n;
  // микшер: к целевым значениям плавно (~20 мс), внутри блока — линейно
  const float ks = n ? 1.0f - (float)std::exp(-(double)n / (kMixSmoothS * sr_)) : 0.0f;
  for (int k = 0; k < kTracks; ++k) {
    float tgt[4];
    mix_matrix(mix_[k], tgt);
    for (int j = 0; j < 4; ++j) {
      const float end = mcur_[k][j] + (tgt[j] - mcur_[k][j]) * ks;
      mstep_[k][j] = n ? (end - mcur_[k][j]) / (float)n : 0.0f;
    }
    bmax_[k] = 0.0f;
  }
  const float mg0 = master_g_;
  master_g_ += (db_gain(master_db_) - master_g_) * ks;
  const float mgs = n ? (master_g_ - mg0) / (float)n : 0.0f;
  // сухой вход — через уровень, панораму и режим канала активной дорожки (mute дорожки его не глушит)
  {
    float tgt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (monitor_) mix_matrix(mix_[idx(active_)], tgt);
    for (int j = 0; j < 4; ++j) {
      const float end = dcur_[j] + (tgt[j] - dcur_[j]) * ks;
      dstep_[j] = n ? (end - dcur_[j]) / (float)n : 0.0f;
    }
  }
  const float lim_rel = (float)std::exp(-1.0 / (kLimRelS * sr_));
  pool_step();
  session_step();
  update_speeds(n);
  lat_ = (uint32_t)std::lround(std::fmin(100.0f, std::fmax(0.0f, set_.latency_ms)) * sr_ / 1000.0);
  float mbl = 0.0f, mbr = 0.0f;
  // «конец цикла» для смены парта — по самой длинной играющей дорожке текущего парта с Lock
  // (они переходят в новый парт), без Lock — по самой длинной вообще (решение 2026-11-12)
  master_ = -1;
  for (int pass = 0; pass < 2 && master_ < 0; ++pass) {
    uint64_t longest = 0;
    for (int k = idx(0); k < idx(0) + ns_; ++k) {
      const Track& t = tr_[k];
      if (pass == 0 && !t.locked) continue;
      if ((t.st == TState::Play || t.st == TState::Dub) && t.len > longest) {
        longest = t.len;
        master_ = k;
      }
    }
  }
  if (pend_part_ >= 0 && set_.part_q == 3 && master_ < 0) part_due_ = true;  // нечему доигрывать
  // Транспорт остановлен и лупы затихли: счёт тактов и курсоры стоят в 0.
  // Считаем дальше, пока что-то ждёт черты, звучит или идёт запись основы.
  hold_ = !rolling_ && ps_pending_ == 0 && pend_part_ < 0;
  for (int k = 0; hold_ && k < kTracks; ++k) {
    const Track& t = tr_[k];
    if (t.st == TState::Armed || t.st == TState::Rec || t.st == TState::Closing || t.st == TState::Dub ||
        t.xf_len != 0 || (t.st == TState::Play && t.sgain > 0.0f))
      hold_ = false;
  }
  if (hold_) beats_ = 0.0;
  pitch_block(in_l, in_r, n);
  const float thr = set_.rec_thresh_db < 0 ? db_gain((float)set_.rec_thresh_db) : 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    // компенсация задержки: запись начинается ровно через lat_ кадров после черты
    // (до проверки черты: взведённая в этом кадре дорожка начинает отсчёт со следующего)
    if (rec_waiting_) {
      rec_waiting_ = false;
      for (int k = 0; k < kTracks; ++k) {
        Track& t = tr_[k];
        if (!t.rec_wait) continue;
        if (t.st != TState::Armed) {
          t.rec_wait = 0;
        } else if (--t.rec_wait == 0) {
          start_rec(k);
          t.rec_lat = lat_;
        } else {
          rec_waiting_ = true;
        }
      }
    }
    // загруженная сессия вступает на ближайшем такте (и в Free: такты Zynthian есть всегда)
    if (entry_pending_ && rolling_) {
      const double c = std::floor(beats_ / bpb_);
      if (std::floor((beats_ - bpf_) / bpb_) != c) session_enter(c * bpb_);
    }
    // beats_ — доля этого кадра; граница там, где доля перешла через целое с прошлого кадра
    if (set_.quantize) {
      const int64_t a = (int64_t)std::floor(beats_ - bpf_), c = (int64_t)std::floor(beats_);
      if (a != c) on_grid(true, ((c % bpb_) + bpb_) % bpb_ == 0);
    } else {
      for (int k = 0; k < kTracks; ++k) {
        Track& t = tr_[k];
        if (t.st != TState::Armed || (t.count_n > 0 && beats_ < t.count_end)) continue;  // идёт счёт
        t.count_n = 0;
        t.count_end = -1.0;
        if (thr > 0.0f) {  // старт по порогу (после счёта, если он был)
          const float a = std::fmax(in_l ? std::fabs(in_l[i]) : 0.0f, in_r ? std::fabs(in_r[i]) : 0.0f);
          if (a < thr) continue;
          start_rec(k);
          prefill_rec(k);
        } else {
          start_rec(k);
        }
      }
    }
    if (part_due_) {  // самый длинный луп дошёл до конца цикла: переход
      part_due_ = false;
      if (pend_part_ >= 0) switch_part(pend_part_);
    }
    if (clones_pending_) {
      clones_pending_ = false;
      for (int k = 0; k < kTracks; ++k)
        if (tr_[k].lock_ready) {
          const int to = tr_[k].lock_to;
          tr_[k].lock_ready = false;
          tr_[k].lock_to = -1;
          clone_into(k, to);
        }
    }
    const float il = in_l ? in_l[i] : 0.0f;
    const float ir = in_r ? in_r[i] : 0.0f;
    float ol = 0.0f, orr = 0.0f;
    if (pool_.ok())
      for (int k = 0; k < kTracks; ++k) {
        const Track& t = tr_[k];  // пустая без затухания: делать нечего (дорожек до 36)
        if ((t.st == TState::Empty || t.st == TState::Armed) && t.xf_len == 0) continue;
        track_sample(k, i, il, ir, ol, orr);
      }
    pre_l_[pre_w_] = il;  // вход этого кадра — в историю (для старта по порогу)
    pre_r_[pre_w_] = ir;
    pre_w_ = (pre_w_ + 1) % kPreMax;
    if (pre_n_ < kPreMax) ++pre_n_;
    // мастер лупов: уровень, мягкий ограничитель, пики; сухой сигнал мимо ограничителя
    const float mg = mg0 + mgs * (float)i;
    ol *= mg;
    orr *= mg;
    if (lim_on_) {
      const float x = std::fmax(std::fabs(ol), std::fabs(orr));
      lim_env_ = x > lim_env_ ? x : x + (lim_env_ - x) * lim_rel;
      if (lim_env_ > kLimThr) {
        const float gr = kLimThr / lim_env_;
        ol *= gr;
        orr *= gr;
      }
      ol = soft_clip(ol);
      orr = soft_clip(orr);
    }
    if (std::fabs(ol) > mbl) mbl = std::fabs(ol);
    if (std::fabs(orr) > mbr) mbr = std::fabs(orr);
    const float fi = (float)i;
    if (out_l) out_l[i] = ol + il * (dcur_[0] + dstep_[0] * fi) + ir * (dcur_[1] + dstep_[1] * fi);
    if (out_r) out_r[i] = orr + il * (dcur_[2] + dstep_[2] * fi) + ir * (dcur_[3] + dstep_[3] * fi);
    if (!hold_) beats_ += bpf_;
  }
  // конец блока: коэффициенты микшера, пики с плавным спадом
  const float fall = (float)std::pow(10.0, -kPeakFallDbS * (double)n / sr_ / 20.0);
  for (int k = 0; k < kTracks; ++k) {
    for (int j = 0; j < 4; ++j) mcur_[k][j] += mstep_[k][j] * (float)n;
    tpeak_[k] = std::fmax(tpeak_[k] * fall, bmax_[k]);
  }
  for (int j = 0; j < 4; ++j) dcur_[j] += dstep_[j] * (float)n;
  mpeak_[0] = std::fmax(mpeak_[0] * fall, mbl);
  mpeak_[1] = std::fmax(mpeak_[1] * fall, mbr);
  if (peaks_)  // виджет перечитает дорисованные колонки
    for (int k = idx(0); k < idx(0) + ns_; ++k)
      if (tr_[k].plive) {
        __atomic_add_fetch(&peaks_->t[k % ns_].live, 1u, __ATOMIC_RELEASE);
        tr_[k].plive = false;
      }
}

// ---------------------------------------------------------------- сессии

// Всё затихло и рабочий поток пул не читает: пул можно отдать (загрузка сессии).
bool Looper::silent_for_swap() const {
  for (const auto& t : tr_)
    if (t.st != TState::Empty || t.xf_len > 0 || t.pinflight) return false;
  return !job_pending_ && !job_running_;
}

// Звук можно заморозить для сохранения: ничего не пишется и ничего не ждёт записи.
bool Looper::can_freeze() const {
  if (!pool_.ok() || pool_state_ != PS_IDLE || pool_msg_ || load_state_ != LS_IDLE) return false;
  if (job_pending_ || job_running_ || grid_pending_ >= 0 || reset_all_pending_ || clones_pending_) return false;
  if (rec_after_switch_) return false;
  for (const auto& t : tr_) {
    if (t.st == TState::Armed || t.st == TState::Rec || t.st == TState::Closing || t.st == TState::Dub) return false;
    if (t.seam_t < xf_ || t.lock_to >= 0 || t.busy) return false;  // хвост стыка ещё пишется в сумму
  }
  return true;
}

void Looper::session_step() {
  if (pool_.ok()) pool_chunks_.store(pool_.total_chunks(), std::memory_order_relaxed);
  // сохранение: заморозка по запросу потока save()
  const bool req = freeze_req_.load(std::memory_order_acquire);
  const bool fz = frozen_.load(std::memory_order_relaxed);
  if (req && !fz) {
    if (can_freeze()) {
      SessionDesc& d = frozen_desc_;
      d = SessionDesc();
      d.sr = sr_;
      d.grid = ns_;
      d.part = cur_part_;
      d.active = active_;
      for (int k = 0; k < ns_ * ns_; ++k) {
        const Track& t = tr_[k];
        SessionTrack& s = d.t[k];
        s.locked = t.locked;
        if (t.sum < 0 || t.len == 0 || t.st != TState::Play) continue;
        s.used = true;
        s.buf = t.sum;
        s.len = t.len;
        s.muted = t.muted;
        s.reversed = t.dir < 0;
        s.synced = t.synced;
        s.loop_beats = t.loop_beats;
        s.rec_bpm = t.rec_bpf * 60.0 * sr_;
        s.b0 = t.b0;
      }
      frozen_epoch_ = edit_epoch_;
      sess_status_.store(SS_SAVING, std::memory_order_relaxed);
      frozen_.store(true, std::memory_order_release);
    }
  } else if (!req && fz) {
    frozen_.store(false, std::memory_order_release);
    if (grid_pending_ > 0 && !job_pending_ && !job_running_) apply_grid();  // ждала сохранения
  }
  // загрузка: принять новый запрос (не во время сохранения и смены пула)
  const uint32_t g = load_gen_.load(std::memory_order_acquire);
  if (g != load_seen_) {
    if (load_state_ == LS_IDLE) {
      if (req || fz || pool_state_ != PS_IDLE) return;
      load_seen_ = g;
      // та же сессия, звук с её сохранения или загрузки не менялся (Zynthian грузит пресет сам сразу
      // после сохранения и при листании списка): грузить нечего
      if (load_id_.load(std::memory_order_relaxed) == cur_id_.load(std::memory_order_relaxed) &&
          edit_epoch_ == saved_epoch_.load(std::memory_order_relaxed))
        return;
      sess_status_.store(SS_LOADING, std::memory_order_relaxed);
      sess_done_.store(0, std::memory_order_relaxed);
      load_state_ = LS_REQ;
    } else if (load_state_ != LS_LOADING) {
      load_seen_ = g;  // ещё не начали читать: прочитаем самый свежий
    }  // читаем: рабочий поток увидит новый номер и бросит, work_done начнёт заново
  }
  if (load_state_ == LS_REQ) {
    if (job_pending_ || job_running_) return;  // undo/redo дочитает пул
    reset_all_now(false);                      // лупы затихают по кроссфейду, микшер остаётся
    load_state_ = LS_DRAIN;
  }
  if (load_state_ == LS_DRAIN && silent_for_swap()) {
    pool_.swap(spare_);  // пул у рабочего потока: лупер молчит, сухой сигнал идёт
    load_msg_ = true;
    load_state_ = LS_LOADING;
  }
}

// Звук сессии в пуле: дорожки как были, все ждут вступления.
void Looper::install_session(const SessionDesc& d) {
  ++edit_epoch_;
  if (valid_grid(d.grid)) ns_ = d.grid;  // дорожки пустые: сбрасывать нечего
  const int n = ns_ * ns_;
  for (int k = 0; k < kTracks; ++k) {
    Track& t = tr_[k];
    if (k < n) t.locked = d.t[k].locked;
    const SessionTrack& s = d.t[k];
    if (!s.used || s.buf < 0 || k >= n) continue;
    t.sum = s.buf;
    t.len = s.len;
    t.st = TState::Play;
    t.dir = s.reversed ? -1 : 1;
    t.rate = (double)t.dir;
    t.rate_step = 0.0;
    t.pos = t.xpos = s.reversed ? (double)(s.len - 1) : 0.0;
    t.muted = s.muted;
    t.mute_pending = false;
    t.gain = s.muted ? 0.0f : 1.0f;
    t.synced = t.rec_q = s.synced;
    t.loop_beats = s.loop_beats;
    t.rec_bpf = s.rec_bpm > 0 ? s.rec_bpm / 60.0 / sr_ : 0.0;
    t.b0 = s.b0;
    t.tspd = 1.0;
    t.stopped = true;  // молчит до вступления
    t.sgain = 0.0f;
    t.seam_t = ~0ull;
    mark_peaks(t);
  }
  cur_part_ = d.part >= 0 && d.part < ns_ ? d.part : 0;
  active_ = d.active >= 0 && d.active < ns_ ? d.active : 0;
  if (rolling_) {
    entry_pending_ = true;  // на ближайшем такте
  } else {
    tstop_ = true;  // как после остановки транспорта: вступит при пуске
    rebase_pending_ = true;
  }
  cur_id_.store(d.id, std::memory_order_relaxed);
  saved_epoch_.store(edit_epoch_, std::memory_order_relaxed);
}

// Сдвинуть сетку сессии: самая длинная дорожка в Quantize начинает цикл с доли e,
// остальные — в той же взаимной фазе, что при сохранении.
void Looper::session_rebase(double e) {
  int m = -1;
  for (int pass = 0; pass < 2 && m < 0; ++pass)  // сначала текущий парт, потом любой
    for (int k = 0; k < ns_ * ns_; ++k) {
      const Track& t = tr_[k];
      if (pass == 0 && !in_cur(k)) continue;
      if (t.st != TState::Play || !t.synced || !t.len) continue;
      if (m < 0 || t.loop_beats > tr_[m].loop_beats) m = k;
    }
  if (m < 0) return;
  const double d = e - tr_[m].b0;
  for (auto& t : tr_)
    if (t.synced) t.b0 += d;
}

void Looper::session_enter(double e) {
  entry_pending_ = rebase_pending_ = false;
  session_rebase(e);
  for (int k = 0; k < kTracks; ++k) {
    Track& t = tr_[k];
    if (t.st != TState::Play || !t.stopped) continue;
    t.stopped = false;
    if (in_cur(k) && !t.ustop) wake(k);  // фаза с сеткой (Free — с начала цикла), огибающая нарастёт
  }
}

// Поток save(): звук заморожен, записать WAV и заполнить описание.
int Looper::session_save_audio(const char* dir, SessionDesc& out) {
  if (!frozen_.load(std::memory_order_acquire)) return SS_E_WRITE;
  SessionDesc d = frozen_desc_;
  d.id = session_new_id();
  uint64_t total = 0;
  for (const auto& s : d.t) total += s.used ? s.len : 0;
  sess_total_.store(total, std::memory_order_relaxed);
  sess_done_.store(0, std::memory_order_relaxed);
  for (int k = 0; k < kTracks; ++k) {
    SessionTrack& s = d.t[k];
    if (!s.used) continue;
    session_file_name(k, d.grid, s.file, sizeof s.file);
    // сумма слоёв бывает громче 0 дБ: такой файл пишется тише, множитель — в описании
    const float pk = buf_peak(pool_, s.buf, s.len);
    s.gain = pk > 0.999f ? pk / 0.999f : 1.0f;
    std::string path = dir ? dir : "";
    if (!path.empty() && path.back() != '/') path += '/';
    path += s.file;
    if (!wav24_write(path.c_str(), pool_, s.buf, s.len, sr_, s.gain, &sess_done_)) return SS_E_WRITE;
  }
  out = d;
  return SS_SAVED;
}

void Looper::session_unfreeze(bool saved, uint64_t id) {
  if (saved) {
    saved_epoch_.store(frozen_epoch_, std::memory_order_relaxed);
    cur_id_.store(id, std::memory_order_relaxed);
    sess_status_.store(SS_SAVED, std::memory_order_relaxed);
  }
  freeze_req_.store(false, std::memory_order_release);
}

void Looper::session_load(const SessionDesc& d, const char* dir) {
  uint32_t g;
  {
    std::lock_guard<std::mutex> lk(load_mx_);
    load_desc_ = d;
    load_dir_ = dir ? dir : "";
    g = ++load_slot_gen_;
  }
  load_id_.store(d.id, std::memory_order_relaxed);
  load_gen_.store(g, std::memory_order_release);
}

float Looper::session_progress() const {
  const uint64_t t = sess_total_.load(std::memory_order_relaxed);
  return t ? (float)((double)sess_done_.load(std::memory_order_relaxed) / (double)t) : 0.0f;
}

// ---------------------------------------------------------------- состояние

TrackView Looper::view(int i) const {
  const Track& t = tr_[idx(i)];
  TrackView v{};
  switch (t.st) {
    case TState::Empty: v.state = 0; break;
    case TState::Armed: v.state = 1; break;
    case TState::Rec:
    case TState::Closing: v.state = 2; break;
    case TState::Play: v.state = 3; break;
    case TState::Dub: v.state = 4; break;
  }
  v.speed = (float)t.mcur;
  v.espeed = (float)t.spd;
  v.muted = t.muted;
  v.stopped = t.stopped || t.ustop;
  v.locked = t.locked;
  v.reversed = t.dir < 0;
  v.can_undo = t.undo_n > 0 || t.st == TState::Dub;
  v.can_redo = t.redo_n > 0;
  v.layers = t.undo_n;
  v.pitch = t.pit;
  v.count = 0;
  if (t.st == TState::Armed && t.count_n > 0 && t.count_end >= 0) {
    const double left = std::ceil(t.count_end - beats_ - 1e-9);
    v.count = (uint8_t)(left < 1 ? 1 : (left > 64 ? 64 : left));
  }
  if (t.len) {
    v.pos = (float)(t.pos / (double)t.len);
    v.len_s = (float)((double)t.len / sr_);
  } else if (t.st == TState::Rec || t.st == TState::Closing) {
    v.len_s = (float)((double)t.rec_frames / sr_);
  }
  return v;
}

float Looper::pool_fill() const {
  uint32_t tot = pool_.total_chunks();
  return tot ? 1.0f - (float)pool_.free_chunks() / (float)tot : 0.0f;
}

}  // namespace alp
