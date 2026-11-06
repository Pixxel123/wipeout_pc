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

    assert(glyph_style_index("playstation")==0 && glyph_style_index("xbox")==1 && glyph_style_index("nintendo")==2);
    assert(glyph_style_index("motion off")==-1 && glyph_style_index(NULL)==-1);
    assert(glyph_wanted(1,0,1)==0 && glyph_wanted(1,0,0)==1 && glyph_wanted(1,-1,1)==1 && glyph_wanted(-1,-1,0)==-1);

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
