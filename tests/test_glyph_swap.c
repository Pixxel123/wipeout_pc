#include "glyph_swap.h"
#include "memory_find.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put64(unsigned char *p, uint64_t v) { memcpy(p,&v,8); }
static void put32(unsigned char *p, uint32_t v) { memcpy(p,&v,4); }

/* memory_find_aligned: aligned copies of any block in one pass, unaligned ones skipped. */
static void test_find_aligned(void) {
    enum { BLOCK=3*4096 };
    static unsigned char memory[64*4096] __attribute__((aligned(4096)));
    static unsigned char x[BLOCK], y[BLOCK];
    for (int i=0;i<BLOCK;++i) { x[i]=(unsigned char)(i*7+1); y[i]=(unsigned char)(i*13+5); }
    const unsigned char *blocks[3]={x,NULL,y};
    const size_t sizes[3]={BLOCK,0,BLOCK}, sig[3]={4096,0,8192};
    memset(memory,0,sizeof(memory));
    memcpy(memory+256,x,BLOCK);            /* aligned: found */
    memcpy(memory+5*4096+16,x,BLOCK);      /* a stale copy off the boundary: skipped */
    memcpy(memory+9*4096+512,y,BLOCK);     /* the other block, aligned: found */
    memcpy(memory+14*4096,x,BLOCK-1);      /* not whole: skipped */
    memcpy(memory+20*4096+768,x,BLOCK);    /* aligned again: found */
    memcpy(memory+60*4096,y,4096);         /* cut off by the range's end: skipped */
    uintptr_t at[8]; int which[8];
    const uintptr_t lo=(uintptr_t)memory, hi=lo+61*4096;
    int n=memory_find_aligned(lo,hi,blocks,sizes,sig,3,at,which,0,8);
    assert(n==3);
    assert(at[0]==lo+256 && which[0]==0);
    assert(at[1]==lo+9*4096+512 && which[1]==2);
    assert(at[2]==lo+20*4096+768 && which[2]==0);
    /* Appended after the hits already found, never past max. */
    assert(memory_find_aligned(lo,hi,blocks,sizes,sig,3,at,which,6,8)==8 && at[6]==lo+256 && at[7]==lo+9*4096+512);
    /* A block that starts before the range is not taken for one inside it. */
    assert(memory_find_aligned(lo+512,hi,blocks,sizes,sig,3,at,which,0,8)==2 && at[0]==lo+9*4096+512);
    /* A signature off the alignment cannot be looked for this way. */
    const size_t odd[3]={100,0,8192+4};
    assert(memory_find_aligned(lo,hi,blocks,sizes,odd,3,at,which,0,8)==0);
}

