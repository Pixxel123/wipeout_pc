/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Live controller glyphs and picture (WipEout, the Controls screens' Controller type rows):
 * GOW3_GLYPH_SWAP=<file> names the button font's atlas and tab textures and the Controls screen's
 * controller sheets, and a file of each style's version (scripts/wipeout_glyphs.py and
 * scripts/xbox_controls.py --swap-config). A thread reads four times a second the live
 * value of an open Controller type row and the game's setting "Pad Style P1" (src/glyph_swap.h):
 * an open row's value is shown as it changes, else the saved P1. When the wanted style changes it
 * finds each texture's copies in guest memory by their bytes (runtime_memory_find_all, up to
 * GLYPH_MAX_COPIES) and writes the wanted style's texels over each, through the backing view with
 * the range invalidated for the GPU (runtime_memory_write_guest): a page the guest keeps read-only
 * cannot stop it, and a texture smaller than a page is uploaded again whatever its first texels. Smooth play first: nothing is searched
 * or written unless the wanted style changed, a row has just opened (a menu screen) or the game
 * has just read a texture from its archive (then the texture may have moved), and a copy is
 * written only where its whole texels were just checked to be the version thought to be there.
 * The settings store and the table of open rows are globals of the image: at their 01.07 offsets
 * (CUSA07671, glyph_swap.h) when those hold them, else found by what they hold (another build, such
 * as 01.09, moved them): find_store and find_table. */
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
enum { GLYPH_MAX_TEXTURES=8, GLYPH_MAX_COPIES=16 };
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
    int start;           /* the style the game's file patches load: launch_style, else playstation */
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
        /* A texture without the launch style's version (the controller sheet has no Nintendo one)
         * starts as PlayStation, as run.py loads it. */
        t->start=t->data[launch_style] ? launch_style : 0;
        t->shown=t->start; t->failed_for=-2; /* as loaded: where is found only when needed */
        ++texture_count;
    }
    fclose(f);
}
/* Image offsets of the settings store's pointer and the open rows' table; -1 while not known.
 * Shared by the switcher thread (which finds them) and the Accept hook. */
static int64_t store_at=-1, table_at=-1;
static int accept_hooked; /* probe.c installed the Accept hook (runtime_glyphs_accept_hooked) */

/* Each guarded read in a function of its own, under fault recovery with the caller's own recovery
 * point kept: 0 when a fault stopped it. */
#define GLYPH_GUARDED(ok, expression) do { \
        sigjmp_buf recover_; sigjmp_buf *const outer_=runtime_fault_recover; (ok)=0; \
        if (!sigsetjmp(recover_,0)) { runtime_fault_recover=&recover_; (ok)=(expression); } \
        runtime_fault_recover=outer_; } while (0)

static __attribute__((noinline)) int store_ok(const unsigned char *store) {
    volatile int ok;
    GLYPH_GUARDED(ok,glyph_store_holds(store,glyph_key("Pad Style P1"),2048));
    return ok;
}
static __attribute__((noinline)) int slot_ok(const unsigned char *table, int i, uint32_t key) {
    volatile int ok;
    GLYPH_GUARDED(ok,glyph_table_slot(table,i,key));
    return ok;
}
/* Whether slot i (or any slot, i<0) of table holds an open Controller type row. */
static int table_ok(const unsigned char *table, int i) {
    const uint32_t keys[2]={glyph_key("FE_Pad Style P1"),glyph_key("FE_Pad Style P2")};
    for (int k=0;k<2;++k)
        for (int at=i<0 ? 0 : i; at<(i<0 ? GLYPH_ROWS : i+1); ++at)
            if (slot_ok(table,at,keys[k])) return 1;
    return 0;
}
/* n bytes at `offset` of the image into out; 0 when they cannot be read (a gap between segments). */
static __attribute__((noinline)) int image_read(const unsigned char *image, uint64_t offset, void *out, size_t n) {
    volatile int ok;
    GLYPH_GUARDED(ok,(memcpy(out,image+offset,n),1));
    return ok;
}
static int image_page(const unsigned char *image, uint64_t offset, unsigned char *page) {
    return image_read(image,offset,page,4096);
}
static uint64_t distance(int64_t a, int64_t b) { return a>b ? (uint64_t)(a-b) : (uint64_t)(b-a); }
/* The image offset of the settings store's pointer: an 8-aligned word of the image that points into
 * guest memory at a store whose list holds "Pad Style P1" with a known style (glyph_store_holds);
 * of several, the nearest to 01.07's place. -1 when none (the profile may not be loaded yet). */
