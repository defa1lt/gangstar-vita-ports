#include "md5.h"
#include <string.h>
/* RFC 1321, used for the original identifier formats. */
static const uint32_t constants[64] = {0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u};
static const unsigned shifts[16] = {7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
void vegas_md5_hex(const void *data, size_t size, char hex[33]) {
    static const char digits[]="0123456789ABCDEF";
    uint8_t digest[16]; vegas_md5(data,size,digest);
    for (unsigned i=0;i<16;++i) {
        hex[i*2]=digits[digest[i]>>4]; hex[i*2+1]=digits[digest[i]&15];
    }
    hex[32]=0;
}
static void block(uint32_t state[4], const uint8_t input[64]) {
    uint32_t words[16];
    for (unsigned i=0;i<16;++i) {
        words[i]=0;
        for (unsigned j=0;j<4;++j) words[i]|=(uint32_t)input[4*i+j]<<(8*j);
    }
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
    for (unsigned i=0;i<64;++i) {
        unsigned round=i/16,index; uint32_t value;
        switch (round) {
            case 0: value=(b&c)|(~b&d); index=i; break;
            case 1: value=(d&b)|(~d&c); index=(5*i+1)%16; break;
            case 2: value=b^c^d; index=(3*i+5)%16; break;
            default: value=c^(b|~d); index=(7*i)%16; break;
        }
        value+=a+constants[i]+words[index];
        unsigned shift=shifts[round*4+i%4];
        a=d; d=c; c=b; b+=(value<<shift)|(value>>(32-shift));
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
}
void vegas_md5(const void *data, size_t size, uint8_t digest[16]) {
    uint32_t state[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    const uint8_t *bytes=data;
    size_t remaining=size;
    while (remaining>=64) { block(state,bytes); bytes+=64; remaining-=64; }
    uint8_t tail[128]={0};
    memcpy(tail,bytes,remaining); tail[remaining]=0x80;
    size_t padded=remaining<56?64:128;
    uint64_t bits=(uint64_t)size*8;
    for (unsigned i=0;i<8;++i) tail[padded-8+i]=bits>>(8*i);
    block(state,tail); if (padded==128) block(state,tail+64);
    for (unsigned i=0;i<16;++i) digest[i]=state[i/4]>>(8*(i%4));
}
