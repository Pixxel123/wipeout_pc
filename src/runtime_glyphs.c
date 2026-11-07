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
 * as 01.09, moved them): find_store and locate_table. */
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
        /* Two names of one archive entry (the controller sheet's 2x and 1x pictures can share their
         * bytes) are one texture. */
        int seen=0;
        for (int i=0;i<texture_count && !seen;++i)
            seen=textures[i].spec.offset==t->spec.offset && !strcasecmp(textures[i].spec.guest,t->spec.guest);
        if (seen) { memset(t,0,sizeof(*t)); continue; }
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
/* The image offset of the settings store's pointer; -1 while not known. The table of rows on open
 * screens: table_ref is its address, or with table_indirect the address of the word that holds it;
 * 0 while not known. table_counted: its count column says which rows are open (else a row is open
 * while its value names a style). Shared by the switcher thread and the Accept hook, which find
 * them; table_lock keeps their searches apart. */
static int64_t store_at=-1;
static uintptr_t table_ref;
static int table_indirect, table_counted;
static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static int field_style(const unsigned char *field);
static int accept_hooked; /* probe.c installed the Accept hook (runtime_glyphs_accept_hooked) */
static int accepted;      /* the Accept hook committed a Controller type */

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
static void locate_table(const unsigned char *image, uint64_t size, int deep);
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
            if (store && store_ok((const unsigned char *)(uintptr_t)store)) {
                at=GLYPH_STORE_AT;
                printf("Runtime: glyphs: settings store at image+0x%llx (01.07's place)\n",(unsigned long long)at);
            }
        }
        if (at<0) at=find_store(image,size);
        if (at>=0) __atomic_store_n(&store_at,at,__ATOMIC_RELEASE);
    }
    if (!__atomic_load_n(&table_ref,__ATOMIC_ACQUIRE) && now>=table_next && size) {
        table_next=now+1000000000ull;
        locate_table(image,size,0);
    }
}
/* ---- the table of rows on open screens ----
 * As found in 01.07 (glyph_swap.h): 256 value pointers, their keys (hash of "FE_" + the row's
 * global) at +0x800 and counts at +0x1000. A run on that build found nothing there while the
 * Controls screen was open, so where it is and whether its counts mean anything are found by
 * trying: the place itself, the place as a pointer to the table, and the rows' keys anywhere in
 * the image or the guest's smaller regions. A slot is a Controller type row when it has the key
 * and a value pointer, and either a count (counted) or a value that names a style. */
static const uint32_t *row_keys(void) {
    static uint32_t keys[2];
    if (!keys[0]) { keys[1]=glyph_key("FE_Pad Style P2"); keys[0]=glyph_key("FE_Pad Style P1"); }
    return keys;
}
static __attribute__((noinline)) int read_word(const void *at, uint64_t *out) {
    volatile int ok;
    GLYPH_GUARDED(ok,(memcpy(out,at,8),1));
    return ok;
}
static __attribute__((noinline)) int counted_slot(const unsigned char *table, int i, uint32_t key) {
    volatile int ok;
    GLYPH_GUARDED(ok,glyph_table_slot(table,i,key));
    return ok;
}
/* Key and value pointer, the value naming a style; the count not looked at. */
static int named_slot(const unsigned char *table, int i, uint32_t key) {
    uint64_t k, field;
    if (!read_word(table+0x800+8*i,&k) || (uint32_t)k!=key || !read_word(table+8*i,&field) || !field || (field & 7)) return 0;
    return field_style((const unsigned char *)(uintptr_t)field)>=0;
}
/* 2 when slot i (every slot, i<0) of `table` holds a Controller type row by its count, 1 by its
 * value only, 0 when none. */
