/* Libraries the app loads on demand (the torrent engine, the sound converter), as on webOS: dlopen first. A Samsung
   refuses to map code from the app's own files (Tizen 4-5.5: "failed to map segment"), so there the library is read and
   laid out here instead, in anonymous memory made executable, which the TV allows (the app's own core is loaded the
   same way by the host). What a library needs from a loader is done here: its relocations (ARM: relative, absolute,
   GOT, PLT, TLS), the libraries it names loaded first, its thread-local data (per thread, made on first use), its
   initialisers, and C++ exceptions (the unwinder finds its tables through dl_iterate_phdr / __gnu_Unwind_Find_exidx,
   which answer for these libraries too). host_bridge.h turns dlopen / dlsym / dlerror into the functions below on Tizen only. */
#ifdef SB_TPK
#include <dlfcn.h>
#include <malloc.h>
/* the system's own loader, by its real names (host_bridge.h renames dlopen / dlsym / dlerror to the functions below) */
extern void *sys_dlopen(const char *, int) __asm__("dlopen");
extern void *sys_dlsym(void *, const char *) __asm__("dlsym");
extern char *sys_dlerror(void) __asm__("dlerror");
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef R_ARM_TLS_DTPMOD32
#define R_ARM_TLS_DTPMOD32 17
#define R_ARM_TLS_DTPOFF32 18
#define R_ARM_TLS_TPOFF32 19
#endif
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif

typedef struct { const char *name; uintptr_t val; } Sym;
typedef struct Mod {
  char path[512]; unsigned char *base; size_t span; uintptr_t bias; /* bias: added to the file's virtual addresses */
  Elf32_Phdr *phdr; int phnum; uintptr_t exidx; unsigned exidxN;
  const char *strtab; Elf32_Sym *symtab; unsigned nsyms; Sym *tab; unsigned ntab; /* tab: its own definitions, sorted by name */
  /* thread-local data: the template (initial bytes + zeroed rest), a key for each thread's copy */
  const unsigned char *tlsInit; size_t tlsInitSz, tlsSz, tlsAlign; pthread_key_t tlsKey; int tlsId;
  struct Mod *next;
} Mod;
static Mod *mods; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static char lastErr[400];
static int nextTlsId = 0x5b000; /* (module ids of ours: far from the system loader's small ones) */

static void *(*real_tls_get_addr)(void *);
static int (*real_iter)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
static uintptr_t (*real_find_exidx)(uintptr_t, int *);

typedef struct { unsigned long module, offset; } TlsIndex;
static void *my_tls_get_addr(TlsIndex *ti) {
  for (Mod *m = mods; m; m = m->next) if (m->tlsId && (unsigned long)m->tlsId == ti->module) {
    unsigned char *blk = pthread_getspecific(m->tlsKey);
    if (!blk) { blk = memalign(m->tlsAlign > 8 ? m->tlsAlign : 8, m->tlsSz ? m->tlsSz : 1); if (!blk) return NULL;
      memcpy(blk, m->tlsInit, m->tlsInitSz); memset(blk + m->tlsInitSz, 0, m->tlsSz - m->tlsInitSz); pthread_setspecific(m->tlsKey, blk); }
    return blk + ti->offset; }
  return real_tls_get_addr ? real_tls_get_addr(ti) : NULL;
}
/* the system's libraries, then ours (an unwinder walks these to find a frame's tables) */
static int my_iterate(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
  int r = real_iter ? real_iter(cb, data) : 0; if (r) return r;
  for (Mod *m = mods; m; m = m->next) { struct dl_phdr_info i; memset(&i, 0, sizeof i); i.dlpi_addr = m->bias; i.dlpi_name = m->path; i.dlpi_phdr = m->phdr; i.dlpi_phnum = (ElfW(Half))m->phnum;
    r = cb(&i, sizeof i, data); if (r) return r; }
  return 0;
}
static uintptr_t my_find_exidx(uintptr_t pc, int *n) {
  for (Mod *m = mods; m; m = m->next) if (pc >= (uintptr_t)m->base && pc < (uintptr_t)m->base + m->span && m->exidx) { *n = (int)m->exidxN; return m->exidx; }
  return real_find_exidx ? real_find_exidx(pc, n) : 0;
}
static void *my_dlopen(const char *p, int f); static void *my_dlsym(void *h, const char *n); static char *my_dlerror(void);
/* names a loaded library is given ours for: its TLS, the walk of loaded objects, and dlopen / dlsym inside it */
static uintptr_t interposed(const char *n) {
  if (!strcmp(n, "__tls_get_addr")) return (uintptr_t)my_tls_get_addr;
  if (!strcmp(n, "dl_iterate_phdr")) return (uintptr_t)my_iterate;
  if (!strcmp(n, "__gnu_Unwind_Find_exidx")) return (uintptr_t)my_find_exidx;
  if (!strcmp(n, "dlopen")) return (uintptr_t)my_dlopen;
  if (!strcmp(n, "dlsym")) return (uintptr_t)my_dlsym;
  if (!strcmp(n, "dlerror")) return (uintptr_t)my_dlerror;
  return 0;
}
static int sym_cmp(const void *a, const void *b) { return strcmp(((const Sym *)a)->name, ((const Sym *)b)->name); }
static uintptr_t own(Mod *m, const char *n) {
  Sym k = { n, 0 }; Sym *s = bsearch(&k, m->tab, m->ntab, sizeof *m->tab, sym_cmp); return s ? s->val : 0;
}
static uintptr_t resolve_sym(Mod *m, const char *n, int weak, int *missing) {
  uintptr_t v = interposed(n); if (v) return v;
  if ((v = own(m, n))) return v;
  for (Mod *o = mods; o; o = o->next) if (o != m && (v = own(o, n))) return v;
  void *s = sys_dlsym(RTLD_DEFAULT, n); if (s) return (uintptr_t)s;
  if (!weak) { *missing = 1; snprintf(lastErr, sizeof lastErr, "%s: undefined symbol %s", m->path, n); }
  return 0;
}

