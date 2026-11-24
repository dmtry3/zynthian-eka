#include "sample.h"
#include <math.h>
#include <sndfile.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 4096

void ep_sample_free(EpSample* s) {
  if (!s) return;
  free(s->data);
  free(s);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static int decode_uri_path(const char* src, char* dst, size_t cap) {
  if (!src || cap < 2) return 0;
  if (strncmp(src, "file://", 7) == 0) src += 7;
  size_t w = 0;
  while (*src && w + 1 < cap) {
    if (src[0] == '%' && src[1] && src[2]) {
      int hi = hex_val(src[1]), lo = hex_val(src[2]);
      if (hi >= 0 && lo >= 0) {
        dst[w++] = (char)((hi << 4) | lo);
        src += 3;
        continue;
      }
    }
    dst[w++] = *src++;
  }
  while (w > 0 && (dst[w - 1] == '\n' || dst[w - 1] == '\r')) w--;
  dst[w] = '\0';
  return w > 0;
}

EpSample* ep_sample_load(const char* path, double max_seconds, int* err) {
  int e = EP_LOAD_OK;
  EpSample* s = NULL;
  float* tmp = NULL;
  SNDFILE* f = NULL;
  SF_INFO info;
  memset(&info, 0, sizeof info);

  if (!path || !*path || strlen(path) >= EP_PATH_MAX) { e = EP_LOAD_PATH; goto out; }
  f = sf_open(path, SFM_READ, &info);
  if (!f) {
    char alt[EP_PATH_MAX];
    if (decode_uri_path(path, alt, sizeof alt) && strcmp(alt, path) != 0) {
      memset(&info, 0, sizeof info);
      f = sf_open(alt, SFM_READ, &info);
    }
  }
  if (!f) { e = EP_LOAD_OPEN; goto out; }
  if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) { e = EP_LOAD_EMPTY; goto out; }
  if ((double)info.frames > max_seconds * info.samplerate) { e = EP_LOAD_TOO_LONG; goto out; }

  const int ch = info.channels;
  s = (EpSample*)calloc(1, sizeof(EpSample));
  tmp = (float*)malloc((size_t)CHUNK * (size_t)ch * sizeof(float));
  if (!s || !tmp) { e = EP_LOAD_NOMEM; goto out; }

  s->frames = info.frames;
  s->data = (float*)malloc((size_t)(info.frames + EP_GUARD) * 2 * sizeof(float));
  if (!s->data) { e = EP_LOAD_NOMEM; goto out; }

  float* d = s->data;
  int64_t n = 0;
  while (n < info.frames) {
    sf_count_t want = info.frames - n < CHUNK ? info.frames - n : CHUNK;
    sf_count_t got = sf_readf_float(f, tmp, want);
    if (got <= 0) break;
    float* o = d + 2 * n;
    if (ch == 1) {
      for (sf_count_t i = 0; i < got; i++) {
        o[2 * i] = tmp[i];
        o[2 * i + 1] = tmp[i];
      }
    } else {
      for (sf_count_t i = 0; i < got; i++) {
        o[2 * i] = tmp[i * ch];
        o[2 * i + 1] = tmp[i * ch + 1];
      }
    }
    n += got;
  }
  if (n == 0) { e = EP_LOAD_EMPTY; goto out; }

  /* Zero any unread tail and guard frames (also pre-faults all pages in worker thread). */
  memset(d + 2 * n, 0, (size_t)(info.frames + EP_GUARD - n) * 2 * sizeof(float));

  float pk = 0.0f;
  for (int64_t i = 0; i < 2 * n; i++) {
    float a = fabsf(d[i]);
    if (a > pk) pk = a;
  }

  s->frames = n;
  s->channels = ch;
  s->rate = info.samplerate;
  s->peak = pk;
  strcpy(s->path, path);

out:
  if (f) sf_close(f);
  free(tmp);
  if (e != EP_LOAD_OK) { ep_sample_free(s); s = NULL; }
  if (err) *err = e;
  return s;
}