static int table_quality(const unsigned char *table, int i) {
    int quality=0;
    for (int k=0;k<2;++k)
        for (int at=i<0 ? 0 : i; at<(i<0 ? GLYPH_ROWS : i+1) && quality<2; ++at) {
            if (counted_slot(table,at,row_keys()[k])) quality=2;
            else if (!quality && named_slot(table,at,row_keys()[k])) quality=1;
        }
    return quality;
}
static int plausible(uint64_t address) { return address>=0x10000 && !(address & 7) && address<(1ull<<40); }
/* The table now: table_ref, or the address the word at table_ref holds; NULL when not known. */
static const unsigned char *table_now(void) {
    const uintptr_t ref=__atomic_load_n(&table_ref,__ATOMIC_ACQUIRE);
    if (!ref || !__atomic_load_n(&table_indirect,__ATOMIC_ACQUIRE)) return (const unsigned char *)ref;
    uint64_t table;
    return read_word((const void *)ref,&table) && plausible(table) ? (const unsigned char *)(uintptr_t)table : NULL;
}
static void use_table(uintptr_t ref, int indirect, int quality, const unsigned char *image, const char *how) {
    __atomic_store_n(&table_counted,quality==2,__ATOMIC_RELEASE);
    __atomic_store_n(&table_indirect,indirect,__ATOMIC_RELEASE);
    __atomic_store_n(&table_ref,ref,__ATOMIC_RELEASE);
    const uint64_t offset=ref-(uintptr_t)image;
    if (offset<runtime_image_size()) printf("Runtime: glyphs: open rows table %s image+0x%llx (%s)\n",
                                            indirect ? "through the pointer at" : "at",(unsigned long long)offset,how);
    else printf("Runtime: glyphs: open rows table at 0x%llx (%s)\n",(unsigned long long)ref,how);
    printf("Runtime: glyphs: rows are open by %s\n",quality==2 ? "their count" : "their value (no count seen)");
}
/* Slot columns around a key found at `key_at`: the best table (quality, address) whose key column
 * has it, preferring a counted one. */
static int table_around(uintptr_t key_at, uintptr_t lowest, uintptr_t *best, int quality) {
    for (int i=0;i<GLYPH_ROWS;++i) {
        if (key_at<lowest+0x800+8*(uintptr_t)i) break;
        const uintptr_t base=key_at-0x800-8*(uintptr_t)i;
        const int q=table_quality((const unsigned char *)base,i);
        if (q>quality) { quality=q; *best=base; }
        if (quality==2) break;
    }
    return quality;
}
/* Finds the table while a Controller type row is open (table_lock held): 01.07's place, that place
 * as a pointer, the image's words holding a row's key, and with `deep` the guest's regions of at
 * most 1 GiB (the heap; the texture pool is larger). A table found in the guest's memory is reached
 * through a word of the image holding its address when there is one (that survives the screen
 * being made again). */
static void locate_table(const unsigned char *image, uint64_t size, int deep) {
    /* The Accept hook (deep) waits for a search the thread is making; the thread skips a tick. */
    if (deep ? pthread_mutex_lock(&table_lock) : pthread_mutex_trylock(&table_lock)) return;
    const uint64_t start=glyph_now();
    const uintptr_t place=(uintptr_t)image+GLYPH_TABLE_AT;
    int quality=0;
    uintptr_t best=0;
    uint64_t word;
    if (size>GLYPH_TABLE_AT+0x1000+4*GLYPH_ROWS && (quality=table_quality((const unsigned char *)place,-1))) {
        use_table(place,0,quality,image,"01.07's place");
        goto done;
    }
    if (size>GLYPH_TABLE_AT+8 && read_word((const void *)place,&word) && plausible(word) &&
        (quality=table_quality((const unsigned char *)(uintptr_t)word,-1))) {
        use_table(place,1,quality,image,"01.07's place holds its address");
        goto done;
    }
    static unsigned char page[4096];
    for (uint64_t offset=0; offset+4096<=size && quality<2; offset+=4096) {
        if (!image_page(image,offset,page)) continue;
        for (int w=0;w<512 && quality<2;++w) {
            const uint32_t value=(uint32_t)glyph_load64(page+8*w);
            if (value==row_keys()[0] || value==row_keys()[1])
                quality=table_around((uintptr_t)image+offset+8*(uintptr_t)w,(uintptr_t)image,&best,quality);
        }
    }
    if (quality) { use_table(best,0,quality,image,"found by its rows' keys in the image"); goto done; }
    if (deep) {
        enum { HITS=64 };
        uintptr_t at[HITS];
        int which[HITS];
        const int hits=runtime_memory_find_u32(row_keys(),2,8,1ull<<30,at,which,HITS);
        for (int h=0;h<hits && quality<2;++h) quality=table_around(at[h],0,&best,quality);
        if (quality) {
            /* A word of the image that holds the table's address reaches it on the next visit too. */
            for (uint64_t offset=0; offset+4096<=size; offset+=4096) {
                if (!image_page(image,offset,page)) continue;
                for (int w=0;w<512;++w)
                    if (glyph_load64(page+8*w)==best) {
                        use_table((uintptr_t)image+offset+8*(uintptr_t)w,1,quality,image,"found in guest memory, its address in the image");
                        printf("Runtime: glyphs: (the table is at 0x%llx; %d key hits, %.0f ms)\n",(unsigned long long)best,hits,
                               (double)(glyph_now()-start)/1e6);
                        goto done;
                    }
            }
            use_table(best,0,quality,image,"found in guest memory");
        }
        printf("Runtime: glyphs: open rows table %s (%d key hits in guest memory, %.0f ms)\n",quality ? "found" : "not found",hits,
               (double)(glyph_now()-start)/1e6);
    }
done:
    pthread_mutex_unlock(&table_lock);
}
/* Once, when Accept is pressed and no table is found: where the rows' keys and the settings' keys
 * are, with the memory around them, so a log shows how this build keeps its open rows. */
