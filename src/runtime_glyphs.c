/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Live controller glyphs (WipEout, the Controls screens' Controller type rows):
 * GOW3_GLYPH_SWAP=<file> names the button font's atlas and tab textures and a file of each style's
 * version (scripts/wipeout_glyphs.py --swap-config). A thread reads four times a second the live
 * value of an open Controller type row and the game's setting "Pad Style P1" (src/glyph_swap.h):
 * an open row's value is shown as it changes, else the saved P1. When the wanted style changes it
 * finds each texture's copies in guest memory by their bytes (runtime_memory_find_all, up to
 * GLYPH_MAX_COPIES) and writes the wanted style's texels over each. Plain writes, as the game's
 * own: the GPU's page tracking uploads the texture again. Smooth play first: nothing is searched
 * or written unless the wanted style changed, a row has just opened (a menu screen) or the game
 * has just read a texture from its archive (then the texture may have moved), and a copy is
 * written only where its whole texels were just checked to be the version thought to be there. Offsets are CUSA07671's (glyph_swap.h);
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

enum { GLYPH_MAX_TEXTURES=8, GLYPH_MAX_COPIES=4 };
#define GLYPH_GUARD_AT 0x87f142
#define GLYPH_STORE_AT 0x10ea398
#define GLYPH_TABLE_AT 0x11ac4c8
static const char glyph_guard[]="E:\\WOPS4\\Wipeout\\Code\\Frontend\\Items\\List_Item.cpp";

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
        if (styles<2) { for (int s=0;s<GLYPH_STYLES;++s) free(t->data[s]); memset(t,0,sizeof(*t)); continue; }
        t->shown=launch_style; t->failed_for=-2; /* as loaded: where is found only when needed */
        ++texture_count;
    }
    fclose(f);
}
/* Whether the image is the build these offsets are for, read under fault recovery once; the
 * switcher thread and the Accept hook share the answer. */
static int build_state; /* 0 not checked, 1 the known build, -1 another */
static int known_build(const unsigned char *image) {
    const int state=__atomic_load_n(&build_state,__ATOMIC_ACQUIRE);
    if (state) return state>0;
    sigjmp_buf recover;
    volatile int ok=0;
    sigjmp_buf *const outer=runtime_fault_recover;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        ok=!memcmp(image+GLYPH_GUARD_AT,glyph_guard,sizeof(glyph_guard));
    }
    runtime_fault_recover=outer;
    __atomic_store_n(&build_state,ok ? 1 : -1,__ATOMIC_RELEASE);
    return ok;
}
/* Guest reads under fault recovery: 0 when a fault stopped them. The live values are those of
 * open Controller type rows (-1 when not open), rows_open whether either row is open (registered
 * with a count, whatever its value pointers hold at that moment). */
