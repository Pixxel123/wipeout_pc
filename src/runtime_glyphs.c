/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Live controller glyphs and picture (WipEout, the Controls screens' Controller type rows):
 * GOW3_GLYPH_SWAP=<file> names the button font's atlas and tab textures and the Controls screen's
 * controller sheet, with a file of each style's version (scripts/wipeout_glyphs.py and
 * scripts/xbox_controls.py --swap-config). A thread reads four times a second the game's settings
 * "Pad Style P<n>" and the value of an open Controller type row (src/glyph_swap.h): an open row's
 * value is shown as it changes, else the saved P1. When the wanted style changes it finds each
 * texture's copies in guest memory by their bytes (runtime_memory_find_all) and writes the wanted
 * style's texels over each through the backing view, the range invalidated for the GPU
 * (runtime_memory_write_guest). Smooth play first: nothing is searched or written unless the
 * wanted style changed, a row has just opened (a menu screen) or the game has just read a texture
 * from its archive (then it may have moved), and a copy is written only where its whole texels
 * were just checked to be the version thought to be there. The settings store and the table of
 * open rows are taken at their 01.07 places (CUSA07671) when those hold them, else found by what
 * they hold (find_store, find_table, the Accept hook). Without the Accept hook (probe.c found no
 * site) a row's value is written into its setting as it changes. */
#define _GNU_SOURCE
#include "runtime.h"
#include "glyph_swap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* Copies: the drawn one and stale ones in freed load buffers, which hold the same bytes; every one
 * is written, so a few stale ones cannot keep the drawn one from being found. */
enum { GLYPH_MAX_TEXTURES=12, GLYPH_MAX_COPIES=16 };
/* 01.07's offsets of the settings store's pointer and of the table of rows on open screens. */
#define GLYPH_STORE_AT 0x10ea398
#define GLYPH_TABLE_AT 0x11ac4c8

typedef struct {
    GlyphTexture spec;
    unsigned char *data[GLYPH_STYLES]; /* whole files; texels from spec.skip */
    size_t size[GLYPH_STYLES], sig_at[GLYPH_STYLES];
    unsigned char *at[GLYPH_MAX_COPIES]; /* the copies of the texels in guest memory */
    int at_style[GLYPH_MAX_COPIES];      /* the style each copy holds */
    int copies;          /* how many places are known; 0 none */
    int shown;           /* the style all copies hold (as loaded while none is known), or -1 */
    int failed_for;      /* the wanted style a search last failed for, or -2 */
    uint64_t read_at;    /* ns of the last read of its archive range; 0 when handled */
    int reloaded;        /* read again since the last search: only the start style is looked for */
    uint64_t retry_at;   /* ns of the one more search after a failed one following a read; 0 none */
} Texture;
static Texture textures[GLYPH_MAX_TEXTURES];
static int texture_count;
static int launch_style; /* GOW3_GLYPH_STYLE: the style the game's file patches load (run.py) */

static uint64_t glyph_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec; }
/* As toggle_watcher in src/runtime_memory.c (no sleep() on Windows). */
static void glyph_pause_ns(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec pause={(time_t)(ns/1000000000ull),(long)(ns%1000000000ull)};
    nanosleep(&pause,NULL);
#endif
}
/* The 64-byte window with the most distinct bytes, among those not starting with 0x00 or 0xFF
 * (the search keys on the first byte; zero-filled memory would match it everywhere). */
static size_t signature_at(const unsigned char *texels, size_t n) {
    size_t at=0, best=0, any_at=0, any_best=0;
    for (size_t o=0; o+64<=n; o+=4096) {
        unsigned char seen[256]={0};
        size_t distinct=0;
        for (size_t i=0;i<64;++i) distinct+=!seen[texels[o+i]]++;
        if (distinct>any_best) { any_best=distinct; any_at=o; }
        if (texels[o]!=0x00 && texels[o]!=0xff && distinct>best) { best=distinct; at=o; }
    }
    return best ? at : any_at;
}
static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    unsigned char *data=NULL;
    if (!fseek(f,0,SEEK_END)) {
        const long n=ftell(f);
        if (n>0 && !fseek(f,0,SEEK_SET) && (data=malloc((size_t)n)) && fread(data,1,(size_t)n,f)!=(size_t)n) { free(data); data=NULL; }
        if (data) *size=(size_t)n;
    }
    fclose(f);
    return data;
}
static void load_config(const char *path) {
    FILE *f=fopen(path,"r");
    if (!f) return;
    char line[2200];
    while (texture_count<GLYPH_MAX_TEXTURES && fgets(line,sizeof(line),f)) {
        Texture *t=&textures[texture_count];
        if (!glyph_parse_line(line,&t->spec)) {
            const int n=(int)strcspn(line,"\r\n");
            if (n) printf("Runtime: glyphs: config line skipped: %.*s\n",n<80 ? n : 80,line);
            continue;
        }
        int styles=0;
        for (int s=0;s<GLYPH_STYLES;++s) {
            if (!t->spec.files[s][0]) continue;
            size_t size=0;
            unsigned char *data=read_file(t->spec.files[s],&size);
            if (!data || size<=t->spec.skip+4096) { free(data); continue; }
            t->data[s]=data; t->size[s]=size-t->spec.skip;
            t->sig_at[s]=signature_at(data+t->spec.skip,t->size[s]);
            ++styles;
        }
        if (styles<2 || !t->data[launch_style]) {
            if (styles>=2) printf("Runtime: glyphs: %s @0x%llx has no %s version, left alone\n",t->spec.guest,
                                  (unsigned long long)t->spec.offset,glyph_style_names[launch_style]);
            for (int s=0;s<GLYPH_STYLES;++s) free(t->data[s]);
            memset(t,0,sizeof(*t));
            continue;
        }
        t->shown=launch_style; t->failed_for=-2; /* as loaded: where is found only when needed */
        ++texture_count;
    }
    fclose(f);
}
/* The settings and rows followed: player n's setting "Pad Style P<n>" and its row, whose key is
 * the hash of "FE_" + the setting (glyph_swap.h). keys are their hashes, set before the thread. */
