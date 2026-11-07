/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GOW3_GLYPH_SWAP_H
#define GOW3_GLYPH_SWAP_H
/* The side-effect-free parts of src/runtime_glyphs.c (WipEout; image offsets of CUSA07671 01.07,
 * which another build such as 01.09 moves: runtime_glyphs.c then finds the store and the table by
 * what they hold, glyph_store_holds and glyph_table_slot):
 * - the game's hash of setting and row names: CRC-32 (0xedb88320) from 0, inverted (0x401a08);
 * - its settings list: store at u64 [0x10ea398], head at store+0x580, node +0 u32 key, +0x48 u32
 *   value length with its NUL, +0x4c u32 capacity of the value, +0x50 next, +0x68 the value;
 * - its table of rows on open screens (0x11ac4c8): 256 value pointers, then 256 u64 keys at
 *   +0x800 (hash of "FE_" + the row's global) and 256 u32 counts at +0x1000; a value pointer
 *   points at the row's current-value field, a char * to the selected entry's value string;
 * - GOW3_GLYPH_SWAP lines (scripts/wipeout_glyphs.py variants()). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { GLYPH_STYLES=3, GLYPH_STORE_HEAD=0x580, GLYPH_ROWS=256, GLYPH_MAX_NODES=8192 };
static const char *const glyph_style_names[GLYPH_STYLES]={"playstation","xbox","nintendo"};

typedef struct { char guest[128]; uint64_t offset, length, skip; char files[GLYPH_STYLES][512]; } GlyphTexture;

static inline uint32_t glyph_key(const char *name) {
    uint32_t c=0;
    for (const unsigned char *p=(const unsigned char *)name; *p; ++p) {
        c^=*p;
        for (int k=0;k<8;++k) c=(c>>1)^(0xedb88320u & (0u-(c&1u)));
    }
    return ~c;
}
static inline uint64_t glyph_load64(const unsigned char *p) { uint64_t v; memcpy(&v,p,8); return v; }
static inline uint32_t glyph_load32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
/* The value of the setting with this key, copied into out (NUL-terminated); NULL when absent. */
static inline const char *glyph_setting(const unsigned char *store, uint32_t key, char *out, size_t cap) {
    const unsigned char *node=(const unsigned char *)(uintptr_t)glyph_load64(store+GLYPH_STORE_HEAD);
    for (int n=0; node && n<GLYPH_MAX_NODES; ++n, node=(const unsigned char *)(uintptr_t)glyph_load64(node+0x50)) {
        if (glyph_load32(node)!=key) continue;
        size_t length=glyph_load32(node+0x48);
        if (!cap) return NULL;
        if (length>cap-1) length=cap-1;
        memcpy(out,node+0x68,length);
        out[length]=0;
        return out;
    }
    return NULL;
}
/* Whether a row registered under this key is on an open screen. */
static inline int glyph_row_open(const unsigned char *table, uint32_t key) {
    for (int i=0;i<GLYPH_ROWS;++i)
        if ((uint32_t)glyph_load64(table+0x800+8*i)==key && glyph_load32(table+0x1000+4*i)) return 1;
    return 0;
}
/* The selected value of the open row with this key (the guest's string, not a copy); NULL when no
 * open row has the key or its pointers are NULL. */
static inline const char *glyph_row_value(const unsigned char *table, uint32_t key) {
    for (int i=0;i<GLYPH_ROWS;++i) {
        if ((uint32_t)glyph_load64(table+0x800+8*i)!=key || !glyph_load32(table+0x1000+4*i)) continue;
        const unsigned char *field=(const unsigned char *)(uintptr_t)glyph_load64(table+8*i);
        return field ? (const char *)(uintptr_t)glyph_load64(field) : NULL;
    }
    return NULL;
}
/* Writes value (with its NUL) into the existing setting with this key when it fits the node's
 * capacity, the rest of the capacity zeroed, as the game's own in-place write (0x696300) does; 1
 * when written. Never grows a node and never adds a missing one. The caller commits only values
 * glyph_style_index() knows (>= 0). */