static int64_t find_store(const unsigned char *image, uint64_t size) {
    static unsigned char page[4096];
    int64_t best=-1;
    int found=0;
    const uint64_t start=glyph_now();
    for (uint64_t offset=0; offset+4096<=size; offset+=4096) {
        if (!image_page(image,offset,page)) continue;
        for (int w=0;w<512;++w) {
            const uint64_t value=glyph_load64(page+8*w);
            if (value<0x10000 || (value & 7) || value>=(1ull<<40)) continue;
            /* Guest memory (the heap) or the image's own data: the image is not a guest mapping. */
            const int in_image=value-(uintptr_t)image<size && size-(value-(uintptr_t)image)>=GLYPH_STORE_HEAD+8;
            uintptr_t lo, hi;
            int mapped;
            if (!in_image && (!runtime_memory_region((uintptr_t)value,&lo,&hi,&mapped) || !mapped || lo>value ||
                              hi-value<GLYPH_STORE_HEAD+8)) continue;
            if (!store_ok((const unsigned char *)(uintptr_t)value)) continue;
            const int64_t at=(int64_t)(offset+8*(uint64_t)w);
            ++found;
            if (best<0 || distance(at,GLYPH_STORE_AT)<distance(best,GLYPH_STORE_AT)) best=at;
        }
    }
    if (best>=0)
        printf("Runtime: glyphs: settings store at image+0x%llx (%d candidates, %.0f ms)\n",(unsigned long long)best,found,
               (double)(glyph_now()-start)/1e6);
    return best;
}
/* The image offset of the table of rows on open screens, while a Controller type row is open: a
 * word of the image holding a row's key, in the table's key column at slot i, with the slot's count
 * and value (glyph_table_slot); of several, the nearest to 01.07's place. -1 when none. */
static int64_t find_table(const unsigned char *image, uint64_t size) {
    static unsigned char page[4096];
    const uint32_t keys[2]={glyph_key("FE_Pad Style P1"),glyph_key("FE_Pad Style P2")};
    int64_t best=-1;
    int found=0;
    const uint64_t start=glyph_now();
    for (uint64_t offset=0; offset+4096<=size; offset+=4096) {
        if (!image_page(image,offset,page)) continue;
        for (int w=0;w<512;++w) {
            const uint32_t value=(uint32_t)glyph_load64(page+8*w);
            if (value!=keys[0] && value!=keys[1]) continue;
            const uint64_t at=offset+8*(uint64_t)w;
            for (int i=0;i<GLYPH_ROWS;++i) {
                if (at<0x800+8*(uint64_t)i) break;
                const uint64_t base=at-0x800-8*(uint64_t)i;
                if (base+0x1000+4*GLYPH_ROWS>size || !table_ok(image+base,i)) continue;
                ++found;
                if (best<0 || distance((int64_t)base,GLYPH_TABLE_AT)<distance(best,GLYPH_TABLE_AT)) best=(int64_t)base;
            }
        }
    }
    if (best>=0)
        printf("Runtime: glyphs: open rows table at image+0x%llx (%d candidates, %.0f ms)\n",(unsigned long long)best,found,
               (double)(glyph_now()-start)/1e6);
    return best;
}
/* Finds what is not known yet: 01.07's places first, then a search of the image, the store at most
 * every 5 s (until the profile is loaded) and the table every second (it can only be found while a
 * Controller type row is open). Once found, a place is kept: they are globals. */