static void dump_keys(const unsigned char *image, uint64_t size) {
    static int dumped;
    if (dumped++) return;
    const uint32_t keys[4]={glyph_key("FE_Pad Style P1"),glyph_key("FE_Pad Style P2"),glyph_key("Pad Style P1"),glyph_key("Pad Style P2")};
    static const char *const names[4]={"FE_Pad Style P1","FE_Pad Style P2","Pad Style P1","Pad Style P2"};
    enum { HITS=24 };
    uintptr_t at[HITS];
    int which[HITS], hits=0;
    static unsigned char page[4096];
    for (uint64_t offset=0; offset+4096<=size && hits<HITS; offset+=4096) {
        if (!image_page(image,offset,page)) continue;
        for (int w=0;w<1024 && hits<HITS;++w) {
            const uint32_t value=glyph_load32(page+4*w);
            for (int k=0;k<4;++k) if (value==keys[k]) { at[hits]=(uintptr_t)image+offset+4*(uintptr_t)w; which[hits++]=k; break; }
        }
    }
    const int in_image=hits;
    hits+=runtime_memory_find_u32(keys,4,4,1ull<<30,at+hits,which+hits,HITS-hits);
    uint64_t words[4];
    for (int i=0;i<4;++i) if (!read_word(image+GLYPH_TABLE_AT+(i<2 ? 8*i : 0x800+8*(i-2)),&words[i])) words[i]=0xdeadull;
    printf("Runtime: glyphs: dump: at 01.07's table place %016llx %016llx, keys %016llx %016llx; %d key hits (%d in the image)\n",
           (unsigned long long)words[0],(unsigned long long)words[1],(unsigned long long)words[2],(unsigned long long)words[3],hits,in_image);
    for (int h=0;h<hits;++h) {
        char line[600];
        int n=snprintf(line,sizeof(line),"Runtime: glyphs: dump: %s at ",names[which[h]]);
        const uint64_t offset=at[h]-(uintptr_t)image;
        n+=offset<size ? snprintf(line+n,sizeof(line)-(size_t)n,"image+0x%llx:",(unsigned long long)offset)
                       : snprintf(line+n,sizeof(line)-(size_t)n,"0x%llx:",(unsigned long long)at[h]);
        const uintptr_t from=(at[h] & ~(uintptr_t)7)-0x20;
        for (int w=0;w<12 && n<(int)sizeof(line)-20;++w) {
            uint64_t v;
            n+=read_word((const void *)(from+8*(uintptr_t)w),&v) ? snprintf(line+n,sizeof(line)-(size_t)n," %s%llx",from+8*(uintptr_t)w==(at[h] & ~(uintptr_t)7) ? "*" : "",(unsigned long long)v)
                                                              : snprintf(line+n,sizeof(line)-(size_t)n," ?");
        }
        puts(line);
    }
}
/* Guest reads under fault recovery: 0 when a fault stopped them. The live values are those of
 * open Controller type rows (-1 when not open), rows_open whether either row is open (registered
 * with a count, whatever its value pointers hold at that moment). A place not known yet reads as
 * nothing there. */
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
/* What an open row's value pointer holds, for the log: the text it points at and the text at it. */
static void describe_field(const unsigned char *field, char *out, size_t cap) {
    char pointed[25]="", inline_text[25]="";
    unsigned char word[8];
    if (guarded_text((const char *)field,inline_text) && image_read(field,0,word,8)) {
        const uint64_t string=glyph_load64(word);
        if (string && !(string>>47)) guarded_text((const char *)(uintptr_t)string,pointed);
    }
    snprintf(out,cap,"-> \"%s\", at it \"%s\"",pointed,inline_text);
}
/* The style of an open row's value (glyph_field_style), each reading under fault recovery of its
 * own: the first 8 bytes at the pointer may be text, not an address. */