enum { SETTING_P1, SETTING_P2, ROW_P1, ROW_P2, KEYS };
static const char *const key_names[KEYS]={"Pad Style P1","Pad Style P2","FE_Pad Style P1","FE_Pad Style P2"};
static uint32_t keys[KEYS];
/* Where the game keeps them, shared by the switcher thread and the Accept hook, which find them:
 * the image offset of the settings store's pointer (-1 while not known); the table of rows on open
 * screens, table_ref its address or with table_indirect the address of the word holding it (0
 * while not known), table_counted when its count column says which rows are open (else a row is
 * open while its value names a style). table_lock keeps the two finders' searches apart. */
static int64_t store_at=-1;
static uintptr_t table_ref;
static int table_indirect, table_counted;
static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static int accept_hooked; /* probe.c installed the Accept hook (runtime_glyphs_accept_hooked) */
static int accepted;      /* the Accept hook committed a Controller type */

/* An expression under fault recovery, the caller's own recovery point kept: ok is 0 when a fault
 * stopped it. Each use sits in a function of its own (noinline), so nothing it changes is held
 * in a register across the jump. */
#define GLYPH_GUARDED(ok, expression) do { \
        sigjmp_buf recover_; sigjmp_buf *const outer_=runtime_fault_recover; (ok)=0; \
        if (!sigsetjmp(recover_,0)) { runtime_fault_recover=&recover_; (ok)=(expression); } \
        runtime_fault_recover=outer_; } while (0)

static __attribute__((noinline)) int guarded_copy(void *out, const void *from, size_t n) {
    volatile int ok;
    GLYPH_GUARDED(ok,(memcpy(out,from,n),1));
    return ok;
}
/* The 8 bytes at `at`, 0 when they cannot be read. */
static uint64_t word_at(const void *at) {
    uint64_t word;
    return guarded_copy(&word,at,8) ? word : 0;
}
static int plausible(uint64_t address) { return address>=0x10000 && !(address & 7) && address<(1ull<<40); }
static uint64_t distance(int64_t a, int64_t b) { return a>b ? (uint64_t)(a-b) : (uint64_t)(b-a); }
static double ms_since(uint64_t start) { return (double)(glyph_now()-start)/1e6; }

/* The image's 8-aligned words, a page at a time under fault recovery (gaps between segments are
 * skipped): visit(offset, word, ctx) until it returns nonzero. */
typedef int (*ImageVisit)(uint64_t offset, uint64_t word, void *ctx);
static int visit_page(const unsigned char *image, uint64_t offset, ImageVisit visit, void *ctx) {
    for (uint64_t w=0;w<4096;w+=8) if (visit(offset+w,glyph_load64(image+offset+w),ctx)) return 2;
    return 1;
}
static __attribute__((noinline)) int guarded_page(const unsigned char *image, uint64_t offset, ImageVisit visit, void *ctx) {
    volatile int result;
    GLYPH_GUARDED(result,visit_page(image,offset,visit,ctx));
    return result;
}
static void image_words(const unsigned char *image, uint64_t size, ImageVisit visit, void *ctx) {
    for (uint64_t offset=0; offset+4096<=size; offset+=4096)
        if (guarded_page(image,offset,visit,ctx)==2) return;
}

/* Whether a search is due: not before *next, and each time the wait doubles, up to 30 s. */
static int due(uint64_t now, uint64_t *next, uint64_t *wait) {
    if (now<*next) return 0;
    *next=now+*wait;
    if (*wait<30000000000ull) *wait*=2;
    return 1;
}

