#include "pixel_layout.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>

int main(void) {
    uint32_t stride, span;
    assert(vgl_pixel_layout(3,2,3,0,4,&stride,&span) && stride==12 && span==21);
    assert(vgl_pixel_layout(3,2,3,0,1,&stride,&span) && stride==9 && span==18);
    assert(vgl_pixel_layout(1,3,2,0,8,&stride,&span) && stride==8 && span==18);
    assert(vgl_pixel_layout(4,2,4,8,4,&stride,&span) && stride==32 && span==48);
    assert(vgl_pixel_layout(4,2,4,2,4,&stride,&span) && stride==8 && span==24);
    assert(vgl_pixel_layout(0,INT_MAX,4,0,4,&stride,&span) && span==0);
    assert(!vgl_pixel_layout(INT_MAX,2,4,0,4,&stride,&span));
    assert(!vgl_pixel_layout(1,INT_MAX,4,2,8,&stride,&span));
    assert(!vgl_pixel_layout(-1,2,4,0,4,&stride,&span));
    assert(!vgl_pixel_layout(1,2,4,-1,4,&stride,&span));
    assert(!vgl_pixel_layout(1,2,4,0,3,&stride,&span));
    unsigned char padded[24], tight[20], output[24];
    for (unsigned i=0;i<sizeof(padded);++i) padded[i]=(unsigned char)i;
    memset(tight,0xee,sizeof(tight)); memset(output,0xcc,sizeof(output));
    vgl_pixel_copy_rows(tight+1,9,padded,12,9,2);
    assert(tight[0]==0xee && tight[19]==0xee);
    assert(memcmp(tight+1,padded,9)==0 && memcmp(tight+10,padded+12,9)==0);
    vgl_pixel_copy_rows(output,12,tight+1,9,9,2);
    assert(memcmp(output,padded,9)==0 && memcmp(output+12,padded+12,9)==0);
    assert(output[9]==0xcc && output[10]==0xcc && output[11]==0xcc);
    assert(output[21]==0xcc && output[22]==0xcc && output[23]==0xcc);
    puts("PASS: checked pixel row layout, padded RGB/packed/RGBA, row-length overlap, overflow, guard bytes and untouched pack padding");
}
