// Extracts the 3.3.5a client's default UI code (Interface\FrameXML, GlueXML and the
// Blizzard_* addons: .lua, .xml, .toc) from its MPQs, using AzerothCore's libmpq. Archives
// are given highest priority first; the first copy of a file wins.
// Usage: mpq_extract <out_dir> <mpq>...  (built and run by tools/extract-client-ui.sh)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mpq.h"
static uint8_t* read_file(mpq_archive_s* a, const char* name, libmpq__off_t* size) {
  uint32_t n;
  if (libmpq__file_number(a, name, &n)) return NULL;
  if (libmpq__file_unpacked_size(a, n, size)) return NULL;
  uint8_t* buf = malloc(*size + 1);
  libmpq__off_t got;
  if (libmpq__file_read(a, n, buf, *size, &got)) { free(buf); return NULL; }
  buf[*size] = 0;
  return buf;
}
static void mkdirs(char* path) {
  for (char* p = path + 1; *p; ++p) if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
}
int main(int argc, char** argv) {
  int total = 0;
  for (int i = 2; i < argc; ++i) {
    mpq_archive_s* a;
    if (libmpq__archive_open(&a, argv[i], -1)) { fprintf(stderr, "skip %s\n", argv[i]); continue; }
    libmpq__off_t size;
    char* list = (char*)read_file(a, "(listfile)", &size);
    int count = 0;
    for (char* line = list ? strtok(list, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
      if (strncasecmp(line, "Interface\\FrameXML\\", 19) && strncasecmp(line, "Interface\\AddOns\\Blizzard_", 26) &&
          strncasecmp(line, "Interface\\GlueXML\\", 18)) continue;
      const char* ext = strrchr(line, '.');
      if (!ext || (strcasecmp(ext, ".lua") && strcasecmp(ext, ".xml") && strcasecmp(ext, ".toc"))) continue;
      char out[4096];
      snprintf(out, sizeof out, "%s/%s", argv[1], line);
      for (char* p = out; *p; ++p) if (*p == '\\') *p = '/';
      struct stat st;
      if (stat(out, &st) == 0) continue;  // A higher-priority archive already had it.
      libmpq__off_t fsize;
      uint8_t* data = read_file(a, line, &fsize);
      if (!data) continue;
      mkdirs(out);
      FILE* f = fopen(out, "wb");
      fwrite(data, 1, fsize, f);
      fclose(f);
      free(data);
      ++count;
    }
    free(list);
    libmpq__archive_close(a);
    printf("%s: %d files\n", argv[i], count);
    total += count;
  }
  printf("total %d\n", total);
  return 0;
}
