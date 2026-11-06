/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Live controller glyphs (WipEout, the Controls screens' Controller type rows):
 * GOW3_GLYPH_SWAP=<file> names the button font's atlas and tab textures and a file of each style's
 * version (scripts/wipeout_glyphs.py --swap-config). A thread reads the game's settings "Pad Style
 * P1" and "Pad Style P2" and whether player 2's row is on screen (src/glyph_swap.h) four times a
 * second; when the wanted style changes it finds each texture in guest memory by its bytes
 * (runtime_memory_find) and writes the wanted style's texels over it. Plain writes, as the game's
 * own: the GPU's page tracking uploads the texture again. Smooth play first: nothing is searched
 * or written unless the wanted style changed or the game has just read a texture from its archive
 * (then the texture may have moved), and a texture is written only where its whole texels were
 * just checked to be the version thought to be there. Offsets are CUSA07671's (glyph_swap.h);
 * another build (no List_Item.cpp path at 0x87f142) leaves everything alone. */
#define _GNU_SOURCE
#include "runtime.h"
#include "glyph_swap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

enum { GLYPH_MAX_TEXTURES=8 };
#define GLYPH_GUARD_AT 0x87f142
#define GLYPH_STORE_AT 0x10ea398
#define GLYPH_TABLE_AT 0x11ac4c8
static const char glyph_guard[]="E:\\WOPS4\\Wipeout\\Code\\Frontend\\Items\\List_Item.cpp";

typedef struct {
    GlyphTexture spec;
    unsigned char *data[GLYPH_STYLES]; /* whole files; texels from spec.skip */
    size_t size[GLYPH_STYLES], sig_at[GLYPH_STYLES];
    unsigned char *at;   /* the texels in guest memory, or NULL */
    int shown;           /* the style at `at` */
    int failed_for;      /* the wanted style a search last failed for, or -2 */
    uint64_t read_at;    /* ns of the last read of its archive range; 0 when handled */
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
static size_t signature_at(const unsigned char *texels, size_t n) {
    size_t at=0, best=0;
    for (size_t o=0; o+64<=n; o+=4096) {
        unsigned char seen[256]={0};
        size_t distinct=0;
        for (size_t i=0;i<64;++i) distinct+=!seen[texels[o+i]]++;
        if (distinct>best) { best=distinct; at=o; }
    }
    return at;
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
        if (!glyph_parse_line(line,&t->spec)) continue;
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
        if (styles<2) { for (int s=0;s<GLYPH_STYLES;++s) free(t->data[s]); memset(t,0,sizeof(*t)); continue; }
        t->shown=launch_style; t->failed_for=-2; /* as loaded: where is found only when needed */
        ++texture_count;
    }
    fclose(f);
}
/* Whether the image is the build these offsets are for, read under fault recovery. */
static int known_build(const unsigned char *image) {
    sigjmp_buf recover;
    volatile int ok=0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        ok=!memcmp(image+GLYPH_GUARD_AT,glyph_guard,sizeof(glyph_guard));
    }
    runtime_fault_recover=NULL;
    return ok;
}
/* Guest reads under fault recovery: 0 when a fault stopped them. */
static int read_state(const unsigned char *image, int *p1, int *p2, int *p2_open) {
    sigjmp_buf recover;
    volatile int ok=0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        const unsigned char *store=(const unsigned char *)(uintptr_t)glyph_load64(image+GLYPH_STORE_AT);
        char value[64];
        *p1=store ? glyph_style_index(glyph_setting(store,glyph_key("Pad Style P1"),value,sizeof(value))) : -1;
        *p2=store ? glyph_style_index(glyph_setting(store,glyph_key("Pad Style P2"),value,sizeof(value))) : -1;
        *p2_open=glyph_row_open(image+GLYPH_TABLE_AT,glyph_key("FE_Pad Style P2"));
        ok=1;
    }
    runtime_fault_recover=NULL;
    return ok;
}
/* Whether t->at still holds the whole texels of t->shown. */
static int still_there(const Texture *t) {
    sigjmp_buf recover;
    volatile int ok=0;
    if (!t->at || t->shown<0 || !t->data[t->shown]) return 0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        ok=glyph_verify(t->at,t->data[t->shown]+t->spec.skip,t->size[t->shown]);
    }
    runtime_fault_recover=NULL;
    return ok;
}
/* Writes style `want` over t's texels once they are checked to be t->shown's; 1 when done. */
static int write_style(Texture *t, int want) {
    sigjmp_buf recover;
    volatile int ok=0;
    const size_t n=t->size[want];
    if (!t->at || t->shown<0 || t->size[t->shown]!=n) return 0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        if (glyph_verify(t->at,t->data[t->shown]+t->spec.skip,n)) {
            memcpy(t->at,t->data[want]+t->spec.skip,n);
            ok=1;
        }
    }
    runtime_fault_recover=NULL;
    return ok;
}
static void locate(Texture *t) {
    const unsigned char *blocks[GLYPH_STYLES];
    for (int s=0;s<GLYPH_STYLES;++s) blocks[s]=t->data[s] ? t->data[s]+t->spec.skip : NULL;
    int which=-1;
    t->at=(unsigned char *)runtime_memory_find(blocks,t->size,t->sig_at,GLYPH_STYLES,&which);
    t->shown=t->at ? which : -1;
    if (t->at) printf("Runtime: glyphs: %s @0x%llx found as %s at %p\n",t->spec.guest,(unsigned long long)t->spec.offset,
                      glyph_style_names[which],(void *)t->at);
}
static void *glyph_thread(void *unused) {
    (void)unused;
    unsigned char *image;
    while (!(image=runtime_image_base())) glyph_pause_ns(1000000000ull);
    glyph_pause_ns(2000000000ull);
    if (!known_build(image)) {
        printf("Runtime: glyph switch off (not the WipEout build it knows)\n");
        return NULL;
    }
    printf("Runtime: glyph switch on (%d textures, %s at start)\n",texture_count,glyph_style_names[launch_style]);
    for (;;) {
        glyph_pause_ns(250000000ull);
        int p1=-1, p2=-1, p2_open=0;
        if (!read_state(image,&p1,&p2,&p2_open)) continue;
        const int want=glyph_wanted(p1,p2,p2_open);
        const uint64_t now=glyph_now();
        for (int i=0;i<texture_count;++i) {
            Texture *t=&textures[i];
            /* The game read the texture again (a load): its file patches gave it the start style,
             * maybe somewhere else. Searched for only if another style is wanted (below). */
            const uint64_t read=__atomic_load_n(&t->read_at,__ATOMIC_ACQUIRE);
            if (read && now>read && now-read>1500000000ull) {
                __atomic_store_n(&t->read_at,0,__ATOMIC_RELEASE);
                t->at=NULL; t->shown=launch_style; t->failed_for=-2;
            }
            if (want<0 || !t->data[want] || t->shown==want || t->failed_for==want) continue;
            if (!still_there(t)) {
                locate(t);          /* the only search: the wanted style changed, the place is unknown */
                if (!t->at) { t->failed_for=want; continue; }
                if (t->shown==want) continue;
            }
            if (write_style(t,want)) {
                t->shown=want;
                t->failed_for=-2;
                printf("Runtime: glyphs: %s @0x%llx now %s\n",t->spec.guest,(unsigned long long)t->spec.offset,glyph_style_names[want]);
            } else {
                t->at=NULL;         /* changed under us: searched for again at the next change */
                t->failed_for=want;
            }
        }
    }
    return NULL;
}
int runtime_glyphs_watches(const char *guest) {
    for (int i=0;i<texture_count;++i) if (!strcasecmp(textures[i].spec.guest,guest)) return 1;
    return 0;
}
void runtime_glyphs_read(const char *guest, int64_t at, int64_t n) {
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
    launch_style=glyph_style_index(getenv("GOW3_GLYPH_STYLE"));
    if (launch_style<0) launch_style=0;
    load_config(path);
    pthread_t thread;
    if (texture_count && !pthread_create(&thread,NULL,glyph_thread,NULL)) pthread_detach(thread);
}