static void find_places(const unsigned char *image, uint64_t now) {
    static uint64_t store_next, table_next;
    const uint64_t size=runtime_image_size();
    if (__atomic_load_n(&store_at,__ATOMIC_ACQUIRE)<0 && now>=store_next && size) {
        store_next=now+5000000000ull;
        int64_t at=-1;
        unsigned char word[8];
        if (size>GLYPH_STORE_AT+8 && image_read(image,GLYPH_STORE_AT,word,8)) {
            const uint64_t store=glyph_load64(word);
            if (store && store_ok((const unsigned char *)(uintptr_t)store)) at=GLYPH_STORE_AT;
        }
        if (at<0) at=find_store(image,size);
        if (at>=0) __atomic_store_n(&store_at,at,__ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&table_at,__ATOMIC_ACQUIRE)<0 && now>=table_next && size) {
        table_next=now+1000000000ull;
        int64_t at=-1;
        if (size>GLYPH_TABLE_AT+0x1000+4*GLYPH_ROWS && table_ok(image+GLYPH_TABLE_AT,-1)) at=GLYPH_TABLE_AT;
        if (at<0) at=find_table(image,size);
        if (at>=0) __atomic_store_n(&table_at,at,__ATOMIC_RELEASE);
    }
}
/* Guest reads under fault recovery: 0 when a fault stopped them. The live values are those of
 * open Controller type rows (-1 when not open), rows_open whether either row is open (registered
 * with a count, whatever its value pointers hold at that moment). A place not known yet reads as
 * nothing there. */
static int read_state(const unsigned char *image, int saved[2], int *live_p1, int *live_p2, int *rows_open) {
    sigjmp_buf recover;
    volatile int ok=0;
    const int64_t store_off=__atomic_load_n(&store_at,__ATOMIC_ACQUIRE), table_off=__atomic_load_n(&table_at,__ATOMIC_ACQUIRE);
    saved[0]=saved[1]=*live_p1=*live_p2=-1;
    *rows_open=0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        const unsigned char *store=store_off<0 ? NULL : (const unsigned char *)(uintptr_t)glyph_load64(image+store_off);
        char value[64];
        if (store) {
            saved[0]=glyph_style_index(glyph_setting(store,glyph_key("Pad Style P1"),value,sizeof(value)));
            saved[1]=glyph_style_index(glyph_setting(store,glyph_key("Pad Style P2"),value,sizeof(value)));
        }
        if (table_off>=0) {
            const unsigned char *table=image+table_off;
            *live_p1=glyph_style_index(glyph_row_value(table,glyph_key("FE_Pad Style P1")));
            *live_p2=glyph_style_index(glyph_row_value(table,glyph_key("FE_Pad Style P2")));
            *rows_open=glyph_row_open(table,glyph_key("FE_Pad Style P1")) || glyph_row_open(table,glyph_key("FE_Pad Style P2"));
        }
        ok=1;
    }
    runtime_fault_recover=NULL;
    return ok;
}
/* Writes the value of player p's open row into its setting (as the Accept hook): 1 when written. */
static int commit_row(const unsigned char *image, int p, int style) {
    static const char *const settings[2]={"Pad Style P1","Pad Style P2"};
    const int64_t store_off=__atomic_load_n(&store_at,__ATOMIC_ACQUIRE);
    if (store_off<0 || style<0) return 0;
    sigjmp_buf recover;
    volatile int ok=0;
    sigjmp_buf *const outer=runtime_fault_recover;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        unsigned char *store=(unsigned char *)(uintptr_t)glyph_load64(image+store_off);
        ok=store && glyph_commit(store,glyph_key(settings[p]),glyph_style_names[style]);
    }
    runtime_fault_recover=outer;
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
/* Writes style `want` over copy i once its texels are checked to be its style's; 1 when done. The
 * check reads under fault recovery; the write takes the memory lock, so it comes after. */