/* ---- the settings store ---- */
static __attribute__((noinline)) int store_ok(uint64_t store) {
    volatile int ok;
    if (!plausible(store)) return 0;
    GLYPH_GUARDED(ok,glyph_store_holds((const unsigned char *)(uintptr_t)store,keys[SETTING_P1],2048));
    return ok;
}
typedef struct { const unsigned char *image; uint64_t size; int64_t best; int found; } StoreSearch;
/* A word pointing at a store (in guest memory or the image's own data, which is no guest mapping)
 * whose list holds "Pad Style P1" with a known style; of several, the nearest to 01.07's place. */
static int store_word(uint64_t offset, uint64_t word, void *ctx) {
    StoreSearch *s=ctx;
    if (!plausible(word)) return 0;
    const uint64_t in_image=word-(uintptr_t)s->image;
    if (!(in_image<s->size && s->size-in_image>=GLYPH_STORE_HEAD+8) && !runtime_memory_is_mapped((uintptr_t)word,GLYPH_STORE_HEAD+8))
        return 0;
    if (!store_ok(word)) return 0;
    ++s->found;
    if (s->best<0 || distance((int64_t)offset,GLYPH_STORE_AT)<distance(s->best,GLYPH_STORE_AT)) s->best=(int64_t)offset;
    return 0;
}
/* The image offset of the store's pointer: 01.07's place, else found by searching the image, with
 * a growing wait between searches. -1 when none: until the profile is loaded the store lacks the
 * setting, so while 01.07's place points at a mapped store nothing is searched. */
static int64_t find_store(const unsigned char *image, uint64_t size, uint64_t now) {
    static uint64_t next, wait=5000000000ull;
    const uint64_t place=size>GLYPH_STORE_AT+8 ? word_at(image+GLYPH_STORE_AT) : 0;
    if (store_ok(place)) {
        printf("Runtime: glyphs: settings store at image+0x%llx (01.07's place)\n",(unsigned long long)GLYPH_STORE_AT);
        return GLYPH_STORE_AT;
    }
    if ((plausible(place) && runtime_memory_is_mapped((uintptr_t)place,GLYPH_STORE_HEAD+8)) || !due(now,&next,&wait)) return -1;
    StoreSearch s={image,size,-1,0};
    const uint64_t start=glyph_now();
    image_words(image,size,store_word,&s);
    if (s.best>=0) printf("Runtime: glyphs: settings store at image+0x%llx (%d candidates, %.0f ms)\n",
                          (unsigned long long)s.best,s.found,ms_since(start));
    return s.best;
}
static __attribute__((noinline)) int read_saved(const unsigned char *store, int saved[2]) {
    volatile int ok;
    char value[2][64];
    GLYPH_GUARDED(ok,(saved[0]=glyph_style_index(glyph_setting(store,keys[SETTING_P1],value[0],64)),
                      saved[1]=glyph_style_index(glyph_setting(store,keys[SETTING_P2],value[1],64)),1));
    return ok;
}
/* Writes style into player p's setting, as the game writes one: 1 when written. */
static __attribute__((noinline)) int commit_row(const unsigned char *image, int p, int style) {
    const int64_t at=__atomic_load_n(&store_at,__ATOMIC_ACQUIRE);
    unsigned char *const store=at<0 || style<0 ? NULL : (unsigned char *)(uintptr_t)word_at(image+at);
    volatile int ok;
    if (!store) return 0;
    GLYPH_GUARDED(ok,glyph_commit(store,keys[SETTING_P1+p],glyph_style_names[style]));
    return ok;
}

/* ---- an open row's value ---- */
/* Up to 24 characters of the text at `from` into `to` (unprintable ones as '?'); 1 when read. */
static int copy_text(const char *from, char to[25]) {
    for (int i=0; i<24 && from[i]; ++i) to[i]=from[i]>=32 && from[i]<127 ? from[i] : '?', to[i+1]=0;
    return 1;
}
static __attribute__((noinline)) int guarded_text(const char *from, char to[25]) {
    volatile int ok;
    to[0]=0;
    GLYPH_GUARDED(ok,copy_text(from,to));
    return ok;
}
/* What a row's value pointer holds, for the log: the text it points at and the text at it. */
static void describe_field(const unsigned char *field, char *out, size_t cap) {
    char pointed[25]="", inline_text[25]="";
    if (guarded_text((const char *)field,inline_text)) {
        const uint64_t string=word_at(field);
        if (string && !(string>>47)) guarded_text((const char *)(uintptr_t)string,pointed);
    }
    snprintf(out,cap,"-> \"%s\", at it \"%s\"",pointed,inline_text);
}
/* The style a row's value names: the string it points at, else the text at it, each read under
 * fault recovery of its own (the first 8 bytes at the pointer may be text, not an address). */
static __attribute__((noinline)) int field_style(const unsigned char *field) {
    volatile int pointed, at;
    if (!field) return -1;
    GLYPH_GUARDED(pointed,glyph_pointed_style(field)+2);
    if (pointed>=2) return pointed-2;
    GLYPH_GUARDED(at,glyph_value_style((const char *)field)+2);
    return at>=2 ? at-2 : -1;
}

