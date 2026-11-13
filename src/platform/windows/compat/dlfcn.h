/* dlopen on LoadLibrary (UTF-8 paths), dlsym on GetProcAddress. */
#ifndef SB_COMPAT_DLFCN_H
#define SB_COMPAT_DLFCN_H
#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0x0
#define RTLD_GLOBAL 0x100
#define RTLD_NOLOAD 0x4 /* only a library already loaded */
void *dlopen(const char *path, int flags);
void *dlsym(void *lib, const char *name);
int dlclose(void *lib);
const char *dlerror(void);
#endif
