#include "glyph_swap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put64(unsigned char *p, uint64_t v) { memcpy(p,&v,8); }
static void put32(unsigned char *p, uint32_t v) { memcpy(p,&v,4); }

int main(void) {
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
    assert(glyph_style_index("auto")==-1);                                 /* no textures of its own */
    /* AUTO: a choice the rows and saves hold, shown as the player's gamepad's style. */
    assert(glyph_choice("auto")==GLYPH_AUTO && glyph_choice("xbox")==1 && glyph_choice("motion off")==-1);
    assert(glyph_choice(NULL)==-1);
    assert(!strcmp(glyph_choice_name(GLYPH_AUTO),"auto") && !strcmp(glyph_choice_name(2),"nintendo"));
    assert(!glyph_choice_name(-1) && !glyph_choice_name(GLYPH_AUTO+1));
    assert(glyph_style_for_pad("ps4")==0 && glyph_style_for_pad("ps5")==0 && glyph_style_for_pad("ps3")==0);
    assert(glyph_style_for_pad("xbox360")==1 && glyph_style_for_pad("xboxone")==1);
    assert(glyph_style_for_pad("standard")==1 && glyph_style_for_pad("")==1); /* the Steam Deck, any other */
    assert(glyph_style_for_pad("switchpro")==2 && glyph_style_for_pad("joyconpair")==2 &&
           glyph_style_for_pad("gamecube")==2);
    assert(glyph_style_for_pad(NULL)==-1);                                 /* no gamepad */
    assert(glyph_resolve(GLYPH_AUTO,2)==2 && glyph_resolve(1,2)==1 && glyph_resolve(-1,2)==-1);
    assert(glyph_commit(store,glyph_key("Pad Style P1"),glyph_choice_name(GLYPH_AUTO)));
    assert(glyph_setting(store,glyph_key("Pad Style P1"),value,sizeof(value)) && glyph_choice(value)==GLYPH_AUTO);
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