static int read_state(const unsigned char *image, int *saved_p1, int *live_p1, int *live_p2, int *rows_open) {
    sigjmp_buf recover;
    volatile int ok=0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        const unsigned char *store=(const unsigned char *)(uintptr_t)glyph_load64(image+GLYPH_STORE_AT);
        char value[64];
        *saved_p1=store ? glyph_style_index(glyph_setting(store,glyph_key("Pad Style P1"),value,sizeof(value))) : -1;
        const char *row1=glyph_row_value(image+GLYPH_TABLE_AT,glyph_key("FE_Pad Style P1"));
        const char *row2=glyph_row_value(image+GLYPH_TABLE_AT,glyph_key("FE_Pad Style P2"));
        *live_p1=glyph_style_index(row1);
        *live_p2=glyph_style_index(row2);
        *rows_open=glyph_row_open(image+GLYPH_TABLE_AT,glyph_key("FE_Pad Style P1")) ||
                   glyph_row_open(image+GLYPH_TABLE_AT,glyph_key("FE_Pad Style P2"));
        ok=1;
    }
    runtime_fault_recover=NULL;
    return ok;
}
/* Whether copy i still holds the whole texels of its style. */
static int still_there(const Texture *t, int i) {
    sigjmp_buf recover;
    volatile int ok=0;
    const int style=t->at_style[i];
    if (!t->at[i] || style<0 || !t->data[style]) return 0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        ok=glyph_verify(t->at[i],t->data[style]+t->spec.skip,t->size[style]);
    }
    runtime_fault_recover=NULL;
    return ok;
}
/* Whether copies are known and every one is still as it was left. */
static int all_there(const Texture *t) {
    for (int i=0;i<t->copies;++i) if (!still_there(t,i)) return 0;
    return t->copies>0;
}
/* Writes style `want` over copy i once its texels are checked to be its style's; 1 when done. */
static int write_style(Texture *t, int i, int want) {
    sigjmp_buf recover;
    volatile int ok=0;
    const size_t n=t->size[want];
    const int style=t->at_style[i];
    if (!t->at[i] || style<0 || t->size[style]!=n) return 0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        if (glyph_verify(t->at[i],t->data[style]+t->spec.skip,n)) {
            memcpy(t->at[i],t->data[want]+t->spec.skip,n);
            ok=1;
        }
    }
    runtime_fault_recover=NULL;
    return ok;
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
           t->copies,(double)(glyph_now()-start)/1e6);
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
static const char *style_or_dash(int style) { return style>=0 ? glyph_style_names[style] : "-"; }
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
    int last_want=-2, was_open=0;
    for (;;) {
        glyph_pause_ns(250000000ull);
        int saved_p1=-1, live_p1=-1, live_p2=-1, rows_open=0;
        if (!read_state(image,&saved_p1,&live_p1,&live_p2,&rows_open)) continue;
        const int want=glyph_wanted_live(live_p1,live_p2,saved_p1);
        if (want!=last_want)
            printf("Runtime: glyphs: wanted %s (live P1 %s, live P2 %s, saved P1 %s)\n",style_or_dash(want),
                   style_or_dash(live_p1),style_or_dash(live_p2),style_or_dash(saved_p1));
        last_want=want;
        const int opened=rows_open && !was_open;
        was_open=rows_open;
        const uint64_t now=glyph_now();
        for (int i=0;i<texture_count;++i) {
            Texture *t=&textures[i];
            /* The game read the texture again (a load): its file patches gave it the start style,
             * maybe somewhere else. Searched for only if another style is wanted (below). */
            const uint64_t read=__atomic_load_n(&t->read_at,__ATOMIC_ACQUIRE);
            if (read && now>read && now-read>1500000000ull) {
                __atomic_store_n(&t->read_at,0,__ATOMIC_RELEASE);
                t->copies=0; t->shown=launch_style; t->failed_for=-2;
                t->reloaded=1; t->retry_at=0;
            }
            /* A Controller type row just opened (a menu screen, never a race): the copies are found
             * now, so the row's first change shows at once. A search that finds none leaves the
             * state as it was and nothing more is searched this tick. */
            if (opened && now>=t->retry_at && !all_there(t)) {
                const int reloaded=t->reloaded, shown=t->shown;
                locate(t);
                if (!t->copies) { t->reloaded=reloaded; t->shown=shown; continue; }
                t->failed_for=-2;   /* found: an earlier failed search no longer applies */
            }
            if (want<0 || !t->data[want] || t->shown==want || t->failed_for==want || now<t->retry_at) continue;
            if (!all_there(t)) {
                const int reloaded=t->reloaded;
                locate(t);          /* the only search: the wanted style changed, a place is unknown */
                if (!t->copies) {
                    /* After a read the game may still be unpacking it: one more try 2 s later. */
                    if (reloaded && !t->retry_at) { t->retry_at=now+2000000000ull; t->reloaded=1; }
                    else t->failed_for=want;
                    continue;
                }
                if (t->shown==want) continue;
            }
            if (write_copies(t,want)) {
                t->shown=want;
                t->failed_for=-2;
            } else {
                t->shown=-1;        /* all changed under us: searched for again at the next change */
                t->failed_for=want;
            }
        }
    }
    return NULL;
}
/* The Controls screen's Accept (probe.c hook in its Update, game thread): its own code copies only
 * the settings it knows into the profile, and a List row writes its setting only when its screen
 * is left with previous="true", which Accept's redirect is not. So the open Controller type row's
 * value is written here into its existing "Pad Style P<n>" setting. Only the screen's own row is
 * open; nothing is written for a value that is not a known style or does not fit the node. */
ABI void runtime_glyphs_accept(void) {
    unsigned char *image=runtime_image_base();
    if (!image || !known_build(image)) return;
    static const char *const rows[2]={"FE_Pad Style P1","FE_Pad Style P2"};
    static const char *const settings[2]={"Pad Style P1","Pad Style P2"};
    volatile int style[2]={-1,-1}, committed[2]={0,0};
    sigjmp_buf recover;
    sigjmp_buf *const outer=runtime_fault_recover;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        unsigned char *store=(unsigned char *)(uintptr_t)glyph_load64(image+GLYPH_STORE_AT);
        for (int p=0; store && p<2; ++p) {
            style[p]=glyph_style_index(glyph_row_value(image+GLYPH_TABLE_AT,glyph_key(rows[p])));
            if (style[p]>=0) committed[p]=glyph_commit(store,glyph_key(settings[p]),glyph_style_names[style[p]]);
        }
    }
    runtime_fault_recover=outer;
    for (int p=0;p<2;++p) {
        if (style[p]<0) continue;
        if (committed[p]) printf("Runtime: Controller type P%d accepted: %s\n",p+1,glyph_style_names[style[p]]);
        else printf("Runtime: Controller type P%d not kept (%s: no setting it fits in)\n",p+1,glyph_style_names[style[p]]);
    }
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
