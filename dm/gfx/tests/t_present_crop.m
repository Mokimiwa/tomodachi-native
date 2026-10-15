// 出圖裁切回歸：彩色8×8紋理，閉式RGBA期望；真實Metal渲染，不依賴遊戲或跨局確定性。
// Present crop regression: colored 8×8 texture, closed-form RGBA expectations; real Metal rendering, no dependence on the game or on cross-run determinism.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"
uint64_t g_t0_ns;   // 宿主 main.c 的進程起點（hle_nvn 逐幀 @t_s 用），單測替身 | process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
#ifdef CROP_LEGACY_HEADER
#include CROP_LEGACY_HEADER
#define ENCODE legacy_present_encode
#else
#define ENCODE present_encode
#endif
static int failed, cases;
#define EXPECT(n, w, v) do { int a=(w),b=(v); cases++; printf("%s 期望 %d → 真結果 %d %s\n",n,a,b,a==b?"✓":"✗");failed+=a!=b; } while(0)
static const uint8_t colors[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255}};

static void check(id<MTLTexture> src, const char *name, const int crop[4], int w, int h, int color) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> dst=[g_dev newTextureWithDescriptor:d];id<MTLCommandBuffer> cb=[g_q commandBuffer];
    EXPECT("編碼成功",1,ENCODE(cb,src,dst,crop));[cb commit];[cb waitUntilCompleted];
    EXPECT("GPU完成",MTLCommandBufferStatusCompleted,cb.status);
    uint8_t *out=malloc(w*h*4);[dst getBytes:out bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    int bad=0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        int q=color;
        if(q==4) { // 正方形有效區對2:1目標：兩側各4列黑，中間8列全紅。 | square valid area onto a 2:1 target: 4 black columns on each side, the middle 8 columns all red.
            static const uint8_t black[4]={0,0,0,255};
            const uint8_t *want=x<4||x>=12?black:colors[0];bad+=memcmp(out+4*(y*w+x),want,4)!=0;continue;
        }
        if(q==-2) { // crop=(1,1,4,4)：3×3紅、末列綠、末行藍、右下1格黃。 | crop=(1,1,4,4): 3×3 red, last column green, last row blue, bottom-right 1 cell yellow.
            q=(x+1>=4)+2*(y+1>=4);
        } else if(q<0) { // 同尺寸默認整幀：分界在正中。 | same-size default whole frame: the boundary is dead center.
            q=(x>=w/2)+2*(y>=h/2);
        }
        bad+=memcmp(out+4*(y*w+x),colors[q],4)!=0;
    }
    EXPECT(name,0,bad);free(out);
}

int main(void) { @autoreleasepool {
    mtl_init();
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> src=[g_dev newTextureWithDescriptor:d];uint8_t px[8*8*4];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)memcpy(px+4*(y*8+x),colors[(x>=4)+2*(y>=4)],4);
    [src replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:px bytesPerRow:32];
    int zero[4]={0},tl[4]={0,0,4,4},tr[4]={4,0,4,4},bl[4]={0,4,4,4},br[4]={4,4,4,4},one[4]={3,3,1,1},inset[4]={1,1,4,4};
    check(src,"默認整幀64像素",zero,8,8,-1);
    check(src,"NULL默認整幀64像素",NULL,8,8,-1);
    check(src,"左上裁切放大256像素全紅",tl,16,16,0);
    check(src,"非零x裁切放大256像素全綠",tr,16,16,1);
    check(src,"非零y裁切放大256像素全藍",bl,16,16,2);
    check(src,"右下裁切貼邊256像素全黃",br,16,16,3);
    check(src,"單紋素放大不跨邊界256像素全紅",one,16,16,0);
    check(src,"非零雙偏移16像素逐字節等手寫子矩形",inset,4,4,-2);
    check(src,"有效區比例保留，128像素中間紅/兩側黑",tl,16,8,4);
#ifndef CROP_LEGACY_HEADER
    {   // sRGB 交換鏈：出圖送原始字節（Switch 掃描輸出口徑），不先解碼；不修時 128/64/200 解成約 55/13/148
        // sRGB swapchain: present sends the raw bytes (as the Switch's scan-out does) without decoding first; unfixed, 128/64/200 decode to about 55/13/148
        MTLTextureDescriptor *sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:8 height:8 mipmapped:NO];sd.storageMode=MTLStorageModeShared;sd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> s8=[g_dev newTextureWithDescriptor:sd];uint8_t g[8*8*4];
        for(int i=0;i<64;i++){g[4*i]=128;g[4*i+1]=64;g[4*i+2]=200;g[4*i+3]=255;}
        [s8 replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:g bytesPerRow:32];
        MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:od];id<MTLCommandBuffer> cb=[g_q commandBuffer];
        EXPECT("sRGB源編碼成功",1,ENCODE(cb,s8,dst,zero));[cb commit];[cb waitUntilCompleted];
        uint8_t o[8*8*4];[dst getBytes:o bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
        int bad=0;for(int i=0;i<64;i++)bad+=abs(o[4*i]-128)>1||abs(o[4*i+1]-64)>1||abs(o[4*i+2]-200)>1;
        EXPECT("sRGB交換鏈出圖送原始字節64像素",0,bad);
    }
