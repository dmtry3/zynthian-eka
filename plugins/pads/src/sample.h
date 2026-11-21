/* Eka Pads: sample loader (libsndfile). Runs in LV2 worker thread and tests. */
#ifndef EP_SAMPLE_H
#define EP_SAMPLE_H

#include <stdint.h>

#define EP_PATH_MAX 1024
#define EP_GUARD    4       /* extra zero/clamped frames at buffer end for safe interpolation */

typedef struct {
  float* data;              /* interleaved stereo (L, R, L, R, ...), frames + EP_GUARD frames */
  int64_t frames;
  int channels;
  double rate;
  float peak;
  char path[EP_PATH_MAX];
} EpSample;

enum {
  EP_LOAD_OK = 0,
  EP_LOAD_OPEN,
  EP_LOAD_EMPTY,
  EP_LOAD_TOO_LONG,
  EP_LOAD_NOMEM,
  EP_LOAD_PATH
};

EpSample* ep_sample_load(const char* path, double max_seconds, int* err);
void ep_sample_free(EpSample* s);

#endif