/* ---- the table of rows on open screens ----
 * 256 value pointers, their keys at +0x800 and counts at +0x1000 (glyph_swap.h). Where it is and
 * whether its counts mean anything are found by trying: 01.07's place, that place as a pointer to
 * it, and the rows' keys in the image or the guest's smaller regions. A slot is a Controller type
 * row when it has a row's key and a value pointer, and a count or, where no count is seen, a value
 * that names a style. */
static __attribute__((noinline)) int counted_slot(const unsigned char *table, int i, uint32_t key) {
    volatile int ok;
    GLYPH_GUARDED(ok,glyph_slot(table,i,key,1));
    return ok;
}
static int named_slot(const unsigned char *table, int i, uint32_t key) {
    const uint64_t field=(uint32_t)word_at(table+0x800+8*i)==key ? word_at(table+8*i) : 0;
    return field && !(field & 7) && field_style((const unsigned char *)(uintptr_t)field)>=0;
}
/* 2 when slot i (every slot, i<0) of `table` holds a Controller type row by its count, 1 by its
 * value only, 0 when none. */
static int table_quality(const unsigned char *table, int i) {
    int quality=0;
    for (int k=ROW_P1;k<=ROW_P2;++k)
        for (int at=i<0 ? 0 : i; at<(i<0 ? GLYPH_ROWS : i+1) && quality<2; ++at) {
            if (counted_slot(table,at,keys[k])) quality=2;
            else if (!quality && named_slot(table,at,keys[k])) quality=1;
        }
    return quality;
}
/* The table now: table_ref, or the address the word at table_ref holds; NULL when not known. */
static const unsigned char *table_now(void) {
    const uintptr_t ref=__atomic_load_n(&table_ref,__ATOMIC_ACQUIRE);
    if (!ref || !__atomic_load_n(&table_indirect,__ATOMIC_ACQUIRE)) return (const unsigned char *)ref;
    const uint64_t table=word_at((const void *)ref);
    return plausible(table) ? (const unsigned char *)(uintptr_t)table : NULL;
}
static int use_table(uintptr_t ref, int indirect, int quality, const unsigned char *image, const char *how) {
    __atomic_store_n(&table_counted,quality==2,__ATOMIC_RELEASE);
    __atomic_store_n(&table_indirect,indirect,__ATOMIC_RELEASE);
    __atomic_store_n(&table_ref,ref,__ATOMIC_RELEASE);
    const uint64_t offset=ref-(uintptr_t)image;
    if (offset<runtime_image_size()) printf("Runtime: glyphs: open rows table %s image+0x%llx (%s)\n",
                                            indirect ? "through the pointer at" : "at",(unsigned long long)offset,how);
    else printf("Runtime: glyphs: open rows table at 0x%llx (%s)\n",(unsigned long long)ref,how);
    printf("Runtime: glyphs: rows are open by %s\n",quality==2 ? "their count" : "their value (no count seen)");
    return 1;
}
/* 01.07's place, or that place as a pointer to the table: cheap, so tried every tick. */
static int table_at_place(const unsigned char *image, uint64_t size) {
    const unsigned char *place=image+GLYPH_TABLE_AT;
    int quality;
    if (size<=GLYPH_TABLE_AT+0x1000+4*GLYPH_ROWS) return 0;
    if ((quality=table_quality(place,-1))) return use_table((uintptr_t)place,0,quality,image,"01.07's place");
    const uint64_t word=word_at(place);
    if (plausible(word) && (quality=table_quality((const unsigned char *)(uintptr_t)word,-1)))
        return use_table((uintptr_t)place,1,quality,image,"01.07's place holds its address");
    return 0;
}
/* The best table (quality, *best) whose key column has a row's key at key_at, a counted one first. */
static int table_around(uintptr_t key_at, uintptr_t lowest, uintptr_t *best, int quality) {
    for (int i=0;i<GLYPH_ROWS && quality<2;++i) {
        if (key_at<lowest+0x800+8*(uintptr_t)i) break;
        const uintptr_t base=key_at-0x800-8*(uintptr_t)i;
        const int q=table_quality((const unsigned char *)base,i);
        if (q>quality) { quality=q; *best=base; }
    }
    return quality;
}
typedef struct { const unsigned char *image; uintptr_t best; int quality; } TableSearch;
static int table_word(uint64_t offset, uint64_t word, void *ctx) {
    TableSearch *s=ctx;
    if ((uint32_t)word==keys[ROW_P1] || (uint32_t)word==keys[ROW_P2])
        s->quality=table_around((uintptr_t)s->image+offset,(uintptr_t)s->image,&s->best,s->quality);
    return s->quality==2;
}
/* The thread's search, while no table is known: the image's words holding a row's key, with a
 * growing wait between searches (the rows are there only while a Controller type row is open, and
 * the Accept hook searches deeper). */
