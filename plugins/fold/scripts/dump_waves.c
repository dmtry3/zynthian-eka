/* Writes the tables of the engines for the widget, which draws the waves from them (run by build.sh):
 * the integrated waves of Wave (int16, 192 x 132 points), then the FM2 ratio quantizer (float32, 130 values,
 * semitones), then the modal resonator's stiffness by Structure (float32, 65 values); little-endian.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "../src/plaits/engines.h"
#include "../src/plaits/resonators.h"

static void put(FILE* f, const void* p, int n) {
  const unsigned char* b = (const unsigned char*)p;
  const int one = 1;
  if (*(const char*)&one) { fwrite(b, 1, (size_t)n, f); return; }
  for (int i = n - 1; i >= 0; i--) fputc(b[i], f);         /* big-endian host: swap */
}

int main(int argc, char** argv) {
  if (argc < 2) return 1;
  FILE* f = fopen(argv[1], "wb");
  if (!f) return 1;
  for (int i = 0; i < FE_WAVES_SIZE; i++) put(f, &fe_waves[i], 2);
  for (int i = 0; i < FE_FM_QUANTIZER_SIZE; i++) put(f, &fe_fm_quantizer[i], 4);
  for (int i = 0; i < FR_STIFFNESS_SIZE; i++) put(f, &fr_stiffness[i], 4);
  return fclose(f) ? 1 : 0;
}