static inline int glyph_commit(unsigned char *store, uint32_t key, const char *value) {
    unsigned char *node=(unsigned char *)(uintptr_t)glyph_load64(store+GLYPH_STORE_HEAD);
    const size_t n=strlen(value)+1;
    for (int i=0; node && i<GLYPH_MAX_NODES; ++i, node=(unsigned char *)(uintptr_t)glyph_load64(node+0x50)) {
        if (glyph_load32(node)!=key) continue;
        const uint32_t capacity=glyph_load32(node+0x4c), length=(uint32_t)n;
        if (n>capacity || capacity>4096) return 0; /* a larger capacity is a corrupt node */
        memcpy(node+0x68,value,n);
        memset(node+0x68+n,0,capacity-n);
        memcpy(node+0x48,&length,4);
        return 1;
    }
    return 0;
}
static inline int glyph_style_index(const char *value) {
    for (int i=0; value && i<GLYPH_STYLES; ++i) if (!strcmp(value,glyph_style_names[i])) return i;
    return -1;
}
/* The value of node's setting when it is a known style (glyph_style_index), else -1: a value whose
 * length (with its NUL) fits its capacity and the node. */
static inline int glyph_node_style(const unsigned char *node) {
    const uint32_t length=glyph_load32(node+0x48), capacity=glyph_load32(node+0x4c);
    char value[16];
    if (!length || length>capacity || capacity>4096 || length>sizeof(value)) return -1;
    memcpy(value,node+0x68,length);
    if (value[length-1]) return -1;
    return glyph_style_index(value);
}
/* Whether `store` looks like the game's settings store: its list (walked at most max_nodes, every
 * node pointer 8-aligned) holds the setting with this key and a known style as its value. For
 * finding the store in a build whose offsets differ; the caller recovers from faults. */
static inline int glyph_store_holds(const unsigned char *store, uint32_t key, int max_nodes) {
    const unsigned char *node=(const unsigned char *)(uintptr_t)glyph_load64(store+GLYPH_STORE_HEAD);
    for (int n=0; node && n<max_nodes; ++n, node=(const unsigned char *)(uintptr_t)glyph_load64(node+0x50)) {
        if ((uintptr_t)node & 7) return 0;
        if (glyph_load32(node)==key) return glyph_node_style(node)>=0;
    }
    return 0;
}
/* Whether slot i of `table` is an open row with this key: its key, a count and a value pointer, as
 * the table of rows on open screens holds while a Controller type row is shown. What the value
 * points at is read with glyph_value_style. */
static inline int glyph_table_slot(const unsigned char *table, int i, uint32_t key) {
    if (i<0 || i>=GLYPH_ROWS || (uint32_t)glyph_load64(table+0x800+8*i)!=key || !glyph_load32(table+0x1000+4*i)) return 0;
    const uint64_t field=glyph_load64(table+8*i);
    return field && !(field & 7);
}
/* The value pointer of the open row with this key (what its slot points at), or NULL. */
static inline const unsigned char *glyph_row_field(const unsigned char *table, uint32_t key) {
    for (int i=0;i<GLYPH_ROWS;++i)
        if (glyph_table_slot(table,i,key)) return (const unsigned char *)(uintptr_t)glyph_load64(table+8*i);
    return NULL;
}
/* The value pointer of the row with this key whatever its count (a table whose counts do not say
 * which rows are open), or NULL. */
static inline const unsigned char *glyph_row_field_any(const unsigned char *table, uint32_t key) {
    for (int i=0;i<GLYPH_ROWS;++i) {
        if ((uint32_t)glyph_load64(table+0x800+8*i)!=key) continue;
        const uint64_t field=glyph_load64(table+8*i);
        if (field && !(field & 7)) return (const unsigned char *)(uintptr_t)field;
    }
    return NULL;
}
/* The style a row's text names: a style (an entry's value) or an entry's label as the row writes
 * them (scripts/controller_row.py: FE_DEFAULT is PlayStation, XBOX is Xbox); -1 otherwise. */
