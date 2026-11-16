/* dump_decs.c - emit the engine's real decs ,dec bplist to a file, via the
 * actual aw_engine_deliver(decs) path, so it can be parsed with an independent
 * CF-equivalent reader (python plistlib). Debug tool. L-VIDEO-IMPL. */
#include "aw_session.h"
#include "aw_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_reply[4096]; static size_t g_len;
static void emit(void *c, const uint8_t *m, size_t n){ (void)c; if(n<=sizeof g_reply){memcpy(g_reply,m,n);g_len=n;} }

static int q(void *c, vtpv_caps *o, size_t cap){
    (void)c; vtpv_codec_cap *cc=(vtpv_codec_cap*)((uint8_t*)o+sizeof(*o));
    if(cap<sizeof(*o)+2*sizeof(*cc)) return -1;
    o->magic=VTPV_CAPS_MAGIC;o->abi=VTPV_ABI;o->n_codecs=2;
    cc[0].codec=VTPV_CODEC_H264;cc[0].can_decode=1;cc[0].can_encode=0;cc[0].reserved=0;cc[0].max_width=4096;cc[0].max_height=4096;cc[0].out_pixfmt=VTPV_PIXFMT_NV12;
    cc[1].codec=VTPV_CODEC_HEVC;cc[1].can_decode=1;cc[1].can_encode=0;cc[1].reserved=0;cc[1].max_width=8192;cc[1].max_height=8192;cc[1].out_pixfmt=VTPV_PIXFMT_NV12;
    return 2;
}
static const vtpv_decoder_backend BE={ q, NULL, NULL, NULL };

int main(int argc, char**argv){
    aw_session_cfg cfg; memset(&cfg,0,sizeof cfg);
    cfg.be=&BE; cfg.emit=emit;
    aw_engine *e=aw_engine_create(&cfg);
    uint8_t m[256]; aw_builder b; aw_builder_init(&b,m,sizeof m);
    aw_build_header(&b,AW_MSG_DECS,AW_FLAG_GUEST_REQ,NULL,0x64);
    aw_put_u32(&b,AW_KEY_VER,5);          /* the real 26.2 guest sends ,ver 5 */
    size_t ml=aw_build_finalize(&b);
    int32_t st=aw_engine_deliver(e,m,ml);
    aw_msg_view v; aw_parse(g_reply,g_len,&v);
    const uint8_t *pls; size_t pl;
    int r=aw_get_bytes(&v,AW_KEY_DEC,AW_VT_PLS,&pls,&pl);
    fprintf(stderr,"decs status=%d reply=%zuB ,dec r=%d pls=%zuB\n",st,g_len,r,pl);
    if(r==0 && argc>1){ FILE*f=fopen(argv[1],"wb"); fwrite(pls,1,pl,f); fclose(f); }
    if(argc>2){ FILE*f=fopen(argv[2],"wb"); fwrite(g_reply,1,g_len,f); fclose(f);
        fprintf(stderr,"full reply=%zuB written; vtpv atom_size field=%u\n", g_len,
            (unsigned)((g_reply[0]<<24)|(g_reply[1]<<16)|(g_reply[2]<<8)|g_reply[3])); }
    aw_engine_destroy(e);
    return 0;
}