static void find_table(const unsigned char *image, uint64_t size, uint64_t now) {
    static uint64_t next, wait=1000000000ull;
    if (table_at_place(image,size) || !due(now,&next,&wait) || pthread_mutex_trylock(&table_lock)) return;
    TableSearch s={image,0,0};
    image_words(image,size,table_word,&s);
    if (s.quality) use_table(s.best,0,s.quality,image,"found by its rows' keys in the image");
    pthread_mutex_unlock(&table_lock);
}
/* Where the settings' and rows' keys are, in the image and the guest's regions of at most 1 GiB
 * (the heap; the texture pool is larger), for the Accept hook's search (8-aligned: a table's keys
 * are) and its log (4-aligned: everything). */
enum { KEY_HITS=64 };
typedef struct { const unsigned char *image; uintptr_t at[KEY_HITS]; int which[KEY_HITS], count, in_image, align; } KeyHits;
static int key_word(uint64_t offset, uint64_t word, void *ctx) {
    KeyHits *h=ctx;
    for (int half=0;half<(h->align==8 ? 1 : 2) && h->count<KEY_HITS;++half)
        for (int k=0;k<KEYS;++k)
            if ((uint32_t)(word>>32*half)==keys[k]) { h->at[h->count]=(uintptr_t)h->image+offset+4*half; h->which[h->count++]=k; break; }
    return h->count==KEY_HITS;
}
static void find_keys(const unsigned char *image, uint64_t size, int align, KeyHits *h) {
    h->image=image;
    h->count=0;
    h->align=align;
    image_words(image,size,key_word,h);
    h->in_image=h->count;
    h->count+=runtime_memory_find_u32(keys,KEYS,align,1ull<<30,h->at+h->count,h->which+h->count,KEY_HITS-h->count);
}
typedef struct { uint64_t target, offset; int found; } WordSearch;
static int word_equal(uint64_t offset, uint64_t word, void *ctx) {
    WordSearch *s=ctx;
    if (word==s->target) { s->offset=offset; s->found=1; }
    return s->found;
}
/* The Accept hook's search (the Controls screen is surely open): the rows' keys found by
 * find_keys. A table in guest memory is reached through a word of the image holding its address
 * when there is one (that survives the screen being made again). */
static int table_from_keys(const unsigned char *image, uint64_t size, const KeyHits *h) {
    uintptr_t best=0;
    int quality=0;
    for (int i=0;i<h->count && quality<2;++i)
        if (h->which[i]>=ROW_P1)
            quality=table_around(h->at[i],i<h->in_image ? (uintptr_t)image : 0,&best,quality);
    if (!quality) return 0;
    if (best-(uintptr_t)image<size) return use_table(best,0,quality,image,"found by its rows' keys in the image");
    WordSearch s={best,0,0};
    image_words(image,size,word_equal,&s);
    printf("Runtime: glyphs: (the table is at 0x%llx in guest memory)\n",(unsigned long long)best);
    return s.found ? use_table((uintptr_t)image+s.offset,1,quality,image,"its address in the image")
                   : use_table(best,0,quality,image,"found in guest memory");
}
/* Once, when Accept finds no table: where the keys are, with the memory around them, so a log
 * shows how this build keeps its open rows. */
static void dump_keys(const unsigned char *image, uint64_t size, const KeyHits *h) {
    static int dumped;
    if (dumped++) return;
    printf("Runtime: glyphs: dump: at 01.07's table place %016llx %016llx, keys %016llx %016llx; %d key hits (%d in the image)\n",
           (unsigned long long)word_at(image+GLYPH_TABLE_AT),(unsigned long long)word_at(image+GLYPH_TABLE_AT+8),
           (unsigned long long)word_at(image+GLYPH_TABLE_AT+0x800),(unsigned long long)word_at(image+GLYPH_TABLE_AT+0x808),
           h->count,h->in_image);
    for (int i=0;i<h->count;++i) {
        char line[600];
        const uint64_t offset=h->at[i]-(uintptr_t)image;
        int n=snprintf(line,sizeof(line),"Runtime: glyphs: dump: %s at %s0x%llx:",key_names[h->which[i]],
                       offset<size ? "image+" : "",(unsigned long long)(offset<size ? offset : h->at[i]));
        /* The 12 words around the key's, the key's marked with '*'. */
        const uintptr_t key_word_at=h->at[i] & ~(uintptr_t)7;
        for (uintptr_t at=key_word_at-0x20; at<key_word_at+0x40 && n<(int)sizeof(line)-24; at+=8) {
            uint64_t v;
            n+=guarded_copy(&v,(const void *)at,8)
                ? snprintf(line+n,sizeof(line)-(size_t)n," %s%llx",at==key_word_at ? "*" : "",(unsigned long long)v)
                : snprintf(line+n,sizeof(line)-(size_t)n," ?");
        }
        puts(line);
    }
}