static Mod *load(const char *path) {
  FILE *f = fopen(path, "rb"); if (!f) { snprintf(lastErr, sizeof lastErr, "%s: cannot read", path); return NULL; }
  fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *file = malloc((size_t)fsz); if (!file || fread(file, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(file); snprintf(lastErr, sizeof lastErr, "%s: read failed", path); return NULL; } fclose(f);
  Elf32_Ehdr *eh = (Elf32_Ehdr *)file;
  if (fsz < (long)sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM) { free(file); snprintf(lastErr, sizeof lastErr, "%s: not a 32-bit ARM library", path); return NULL; }
  Elf32_Phdr *ph = (Elf32_Phdr *)(file + eh->e_phoff); uintptr_t lo = UINTPTR_MAX, hi = 0; long pg = sysconf(_SC_PAGESIZE);
  for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) { if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr; if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz; }
  lo &= ~(uintptr_t)(pg - 1); hi = (hi + pg - 1) & ~(uintptr_t)(pg - 1);
  unsigned char *base = mmap(NULL, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) { free(file); snprintf(lastErr, sizeof lastErr, "%s: no memory", path); return NULL; }
  Mod *m = calloc(1, sizeof *m); snprintf(m->path, sizeof m->path, "%s", path); m->base = base; m->span = hi - lo; m->bias = (uintptr_t)base - lo;
  Elf32_Dyn *dyn = NULL;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD) { memcpy((void *)(m->bias + ph[i].p_vaddr), file + ph[i].p_offset, ph[i].p_filesz); }
    else if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf32_Dyn *)(m->bias + ph[i].p_vaddr);
    else if (ph[i].p_type == PT_ARM_EXIDX) { m->exidx = m->bias + ph[i].p_vaddr; m->exidxN = ph[i].p_memsz / 8; }
    else if (ph[i].p_type == PT_TLS) { m->tlsSz = ph[i].p_memsz; m->tlsInitSz = ph[i].p_filesz; m->tlsAlign = ph[i].p_align; }
  }
  m->phnum = eh->e_phnum; m->phdr = malloc(sizeof(Elf32_Phdr) * m->phnum); memcpy(m->phdr, ph, sizeof(Elf32_Phdr) * m->phnum);
  if (m->tlsSz) { for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_TLS) { unsigned char *t = malloc(m->tlsInitSz ? m->tlsInitSz : 1); memcpy(t, file + ph[i].p_offset, m->tlsInitSz); m->tlsInit = t; }
    pthread_key_create(&m->tlsKey, free); m->tlsId = nextTlsId++; }
  if (!dyn) { snprintf(lastErr, sizeof lastErr, "%s: no dynamic section", path); goto fail; }
  /* the dynamic table */
  uintptr_t rel = 0, relsz = 0, jmprel = 0, pltsz = 0, init = 0, initArr = 0, initArrSz = 0, hash = 0, gnuHash = 0; const char *strtab = NULL; Elf32_Sym *symtab = NULL;
  for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) switch (d->d_tag) {
    case DT_STRTAB: strtab = (const char *)(m->bias + d->d_un.d_ptr); break; case DT_SYMTAB: symtab = (Elf32_Sym *)(m->bias + d->d_un.d_ptr); break;
    case DT_REL: rel = m->bias + d->d_un.d_ptr; break; case DT_RELSZ: relsz = d->d_un.d_val; break;
    case DT_JMPREL: jmprel = m->bias + d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
    case DT_INIT: init = m->bias + d->d_un.d_ptr; break; case DT_INIT_ARRAY: initArr = m->bias + d->d_un.d_ptr; break; case DT_INIT_ARRAYSZ: initArrSz = d->d_un.d_val; break;
    case DT_HASH: hash = m->bias + d->d_un.d_ptr; break; case DT_GNU_HASH: gnuHash = m->bias + d->d_un.d_ptr; break; default: break; }
  m->strtab = strtab; m->symtab = symtab;
  /* how many symbols: DT_HASH says it; else the GNU hash's last chain */
  if (hash) m->nsyms = ((uint32_t *)hash)[1];
  else if (gnuHash) { uint32_t *gh = (uint32_t *)gnuHash, nb = gh[0], so = gh[1], bw = gh[2]; uint32_t *bk = gh + 4 + bw, *ch = bk + nb; uint32_t mx = 0;
    for (uint32_t i = 0; i < nb; i++) if (bk[i] > mx) mx = bk[i];
    if (mx >= so) { while (!(ch[mx - so] & 1)) mx++; mx++; } m->nsyms = mx; }
  /* its own definitions, for lookups */
  m->tab = malloc(sizeof(Sym) * (m->nsyms ? m->nsyms : 1));
  for (unsigned i = 1; i < m->nsyms; i++) { Elf32_Sym *s = &symtab[i]; int bind = ELF32_ST_BIND(s->st_info), type = ELF32_ST_TYPE(s->st_info);
    if (s->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK) || type == STT_TLS) continue;
    m->tab[m->ntab].name = strtab + s->st_name; m->tab[m->ntab].val = m->bias + s->st_value; m->ntab++; }
  qsort(m->tab, m->ntab, sizeof *m->tab, sym_cmp);
  /* the libraries it names, by the system's loader (the TV's own: libc, libm, libz...) */
  for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) if (d->d_tag == DT_NEEDED) { const char *nn = strtab + d->d_un.d_val; if (!sys_dlopen(nn, RTLD_NOW | RTLD_GLOBAL)) fprintf(stderr, "[elf] %s needs %s: %s\n", path, nn, sys_dlerror()); }
  /* relocations */
  int missing = 0;
  for (int pass = 0; pass < 2; pass++) { uintptr_t r = pass ? jmprel : rel, sz = pass ? pltsz : relsz; if (!r) continue;
    for (Elf32_Rel *e = (Elf32_Rel *)r; (uintptr_t)e < r + sz; e++) {
      unsigned type = ELF32_R_TYPE(e->r_info), si = ELF32_R_SYM(e->r_info); uint32_t *where = (uint32_t *)(m->bias + e->r_offset);
      Elf32_Sym *s = si ? &symtab[si] : NULL; const char *n = s ? strtab + s->st_name : "";
      switch (type) {
        case R_ARM_NONE: break;
        case R_ARM_RELATIVE: *where += m->bias; break;
        case R_ARM_ABS32: { uintptr_t v = s && s->st_shndx != SHN_UNDEF && ELF32_ST_BIND(s->st_info) == STB_LOCAL ? m->bias + s->st_value : resolve_sym(m, n, s && ELF32_ST_BIND(s->st_info) == STB_WEAK, &missing); *where += v; break; }
        case R_ARM_GLOB_DAT: case R_ARM_JUMP_SLOT: *where = resolve_sym(m, n, s && ELF32_ST_BIND(s->st_info) == STB_WEAK, &missing); break;
        case R_ARM_TLS_DTPMOD32: *where = (uint32_t)(s && s->st_shndx == SHN_UNDEF && si ? 0 : m->tlsId); break;
        case R_ARM_TLS_DTPOFF32: *where += s ? s->st_value : 0; break;
        default: snprintf(lastErr, sizeof lastErr, "%s: relocation type %u not supported (%s)", path, type, n); goto fail; }
    } }
  if (missing) goto fail;
  /* code made executable (and the instruction cache told), the rest left writable */
  /* page by page: a page two segments share gets both their rights (else the second's would take the code's away) */
  { size_t np = m->span / (size_t)pg; unsigned char *pp = calloc(np ? np : 1, 1);
    if (!pp) { snprintf(lastErr, sizeof lastErr, "%s: no memory", path); goto fail; }
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) {
      uintptr_t a = (m->bias + ph[i].p_vaddr) & ~(uintptr_t)(pg - 1), e = (m->bias + ph[i].p_vaddr + ph[i].p_memsz + pg - 1) & ~(uintptr_t)(pg - 1);
      int prot = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0) | (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
      for (uintptr_t q = a; q < e; q += (uintptr_t)pg) pp[(q - (uintptr_t)base) / (size_t)pg] |= (unsigned char)prot; }
    for (size_t i = 0; i < np; ) { size_t j = i; while (j < np && pp[j] == pp[i]) j++;
      unsigned char *a = base + i * (size_t)pg; size_t len = (j - i) * (size_t)pg;
      if (pp[i] & PROT_EXEC) __builtin___clear_cache((char *)a, (char *)a + len);
      if (mprotect(a, len, pp[i] ? pp[i] : PROT_NONE)) { free(pp); snprintf(lastErr, sizeof lastErr, "%s: the TV refused to make its code executable", path); goto fail; }
      i = j; }
    free(pp); }
  free(file); file = NULL;
  pthread_mutex_lock(&mu); m->next = mods; mods = m; pthread_mutex_unlock(&mu); /* (known before its initialisers run: they may throw and catch) */
  if (init) ((void (*)(void))init)();
  for (uintptr_t i = 0; initArr && i < initArrSz / 4; i++) { void (*fn)(void) = ((void (**)(void))initArr)[i]; if ((uintptr_t)fn > 1 && fn != (void (*)(void))-1) fn(); }
  fprintf(stderr, "[elf] %s laid out by the app (%zu KB, %u symbols%s)\n", path, m->span / 1024, m->ntab, m->tlsSz ? ", thread-local data" : "");
  return m;