int main(void) {
    test_find_aligned();
    assert(glyph_key("Motion Sensitivity")==0x1b432e78u);
    assert(glyph_key("Pad Style")==0xdde7131fu);
    assert(glyph_key("FE_Pad Style")==0xedb96967u);

    static unsigned char store[0x600], a[0xa8], b[0xa8];
    put64(store+0x580,(uint64_t)(uintptr_t)a);
    put32(a,glyph_key("Language")); put32(a+0x48,2); memcpy(a+0x68,"1",2); put64(a+0x50,(uint64_t)(uintptr_t)b);
    put32(b,glyph_key("Pad Style P1")); put32(b+0x48,5); memcpy(b+0x68,"xbox",5); put64(b+0x50,0);
    char value[64];
    assert(glyph_setting(store,glyph_key("Pad Style P1"),value,sizeof(value)) && !strcmp(value,"xbox"));
    assert(!glyph_setting(store,glyph_key("Pad Style P2"),value,sizeof(value)));
    put64(b+0x50,(uint64_t)(uintptr_t)a); /* a loop: the walk still ends */
    assert(!glyph_setting(store,glyph_key("Pad Style P2"),value,sizeof(value)));

    static unsigned char table[0x1400];
    put64(table+0x800+8*7,glyph_key("FE_Pad Style P2"));
    assert(!glyph_row_open(table,glyph_key("FE_Pad Style P2")));
    put32(table+0x1000+4*7,1);
    assert(glyph_row_open(table,glyph_key("FE_Pad Style P2")));
    assert(!glyph_row_open(table,glyph_key("FE_Pad Style P1")));

    /* The open row's selected value: slot pointer -> current-value field -> string. */
    static const char selected[]="nintendo";
    uint64_t field=(uint64_t)(uintptr_t)selected;
    put64(table+8*7,(uint64_t)(uintptr_t)&field);
    assert(glyph_row_value(table,glyph_key("FE_Pad Style P2"))==selected);
    assert(!glyph_row_value(table,glyph_key("FE_Pad Style P1")));        /* no such row */
    put32(table+0x1000+4*7,0);
    assert(!glyph_row_value(table,glyph_key("FE_Pad Style P2")));        /* not open */
    put32(table+0x1000+4*7,1);
    field=0;
    assert(!glyph_row_value(table,glyph_key("FE_Pad Style P2")));        /* NULL string */
    put64(table+8*7,0);
    assert(!glyph_row_value(table,glyph_key("FE_Pad Style P2")));        /* NULL slot */

    /* Commit: only into an existing node, only within its capacity. */
    put64(b+0x50,0);
    put32(b+0x4c,12);
    assert(glyph_commit(store,glyph_key("Pad Style P1"),"nintendo"));
    assert(glyph_load32(b+0x48)==9 && !strcmp((const char *)b+0x68,"nintendo"));
    b[0x68+12]=0x55;                                                       /* past the capacity */
    assert(glyph_commit(store,glyph_key("Pad Style P1"),"xbox"));          /* the rest is zeroed */
    assert(glyph_load32(b+0x48)==5 && !memcmp(b+0x68,"xbox\0\0\0\0\0\0\0\0",12) && b[0x68+12]==0x55);
    assert(glyph_commit(store,glyph_key("Pad Style P1"),"nintendo"));
    assert(glyph_setting(store,glyph_key("Pad Style P1"),value,sizeof(value)) && !strcmp(value,"nintendo"));
    put32(b+0x4c,9);
    assert(!glyph_commit(store,glyph_key("Pad Style P1"),"playstation")); /* 12 bytes > capacity 9 */
    assert(glyph_load32(b+0x48)==9 && !strcmp((const char *)b+0x68,"nintendo"));
    b[0x68+9]=0x55;
    assert(glyph_commit(store,glyph_key("Pad Style P1"),"nintendo"));    /* 9 bytes == capacity 9 */
    assert(glyph_load32(b+0x48)==9 && !strcmp((const char *)b+0x68,"nintendo") && b[0x68+9]==0x55);
    put32(b+0x4c,4097);                                                    /* corrupt: refused */
    assert(!glyph_commit(store,glyph_key("Pad Style P1"),"xbox"));
    assert(glyph_load32(b+0x48)==9 && !strcmp((const char *)b+0x68,"nintendo"));
    put32(b+0x4c,9);
    unsigned char before_a[sizeof(a)], before_b[sizeof(b)];
    memcpy(before_a,a,sizeof(a)); memcpy(before_b,b,sizeof(b));
    assert(!glyph_commit(store,glyph_key("Pad Style P2"),"xbox"));       /* missing: nothing written */
    assert(!memcmp(before_a,a,sizeof(a)) && !memcmp(before_b,b,sizeof(b)));

    assert(glyph_style_index("playstation")==0 && glyph_style_index("xbox")==1 && glyph_style_index("nintendo")==2);
    assert(glyph_style_index("motion off")==-1 && glyph_style_index(NULL)==-1);
    /* Live row values first (P2's row before P1's), else the saved P1. */
    assert(glyph_wanted_live(-1,0,1)==0);   /* P2's row open */
    assert(glyph_wanted_live(2,-1,1)==2);   /* P1's row open */
    assert(glyph_wanted_live(2,0,1)==0);    /* both: P2's */
    assert(glyph_wanted_live(-1,-1,1)==1);  /* no row: saved P1 */
    assert(glyph_wanted_live(-1,-1,-1)==-1);
    const int same[3]={1,1,1}, mixed[3]={1,0,1};
    assert(glyph_copies_style(same,3)==1 && glyph_copies_style(mixed,3)==-1 && glyph_copies_style(same,0)==-1);
    assert(glyph_copies_style(mixed,1)==1);

    GlyphTexture t;
    const char *line="/app0/data08.psarc\t7439dc50\t16ab0\t100\tplaystation=/a b/ps.gnf\txbox=/a b/x.gnf\n";
    assert(glyph_parse_line(line,&t));
    assert(!strcmp(t.guest,"/app0/data08.psarc") && t.offset==0x7439dc50 && t.length==0x16ab0 && t.skip==0x100);
    assert(!strcmp(t.files[0],"/a b/ps.gnf") && !strcmp(t.files[1],"/a b/x.gnf") && !t.files[2][0]);
    assert(!glyph_parse_line("/app0/x.psarc\tzz\n",&t));
    assert(!glyph_parse_line("/app0/x.psarc\t10\t10\t100\tpurple=/f\n",&t));

    const unsigned char x[4]={1,2,3,4}, y[4]={1,2,3,5};
    assert(glyph_verify(x,x,4) && !glyph_verify(x,y,4));
    printf("glyph swap tests passed\n");
    return 0;
}