/* ---- each tick ---- */
/* counted: the table's counts say which rows are open (table_counted, read once a tick). */
typedef struct { int saved[2], live[2], open, counted; const unsigned char *field[2]; } State;
static int row_fields(const unsigned char *table, State *s) {
    for (int p=0;p<2;++p) s->field[p]=glyph_row_field(table,keys[ROW_P1+p],s->counted);
    s->open=s->counted && (glyph_row_open(table,keys[ROW_P1]) || glyph_row_open(table,keys[ROW_P2]));
    return 1;
}
static __attribute__((noinline)) int read_rows(const unsigned char *table, State *s) {
    volatile int ok;
    GLYPH_GUARDED(ok,row_fields(table,s));
    return ok;
}
/* The saved settings and the rows' values (-1 for what is not there or not known yet); open:
 * whether a Controller type row is open, by its count, else while its value names a style. 0 when
 * a fault stopped the reads. */
static int read_state(const unsigned char *image, State *s) {
    const int64_t at=__atomic_load_n(&store_at,__ATOMIC_ACQUIRE);
    const unsigned char *const store=at<0 ? NULL : (const unsigned char *)(uintptr_t)word_at(image+at);
    const unsigned char *const table=table_now();
    *s=(State){{-1,-1},{-1,-1},0,__atomic_load_n(&table_counted,__ATOMIC_ACQUIRE),{NULL,NULL}};
    if ((store && !read_saved(store,s->saved)) || (table && !read_rows(table,s))) return 0;
    for (int p=0;p<2;++p) s->live[p]=field_style(s->field[p]);
    if (!s->counted) s->open=s->live[0]>=0 || s->live[1]>=0;
    return 1;
}

/* ---- the textures ---- */
/* Whether copy i still holds the whole texels of its style. */
static __attribute__((noinline)) int still_there(const Texture *t, int i) {
    const int style=t->at_style[i];
    volatile int ok;
    if (!t->at[i] || style<0 || !t->data[style]) return 0;
    GLYPH_GUARDED(ok,glyph_verify(t->at[i],t->data[style]+t->spec.skip,t->size[style]));
    return ok;
}
/* Whether copies are known and every one is still as it was left. */
static int all_there(const Texture *t) {
    for (int i=0;i<t->copies;++i) if (!still_there(t,i)) return 0;
    return t->copies>0;
}
/* Writes style `want` over copy i once its texels are checked to be its style's; 1 when done. The
 * check reads under fault recovery; the write takes the memory lock, so it comes after. */
static int write_style(Texture *t, int i, int want) {
    const size_t n=t->size[want];
    if (!still_there(t,i) || t->size[t->at_style[i]]!=n) return 0;
    return runtime_memory_write_guest((uintptr_t)t->at[i],t->data[want]+t->spec.skip,n);
}
static void locate(Texture *t) {
    const unsigned char *blocks[GLYPH_STYLES];
    /* Just read again, the texture holds the start style: a copy of another style written earlier
     * may still be in memory and must not be taken for it. */
    for (int s=0;s<GLYPH_STYLES;++s)
        blocks[s]=t->data[s] && (!t->reloaded || s==launch_style) ? t->data[s]+t->spec.skip : NULL;
    t->reloaded=0;
    uintptr_t found[GLYPH_MAX_COPIES];
    const uint64_t start=glyph_now();
    t->copies=runtime_memory_find_all(blocks,t->size,t->sig_at,GLYPH_STYLES,found,t->at_style,GLYPH_MAX_COPIES);
    printf("Runtime: glyphs: search %s @0x%llx: %d copies in %.1f ms\n",t->spec.guest,(unsigned long long)t->spec.offset,
           t->copies,ms_since(start));
    for (int i=0;i<t->copies;++i) {
        t->at[i]=(unsigned char *)found[i];
        printf("Runtime: glyphs: %s @0x%llx found as %s at %p (copy %d of %d)\n",t->spec.guest,
               (unsigned long long)t->spec.offset,glyph_style_names[t->at_style[i]],(void *)t->at[i],i+1,t->copies);
    }
    t->shown=glyph_copies_style(t->at_style,t->copies);
}
/* Writes `want` over every copy not holding it, each checked right before its write; a copy that
 * changed under us is dropped. The copies left all hold `want`; returns how many. */
static int write_copies(Texture *t, int want) {
    const int n=t->copies;
    int kept=0;
    for (int i=0;i<n;++i) {
        if (t->at_style[i]!=want && !write_style(t,i,want)) continue;
        t->at[kept]=t->at[i];
        t->at_style[kept++]=want;
    }
    t->copies=kept;
    if (kept) printf("Runtime: glyphs: %s @0x%llx now %s (%d of %d copies)\n",t->spec.guest,
                     (unsigned long long)t->spec.offset,glyph_style_names[want],kept,n);
    return kept;
}
/* One texture's work in a tick: `style` wanted (-1 none), `opened` a Controller type row just
 * opened (a menu screen, never a race: the copies are found now, so the row's first change shows
 * at once). */