#endif
    int invalid[4][4]={{-1,0,4,4},{0,-1,4,4},{0,0,0,4},{7,0,4,4}};
    for(int i=0;i<4;i++) {
        MTLTextureDescriptor *dd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];dd.usage=MTLTextureUsageRenderTarget;dd.storageMode=MTLStorageModeShared;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:dd];
        EXPECT("無效crop不編碼、不把整張拉伸冒充成功",0,ENCODE([g_q commandBuffer],src,dst,invalid[i]));
    }
    // 真實QueuePresent入口：先設crop入隊，再立即改window，出圖票號必須保留當幀副本。
    // Real QueuePresent entry: set crop and enqueue first, then immediately change the window; the present ticket must keep that frame's copy.
    PFNNVNDEVICEGETPROCADDRESSPROC get=(PFNNVNDEVICEGETPROCADDRESSPROC)h_nvnBootstrapLoader("nvnDeviceGetProcAddress");
    NVNT_nvnWindowSetCrop set=(NVNT_nvnWindowSetCrop)get(NULL,"nvnWindowSetCrop");
    NVNT_nvnQueuePresentTexture present=(NVNT_nvnQueuePresentTexture)get(NULL,"nvnQueuePresentTexture");
    tex_t tex={.magic=MAGIC('T', 'E', 'X', 0), .serial =10001};tex.d.width=tex.d.height=8;tex.d.depth=tex.d.levels=1;tex.d.format=NVN_FORMAT_RGBA8;tex.d.target=NVN_TEXTURE_TARGET_2D;serial_put(tex.serial,src);
    win_t win={.ntex=1};win.tex[0]=(NVNtexture *)&tex;set((NVNwindow *)&win,4,4,4,4);
    mtl_present_test_stall(100000000);present(NULL,(NVNwindow *)&win,0);uint64_t tk=win.ticket[0];set((NVNwindow *)&win,0,0,8,8);
#ifdef CROP_LEGACY_HEADER
    EXPECT("舊隊列結構沒有四參數副本空間（結構檢查）",1,sizeof(pq_item)>=sizeof(tex_t)+sizeof(double)+sizeof(uint64_t)+sizeof br);
#else
    pthread_mutex_lock(&g_pq_mu);int saved=memcmp(g_pq[(tk-1)%PQ].crop,br,sizeof br)==0;pthread_mutex_unlock(&g_pq_mu);
    EXPECT("真實QueuePresent當幀crop副本不隨window變化",1,saved);
#endif
    mtl_present_wait(tk);mtl_present_test_stall(0);
    printf("== %d 項，失敗 %d ==\n",cases,failed);return failed!=0;
} }