static __attribute__((noinline)) int field_style(const unsigned char *field) {
    volatile int pointed, at;
    if (!field) return -1;
    GLYPH_GUARDED(pointed,glyph_pointed_style(field)+2);
    if (pointed>=2) return pointed-2;
    GLYPH_GUARDED(at,glyph_inline_style(field)+2);
    return at>=2 ? at-2 : -1;
}
/* text: each open row's value pointer as describe_field reads it ("" when not open). */
static int read_state(const unsigned char *image, int saved[2], int *live_p1, int *live_p2, int *rows_open, char text[2][80]) {
    sigjmp_buf recover;
    volatile int ok=0;
    const unsigned char *volatile fields[2]={NULL,NULL};
    const int64_t store_off=__atomic_load_n(&store_at,__ATOMIC_ACQUIRE);
    const unsigned char *const table=table_now();
    const int counted=__atomic_load_n(&table_counted,__ATOMIC_ACQUIRE);
    saved[0]=saved[1]=*live_p1=*live_p2=-1;
    *rows_open=0;
    text[0][0]=text[1][0]=0;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        const unsigned char *store=store_off<0 ? NULL : (const unsigned char *)(uintptr_t)glyph_load64(image+store_off);
        char value[64];
        if (store) {
            saved[0]=glyph_style_index(glyph_setting(store,glyph_key("Pad Style P1"),value,sizeof(value)));
            saved[1]=glyph_style_index(glyph_setting(store,glyph_key("Pad Style P2"),value,sizeof(value)));
        }
        if (table) {
            for (int p=0;p<2;++p)
                fields[p]=counted ? glyph_row_field(table,row_keys()[p]) : glyph_row_field_any(table,row_keys()[p]);
            *rows_open=counted && (glyph_row_open(table,row_keys()[0]) || glyph_row_open(table,row_keys()[1]));
        }
        ok=1;
    }
    runtime_fault_recover=NULL;
    if (!ok) return 0;
    /* What the values hold, each read guarded on its own (a fault there is no fault of the rest). */
    for (int p=0;p<2;++p) if (fields[p]) describe_field(fields[p],text[p],80);
    *live_p1=field_style(fields[0]);
    *live_p2=field_style(fields[1]);
    if (!counted) *rows_open=*live_p1>=0 || *live_p2>=0; /* no count: a row is open while its value names a style */
    return 1;
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
    printf("Runtime: glyph switch on (%d textures, %s at start, follows the Controller type row once it is open; Accept %s)\n",
           texture_count,glyph_style_names[launch_style],
           __atomic_load_n(&accept_hooked,__ATOMIC_ACQUIRE) ? "hooked" : "not hooked: a row's value is kept as it changes");
    int last_want=-2, was_open=0, row_seen=0, last_live[2]={-1,-1}, active=-1, armed=0, last_saved[2]={-2,-2};
    uint64_t mounted_at=0;
    char last_text[2][80]={"",""};
    for (;;) {
        glyph_pause_ns(250000000ull);
        find_places(image,glyph_now());
        int saved[2], live_p1=-1, live_p2=-1, rows_open=0;
        char text[2][80];
        if (!read_state(image,saved,&live_p1,&live_p2,&rows_open,text)) continue;
        const int saved_p1=saved[0];
        for (int p=0;p<2;++p)
            if (strcmp(text[p],last_text[p])) {
                if (text[p][0]) printf("Runtime: glyphs: P%d row value %s (%s)\n",p+1,text[p],style_or_dash(p ? live_p2 : live_p1));
                strcpy(last_text[p],text[p]);
            }
        /* No Accept hook in this build (its code moved, probe.c): the row's value is written into
         * its setting as it changes, so it stays after the screen is left (Cancel keeps it too). */
        if (!__atomic_load_n(&accept_hooked,__ATOMIC_ACQUIRE)) {
            const int live[2]={live_p1,live_p2};
            for (int p=0;p<2;++p)
                if (live[p]>=0 && live[p]!=saved[p] && commit_row(image,p,live[p]))
                    printf("Runtime: Controller type P%d kept: %s\n",p+1,glyph_style_names[live[p]]);
        }
        /* The prompts stay as loaded (run.py chose them from the save) until the game's profile is
         * in: before it the setting holds the row's default. From then the saved setting is
         * followed (an Accept changes it), and an open row's value while it is open. Armed by the
         * profile's save being mounted 2 s ago, a Controller type row being open, or an Accept. A
         * table without counts may show both screens' rows at once: the one changed last is
         * followed. */
        const uint64_t now_ns=glyph_now();
        if (!mounted_at && runtime_savedata_mounts()) mounted_at=now_ns;
        for (int p=0;p<2;++p)
            if (saved[p]!=last_saved[p]) {
                if (last_saved[p]!=-2 || saved[p]>=0) printf("Runtime: glyphs: saved P%d now %s\n",p+1,style_or_dash(saved[p]));
                last_saved[p]=saved[p];
            }
        const int live[2]={live_p1,live_p2};
        for (int p=0;p<2;++p) {
            if (live[p]>=0 && last_live[p]>=0 && live[p]!=last_live[p]) active=p;
            last_live[p]=live[p];
        }
        const int counted=__atomic_load_n(&table_counted,__ATOMIC_ACQUIRE);
        row_seen|=counted ? rows_open : active>=0;
        if (!armed && (row_seen || __atomic_load_n(&accepted,__ATOMIC_ACQUIRE) || (mounted_at && now_ns-mounted_at>2000000000ull))) {
            armed=1;
            printf("Runtime: glyphs: following the Controller type (saved P1 %s)\n",style_or_dash(saved_p1));
        }
        int want=-1;
        if (armed)
            want=counted ? glyph_wanted_live(live_p1,live_p2,saved_p1) : active>=0 && live[active]>=0 ? live[active] : saved_p1;
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
    if (!image) return;
    /* The Controls screen is open now, so the table can be found: a short search on Accept (once per
     * run when it is found where it stays) rather than keeping no choice at all. */
    const unsigned char *table=table_now();
    static int deep_searches; /* a search of the guest's memory stalls Accept a moment: three at most */
    if ((!table || !table_quality(table,-1)) && deep_searches<3) {
        ++deep_searches;
        locate_table(image,runtime_image_size(),1);
        table=table_now();
    }
    if (!table) {
        dump_keys(image,runtime_image_size());
        printf("Runtime: Controller type not kept (the game's open rows not found)\n");
        return;
    }
    if (__atomic_load_n(&store_at,__ATOMIC_ACQUIRE)<0) {
        printf("Runtime: Controller type not kept (the game's settings not found yet)\n");
        return;
    }
    const int counted=__atomic_load_n(&table_counted,__ATOMIC_ACQUIRE);
    const unsigned char *volatile fields[2]={NULL,NULL};
    int style[2];
    sigjmp_buf recover;
    sigjmp_buf *const outer=runtime_fault_recover;
    if (!sigsetjmp(recover,0)) {
        runtime_fault_recover=&recover;
        for (int p=0;p<2;++p)
            fields[p]=counted ? glyph_row_field(table,row_keys()[p]) : glyph_row_field_any(table,row_keys()[p]);
    }
    runtime_fault_recover=outer;
    for (int p=0;p<2;++p) style[p]=field_style(fields[p]);
    for (int p=0;p<2;++p) {
        if (style[p]<0) continue;
        if (commit_row(image,p,style[p])) {
            __atomic_store_n(&accepted,1,__ATOMIC_RELEASE);
            printf("Runtime: Controller type P%d accepted: %s\n",p+1,glyph_style_names[style[p]]);
        }
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