static void tick_texture(Texture *t, int style, int opened, uint64_t now) {
    /* The game read the texture again (a load): its file patches gave it the start style, maybe
     * somewhere else. Searched for only if another style is wanted (below). */
    const uint64_t read=__atomic_load_n(&t->read_at,__ATOMIC_ACQUIRE);
    int located=0;
    if (read && now>read && now-read>1500000000ull) {
        __atomic_store_n(&t->read_at,0,__ATOMIC_RELEASE);
        t->copies=0; t->shown=launch_style; t->failed_for=-2;
        t->reloaded=1; t->retry_at=0;
    }
    if (opened && now>=t->retry_at && !all_there(t)) {
        const int reloaded=t->reloaded, shown=t->shown;
        locate(t);
        if (!t->copies) { t->reloaded=reloaded; t->shown=shown; return; } /* left as it was */
        t->failed_for=-2;   /* found: an earlier failed search no longer applies */
        located=1;
    }
    if (style<0 || !t->data[style] || t->shown==style || t->failed_for==style || now<t->retry_at) return;
    if (!located && !all_there(t)) {
        const int reloaded=t->reloaded;
        locate(t);          /* the only search: the wanted style changed, a place is unknown */
        if (!t->copies) {
            /* After a read the game may still be unpacking it: one more try 2 s later. */
            if (reloaded && !t->retry_at) { t->retry_at=now+2000000000ull; t->reloaded=1; }
            else t->failed_for=style;
            return;
        }
        if (t->shown==style) return;
    }
    if (write_copies(t,style)) {
        t->shown=style;
        t->failed_for=-2;
    } else {
        t->shown=-1;        /* all changed under us: searched for again at the next change */
        t->failed_for=style;
    }
}
static const char *style_or_dash(int style) { return style>=0 ? glyph_style_names[style] : "-"; }
static void *glyph_thread(void *unused) {
    (void)unused;
    unsigned char *image;
    while (!(image=runtime_image_base())) glyph_pause_ns(1000000000ull);
    glyph_pause_ns(2000000000ull);
    printf("Runtime: glyph switch on (%d textures, %s at start; Accept %s)\n",texture_count,glyph_style_names[launch_style],
           __atomic_load_n(&accept_hooked,__ATOMIC_ACQUIRE) ? "hooked" : "not hooked: a row's value is kept as it changes");
    int last_want=-2, was_open=0, profile_loaded=0, active=-1, last_saved[2]={-2,-2}, last_live[2]={-1,-1};
    const unsigned char *last_field[2]={NULL,NULL};
    for (;;) {
        glyph_pause_ns(250000000ull);
        const uint64_t now=glyph_now(), size=runtime_image_size();
        if (size && __atomic_load_n(&store_at,__ATOMIC_ACQUIRE)<0) __atomic_store_n(&store_at,find_store(image,size,now),__ATOMIC_RELEASE);
        if (size && !__atomic_load_n(&table_ref,__ATOMIC_ACQUIRE)) find_table(image,size,now);
        State s;
        if (!read_state(image,&s)) continue;
        for (int p=0;p<2;++p) {
            if (s.saved[p]!=last_saved[p] && (last_saved[p]!=-2 || s.saved[p]>=0))
                printf("Runtime: glyphs: saved P%d now %s\n",p+1,style_or_dash(s.saved[p]));
            if (s.field[p] && (s.field[p]!=last_field[p] || s.live[p]!=last_live[p])) {
                char text[80];
                describe_field(s.field[p],text,sizeof(text));
                printf("Runtime: glyphs: P%d row value %s (%s)\n",p+1,text,style_or_dash(s.live[p]));
            }
            if (s.live[p]>=0 && last_live[p]>=0 && s.live[p]!=last_live[p]) active=p;
            last_saved[p]=s.saved[p]; last_field[p]=s.field[p]; last_live[p]=s.live[p];
        }
        /* No Accept hook: the row's value is written into its setting as it changes, so it stays
         * after the screen is left (Cancel keeps it too). */
        if (!__atomic_load_n(&accept_hooked,__ATOMIC_ACQUIRE))
            for (int p=0;p<2;++p)
                if (s.live[p]>=0 && s.live[p]!=s.saved[p] && commit_row(image,p,s.live[p]))
                    printf("Runtime: Controller type P%d kept: %s\n",p+1,glyph_style_names[s.live[p]]);
        /* A table without counts may show both screens' rows at once: only the one changed last
         * counts as open. */
        if (!s.counted) for (int p=0;p<2;++p) if (p!=active) s.live[p]=-1;
        /* Before the profile is loaded the setting holds the row's default: the prompts stay as
         * loaded (run.py chose them from the save) until the setting agrees with them, a row is
         * open or Accept committed one. */
        if (!profile_loaded && (s.saved[0]==launch_style || s.live[0]>=0 || s.live[1]>=0 || __atomic_load_n(&accepted,__ATOMIC_ACQUIRE))) {
            profile_loaded=1;
            printf("Runtime: glyphs: following the Controller type (saved P1 %s)\n",style_or_dash(s.saved[0]));
        }
        const int want=profile_loaded ? glyph_wanted_live(s.live[0],s.live[1],s.saved[0]) : -1;
        if (s.open!=was_open)
            printf("Runtime: glyphs: Controller type row %s (live P1 %s, live P2 %s, saved P1 %s)\n",s.open ? "open" : "closed",
                   style_or_dash(s.live[0]),style_or_dash(s.live[1]),style_or_dash(s.saved[0]));
        if (want!=last_want) {
            printf("Runtime: glyphs: wanted %s (live P1 %s, live P2 %s, saved P1 %s)\n",style_or_dash(want),
                   style_or_dash(s.live[0]),style_or_dash(s.live[1]),style_or_dash(s.saved[0]));
            /* A new choice: a style whose search failed before is looked for again. */
            for (int i=0;i<texture_count;++i) textures[i].failed_for=-2;
        }
        last_want=want;
        for (int i=0;i<texture_count;++i) tick_texture(&textures[i],want,s.open && !was_open,now);
        was_open=s.open;
    }
    return NULL;
}
/* The Controls screen's Accept (probe.c hook in its Update, game thread): its own code copies only
 * the settings it knows into the profile, and a List row writes its setting only when its screen
 * is left with previous="true", which Accept's redirect is not. So the open Controller type row's
 * value is written here into its existing "Pad Style P<n>" setting; nothing is written for a value
 * that is not a known style or does not fit the node. The screen is surely open here, so a table
 * not known yet is searched for in guest memory too (it stalls Accept a moment: three times at
 * most), and when none is found the keys are logged once. */