static int write_style(Texture *t, int i, int want) {
    const size_t n=t->size[want];
    const int style=t->at_style[i];
    if (!t->at[i] || style<0 || t->size[style]!=n || !still_there(t,i)) return 0;
    return runtime_memory_write_guest((uintptr_t)t->at[i],t->data[want]+t->spec.skip,n);
}
static void locate(Texture *t) {
    const unsigned char *blocks[GLYPH_STYLES];
    /* Just read again, the texture holds the start style: a copy of another style written earlier
     * may still be in memory and must not be taken for it. */
    for (int s=0;s<GLYPH_STYLES;++s)
        blocks[s]=t->data[s] && (!t->reloaded || s==t->start) ? t->data[s]+t->spec.skip : NULL;
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
    printf("Runtime: glyph switch on (%d textures, %s at start; Accept %s)\n",texture_count,glyph_style_names[launch_style],
           __atomic_load_n(&accept_hooked,__ATOMIC_ACQUIRE) ? "hooked" : "not hooked: a row's value is kept as it changes");
    int last_want=-2, was_open=0;
    for (;;) {
        glyph_pause_ns(250000000ull);
        find_places(image,glyph_now());
        int saved[2], live_p1=-1, live_p2=-1, rows_open=0;
        if (!read_state(image,saved,&live_p1,&live_p2,&rows_open)) continue;
        const int saved_p1=saved[0];
        /* No Accept hook in this build (its code moved, probe.c): the row's value is written into
         * its setting as it changes, so it stays after the screen is left (Cancel keeps it too). */
        if (!__atomic_load_n(&accept_hooked,__ATOMIC_ACQUIRE)) {
            const int live[2]={live_p1,live_p2};
            for (int p=0;p<2;++p)
                if (live[p]>=0 && live[p]!=saved[p] && commit_row(image,p,live[p]))
                    printf("Runtime: Controller type P%d kept: %s\n",p+1,glyph_style_names[live[p]]);
        }
        const int want=glyph_wanted_live(live_p1,live_p2,saved_p1);
        if (rows_open!=was_open)
            printf("Runtime: glyphs: Controller type row %s (live P1 %s, live P2 %s, saved P1 %s)\n",
                   rows_open ? "open" : "closed",style_or_dash(live_p1),style_or_dash(live_p2),style_or_dash(saved_p1));
        if (want!=last_want) {
            printf("Runtime: glyphs: wanted %s (live P1 %s, live P2 %s, saved P1 %s)\n",style_or_dash(want),
                   style_or_dash(live_p1),style_or_dash(live_p2),style_or_dash(saved_p1));
            /* A new choice: a style whose search failed before is looked for again. */
            for (int i=0;i<texture_count;++i) textures[i].failed_for=-2;
        }
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
                t->copies=0; t->shown=t->start; t->failed_for=-2;
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
    const int64_t table_off=__atomic_load_n(&table_at,__ATOMIC_ACQUIRE);
    if (!image) return;
    if (table_off<0 || __atomic_load_n(&store_at,__ATOMIC_ACQUIRE)<0) {
        printf("Runtime: Controller type not kept (the game's %s not found yet)\n",table_off<0 ? "open rows" : "settings");
        return;
    }
    static const char *const rows[2]={"FE_Pad Style P1","FE_Pad Style P2"};
    volatile int style[2]={-1,-1};
    sigjmp_buf recover;
    sigjmp_buf *const outer=runtime_fault_recover;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        for (int p=0;p<2;++p) style[p]=glyph_style_index(glyph_row_value(image+table_off,glyph_key(rows[p])));
    }
    runtime_fault_recover=outer;
    for (int p=0;p<2;++p) {
        if (style[p]<0) continue;
        if (commit_row(image,p,style[p])) printf("Runtime: Controller type P%d accepted: %s\n",p+1,glyph_style_names[style[p]]);
        else printf("Runtime: Controller type P%d not kept (%s: no setting it fits in)\n",p+1,glyph_style_names[style[p]]);
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
    launch_style=glyph_style_index(getenv("GOW3_GLYPH_STYLE"));
    if (launch_style<0) launch_style=0;
    load_config(path);
    printf("Runtime: glyphs: %d textures from %s\n",texture_count,path);
    if (!texture_count) return;
    runtime_file_watch(glyphs_watches,glyphs_read);
    pthread_t thread;
    if (!pthread_create(&thread,NULL,glyph_thread,NULL)) pthread_detach(thread);
}
