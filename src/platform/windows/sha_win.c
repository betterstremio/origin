/* A file's SHA-256 (the updater's check), apart from windows.h: sha256.h has a WORD of its own. */
#include "sha256.h"
#include <stdio.h>
#include <wchar.h>
int win_sha256_file(const wchar_t *path, char hex[65]) {
  FILE *f = _wfopen(path, L"rb"); if (!f) return 0;
  SHA256_CTX h; sha256_init(&h); static unsigned char buf[65536]; unsigned char d[32]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&h, buf, n);
  fclose(f); sha256_final(&h, d);
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
  return 1;
}