ABI void runtime_glyphs_accept(void) {
    unsigned char *image=runtime_image_base();
    const uint64_t size=runtime_image_size();
    static int searches;
    if (!image) return;
    const unsigned char *table=table_now();
    if ((!table || !table_quality(table,-1)) && !table_at_place(image,size) && searches<3) {
        static KeyHits hits;    /* static: the hook runs on a guest thread's small stack */
        ++searches;
        pthread_mutex_lock(&table_lock);
        const uint64_t start=glyph_now();
        find_keys(image,size,8,&hits);
        const int found=table_from_keys(image,size,&hits);
        printf("Runtime: glyphs: open rows table %s (%d key hits, %.0f ms)\n",found ? "found" : "not found",hits.count,ms_since(start));
        if (!found) { find_keys(image,size,4,&hits); dump_keys(image,size,&hits); }
        pthread_mutex_unlock(&table_lock);
    }
    State s;
    if (!(table=table_now()) || __atomic_load_n(&store_at,__ATOMIC_ACQUIRE)<0 || !read_state(image,&s)) {
        printf("Runtime: Controller type not kept (the game's %s not found)\n",table ? "settings" : "open rows");
        return;
    }
    for (int p=0;p<2;++p) {
        if (s.live[p]<0) continue;
        if (!commit_row(image,p,s.live[p])) {
            printf("Runtime: Controller type P%d not kept (%s: no setting it fits in)\n",p+1,glyph_style_names[s.live[p]]);
            continue;
        }
        __atomic_store_n(&accepted,1,__ATOMIC_RELEASE);
        printf("Runtime: Controller type P%d accepted: %s\n",p+1,glyph_style_names[s.live[p]]);
    }
}
/* probe.c installed the Accept hook: the rows' values are written on Accept only. */
void runtime_glyphs_accept_hooked(void) { __atomic_store_n(&accept_hooked,1,__ATOMIC_RELEASE); }
static int glyphs_watches(const char *guest) {
    for (int i=0;i<texture_count;++i) if (!strcasecmp(textures[i].spec.guest,guest)) return 1;
    return 0;
}
static void glyphs_read(const char *guest, int64_t at, int64_t n) {
    for (int i=0;i<texture_count;++i) {
        Texture *t=&textures[i];
        if (at<(int64_t)(t->spec.offset+t->spec.length) && at+n>(int64_t)t->spec.offset && !strcasecmp(t->spec.guest,guest))
            __atomic_store_n(&t->read_at,glyph_now(),__ATOMIC_RELEASE);
    }
}
void runtime_glyphs_start(void) {
    static int started;
    const char *path=getenv("GOW3_GLYPH_SWAP");
    if (started++ || !path || !*path) return;
    for (int k=0;k<KEYS;++k) keys[k]=glyph_key(key_names[k]);
    launch_style=glyph_style_index(getenv("GOW3_GLYPH_STYLE"));
    if (launch_style<0) launch_style=0;
    load_config(path);
    printf("Runtime: glyphs: %d textures from %s\n",texture_count,path);
    if (!texture_count) return;
    runtime_file_watch(glyphs_watches,glyphs_read);
    pthread_t thread;
    if (!pthread_create(&thread,NULL,glyph_thread,NULL)) pthread_detach(thread);
}