fail:
  free(file); munmap(base, hi - lo); free(m->tab); free(m->phdr); free(m); return NULL;
}

static void setup(void) {
  static int done; if (done) return; done = 1;
  real_tls_get_addr = (void *(*)(void *))sys_dlsym(RTLD_DEFAULT, "__tls_get_addr");
  real_iter = (int (*)(int (*)(struct dl_phdr_info *, size_t, void *), void *))sys_dlsym(RTLD_DEFAULT, "dl_iterate_phdr");
  real_find_exidx = (uintptr_t (*)(uintptr_t, int *))sys_dlsym(RTLD_DEFAULT, "__gnu_Unwind_Find_exidx");
}
static int ours(void *h) { for (Mod *m = mods; m; m = m->next) if (m == h) return 1; return 0; }
static void *my_dlopen(const char *p, int fl) { return tpk_dlopen(p, fl); }
static void *my_dlsym(void *h, const char *n) { return tpk_dlsym(h, n); }
static char *my_dlerror(void) { return tpk_dlerror(); }

void *tpk_dlopen(const char *path, int flags) {
  void *h = sys_dlopen(path, flags); if (h || !path || !strchr(path, '/')) { if (!h) snprintf(lastErr, sizeof lastErr, "%s", sys_dlerror()); return h; } /* (a system library by name: the system's loader alone) */
  char why[300]; snprintf(why, sizeof why, "%s", sys_dlerror());
  pthread_mutex_lock(&mu); for (Mod *m = mods; m; m = m->next) if (!strcmp(m->path, path)) { pthread_mutex_unlock(&mu); return m; } pthread_mutex_unlock(&mu);
  setup(); fprintf(stderr, "[elf] the system would not load %s (%s): laid out by the app\n", path, why);
  return load(path);
}
void *tpk_dlsym(void *h, const char *name) {
  if (h && ours(h)) { uintptr_t v = own((Mod *)h, name); if (!v) snprintf(lastErr, sizeof lastErr, "undefined symbol %s", name); return (void *)v; }
  return sys_dlsym(h, name);
}
char *tpk_dlerror(void) { static char e[400]; if (!lastErr[0]) return sys_dlerror(); snprintf(e, sizeof e, "%s", lastErr); lastErr[0] = 0; return e; }
#endif /* SB_TPK */