static inline int glyph_value_style(const char *text) {
    const int style=glyph_style_index(text);
    if (style>=0 || !text) return style;
    if (!strcmp(text,"FE_DEFAULT") || !strcmp(text,"DEFAULT")) return 0;
    if (!strcmp(text,"XBOX")) return 1;
    return -1;
}
/* The style the string an open row's value pointer points at names (the selected entry's value),
 * or -1. Reads where the pointer's first 8 bytes point: the runtime calls it under fault recovery. */
static inline int glyph_pointed_style(const unsigned char *field) {
    const uint64_t string=field ? glyph_load64(field) : 0;
    return string && !(string>>47) ? glyph_value_style((const char *)(uintptr_t)string) : -1;
}
/* The style the text at the value pointer itself names, or -1. */
static inline int glyph_inline_style(const unsigned char *field) {
    return field ? glyph_value_style((const char *)field) : -1;
}
/* The value's style: the string it points at, else the text at it. */
static inline int glyph_field_style(const unsigned char *field) {
    const int style=glyph_pointed_style(field);
    return style>=0 ? style : glyph_inline_style(field);
}
/* The style the prompts should show: an open Controller type row's live value (player 2's before
 * player 1's; one Controls screen is open at a time), else player 1's saved setting. Each is a
 * glyph_style_index, -1 when absent. */
static inline int glyph_wanted_live(int live_p1, int live_p2, int saved_p1) {
    return live_p2>=0 ? live_p2 : live_p1>=0 ? live_p1 : saved_p1;
}
/* The style all n copies of a texture hold, or -1 when they differ or there are none. */
static inline int glyph_copies_style(const int *styles, int n) {
    for (int i=1;i<n;++i) if (styles[i]!=styles[0]) return -1;
    return n>0 ? styles[0] : -1;
}
static inline int glyph_verify(const void *at, const void *texels, size_t n) { return !memcmp(at,texels,n); }
/* A GOW3_GLYPH_SWAP line: guest archive, offset, packed length, header length (hex), then
 * style=file fields; tab-separated. 1 when it reads and names at least one known style. */
static inline int glyph_parse_line(const char *line, GlyphTexture *t) {
    memset(t,0,sizeof(*t));
    const char *p=line;
    int field=0, styles=0;
    while (*p && *p!='\n' && *p!='\r') {
        const char *end=p;
        while (*end && *end!='\t' && *end!='\n' && *end!='\r') ++end;
        const size_t n=(size_t)(end-p);
        char text[600];
        if (n>=sizeof(text)) return 0;
        memcpy(text,p,n); text[n]=0;
        if (field==0) {
            if (n>=sizeof(t->guest)) return 0;
            memcpy(t->guest,text,n+1);
        } else if (field<=3) {
            char *stop;
            const uint64_t v=strtoull(text,&stop,16);
            if (!n || *stop) return 0;
            if (field==1) t->offset=v; else if (field==2) t->length=v; else t->skip=v;
        } else {
            const char *eq=strchr(text,'=');
            if (!eq) return 0;
            int style=-1;
            for (int i=0;i<GLYPH_STYLES;++i)
                if ((size_t)(eq-text)==strlen(glyph_style_names[i]) && !strncmp(text,glyph_style_names[i],(size_t)(eq-text))) style=i;
            if (style<0 || strlen(eq+1)>=sizeof(t->files[0])) return 0;
            strcpy(t->files[style],eq+1);
            ++styles;
        }
        ++field;
        p=*end=='\t' ? end+1 : end;
    }
    return field>=5 && styles>0 && t->guest[0];
}
#endif
