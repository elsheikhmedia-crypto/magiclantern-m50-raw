#include "dryos.h"
#include "module.h"
#include "lens.h"
#include "raw.h"
#include "edmac.h"

struct TwoInTwoOutLosslessPath_args
{ 
  uint32_t * ResLockKey;
  uint32_t   ResLockKeySize;
  uint32_t * engio_cmd_1_prepare;
  uint32_t * engio_cmd_2_prepare;
  uint32_t * engio_cmd_pre_jpcore;
  uint32_t * engio_cmd_post_jpcore;
  uint32_t * engio_cmd_stop;
  uint32_t * engio_cmd_6_prepare;
  uint32_t * engio_cmd_7_prepare;
  uint32_t * engio_cmd_8_HIV_PPR;
  uint16_t * PPR_table;
  uint32_t * goes_to_obinteg_C0F0E000_size_0x188;
  uint32_t * goes_to_C0F21000_size_0x7E0;
  uint32_t * LuckyEnable_goes_to_C0F20000_size_0x1000;
  uint16_t   LuckyEnable;
  uint16_t   xRes;
  uint16_t   yRes;
  uint16_t   off_0x3E;
  uint32_t   SamplePrecision;
  uint32_t   off_0x44;
  uint32_t   RD1_Channel;
  uint32_t   RD1_StartFlags;
  uint32_t   RD1_Flags;
  uint32_t   RD1_Connection;
  void *     RD1_Address;
  uint32_t   RD1_Offset;
  uint32_t   RD2_Channel;
  uint32_t   RD2_StartFlags;
  uint32_t   RD2_Flags;
  uint32_t   RD2_Connection;
  void *     RD2_Address;
  uint32_t   RD2_Offset;
  uint32_t   WR1_Channel;
  uint32_t   WR1_StartFlags;
  uint32_t   WR1_Flags;
  uint32_t   WR1_Connection;
  struct memSuite * WR1_MemSuite;
  uint32_t   off_0x8C;
  uint32_t   WR2_Channel;
  uint32_t   WR2_StartFlags;
  uint32_t   WR2_Flags;
  uint32_t   WR2_Connection;
  void *     WR2_Address;
  uint32_t   WR2_Offset;
};

static struct TwoInTwoOutLosslessPath_args TTL_Args;
static struct LockEntry * TTL_ResLock = 0;
static struct semaphore * lossless_sem = 0;
static int verbose = 0;

static void LosslessCompleteCBR()
{
//    DryosDebugMsg(0, 0, "LosslessCompleteCBR\n");
    give_semaphore(lossless_sem);
}

/* pack two 16-bit values into a 32-bit one */
#define PACK32(lo,hi) (((uint32_t)(lo) & 0xFFFF) | ((uint32_t)(hi) << 16))

static void (*TTL_SetArgs)(int unused, void * TTL_Args, int PictureSize) = NULL;
static void (*TTL_Prepare)(void * ResLock, void * TTL_Args) = NULL;
static void (*TTL_RegisterCBR)(int id, void * cbr, void * cbr_arg) = NULL;
static void (*TTL_SetFlags)(int PictureType) = NULL;
static void (*TTL_Start)(void * TTL_Args) = NULL;
static void (*TTL_Stop)(void * TTL_Args) = NULL;
static void (*TTL_Finish)(void * ResLock, void * TTL_Args, uint32_t * output_size) = NULL;

/* edmac-memcpy.c */
extern uint32_t edmac_write_chan;
extern uint32_t edmac_read_chan;

/* ------------------------------------------------------------------------------------------
 * Canon EOS M50 1.1.0 (DIGIC 8): no JPCORE/LJ92 path. Lossless RAW = Canon's own CR3 (CRX)
 * encoder, the way rawdump proved it bit-exact (raw_tests/crxenc3, crx16_2, crxmem1):
 *   LV raw (14-bit pixblock) --EDMAC ch61->ch24, unpack {0,2,0,0} / pack {1,1,2,0,0}, mode 0-->
 *   u16 samples = pixel << 2 (6.6 ms / 2096x832) --ALGSMGR_StartEncode(type 1 = Id 1 = CtMode 0,
 *   levels 0, bit depth 16)--> cbr(ctx, handle, 0x24) --GetRingList--> [112 B header, 4 planes]
 *   --EDMAC plain copy--> output at 64-byte aligned offsets --> ALGSMGR_FreeEncBuffer.
 * Output frame layout: CRX mdat header (FF01 tile / FF02 plane / FF03 subband, 112 B) padded
 * to 64 B, then each plane (size = FF02 compSize) padded to 64 B. PC: tools/crx/crx_decode.py
 * (nBits 16, cfa 1, then >> 2). ALGSMGR keeps its own output buffers (2 x 44.7 MB, outside the
 * shoot memory: crxmem1), 48 frames at ~22 ms/frame while holding all shoot memory.
 * ROM: ALGSMGR_StartEncode 0xe0795850, GetRingList 0xe0795ca8, FreeEncBuffer 0xe0795d2e,
 * pAlgsMgrData *(0x5a20), EncInit-done flags am+0x108+Id*4, m2m5 0xE084DE9C.., pack
 * 0xE054B99A (write) / 0xE054BA44 (read), res lock 0xE055819E.., PwrMng 0xE0557ECE/EFC.
 * ------------------------------------------------------------------------------------------ */
struct m50_edmac_info
{
    uint32_t off1s, off1a, off1b, off2s, off2a, off2b, off3;
    uint32_t xs, xa, xb, ys, ya, yb, xn, yn;
};
typedef void *(*m50_create_lock_t)(uint32_t *ids, uint32_t count);
typedef unsigned int (*m50_lock_op_t)(void *entry);
typedef void (*m50_subchips_t)(const uint32_t *list);
typedef void (*m50_pack_t)(int ch, uint32_t *cfg);
typedef void (*m50_v_t)(void);
#define M50_CreateResLockEntry   ((m50_create_lock_t) (0xE055819E | 1))
#define M50_LockEngineResources  ((m50_lock_op_t)     (0xE0558386 | 1))
#define M50_UnLockEngineResources ((m50_lock_op_t)    (0xE0558426 | 1))
#define M50_DeleteResLockEntry   ((m50_lock_op_t)     (0xE0558282 | 1))
#define M50_PwrMngEnableSubChip  ((m50_subchips_t)    (0xE0557ECE | 1))
#define M50_PwrMngDisableSubChip ((m50_subchips_t)    (0xE0557EFC | 1))
#define M50_EdmacSetPack         ((m50_pack_t)        (0xE054B99A | 1))
#define M50_EdmacSetUnpack       ((m50_pack_t)        (0xE054BA44 | 1))
#define M50_m2m5_Setup           ((void (*)(uint32_t *))            (0xE084DE9C | 1))
#define M50_m2m5_RegisterCBR     ((void (*)(void (*)(void *), void *)) (0xE084DF40 | 1))
#define M50_m2m5_ResetCBR        ((m50_v_t)                         (0xE084DF4A | 1))
#define M50_m2m5_ClearEdmac      ((m50_v_t)                         (0xE084DF26 | 1))
#define M50_m2m5_SetAddress      ((void (*)(void **))               (0xE084DF70 | 1))
#define M50_m2m5_SetSize         ((void (*)(void *))                (0xE084DF8A | 1))
#define M50_m2m5_Start           ((m50_v_t)                         (0xE084DF56 | 1))
#define M50_StartEncode  ((uint32_t (*)(uint32_t *, int, void (*)(void *, uint32_t, uint32_t), void *)) 0xe0795851)
#define M50_GetRingList  ((uint32_t (*)(uint32_t, int *, uint32_t **, uint32_t **)) 0xe0795ca9)
#define M50_FreeEncBuffer ((uint32_t (*)(uint32_t, int, int)) 0xe0795d2f)
#define M50_PACK_RD61    0xD0487D00
#define M50_PACK_WR24    0xD0487900
#define M50_PORT_RD61    0xD0487600
#define M50_PORT_WR24    0xD0487100
#define M50_ENC_ID       1
#define M50_ENC_TYPE     1
#define M50_IN_SMALL     (2100 * 1200 * 2)   /* 16-bit encoder input, up to the movie LV raw size */
#define M50_IN_BIG       (4088 * 1168 * 2)   /* photo LV x5 readout (4200x1168), one CRX tile (width <= 4088) */
#define M50_ONE_TILE_W   4088                /* wider = 2 CRX tiles: untested, and mlvcrx2raw reads one tile */

static int m50_mode = 0;                     /* 1 = M50 CRX backend active */
static void *m50_in_raw = 0, *m50_in = 0;
static int m50_in_cap = 0;                   /* bytes usable at m50_in */
static void *m50_lock = 0;
static uint32_t m50_saved_pack[4];
static struct semaphore *m50_dma_sem = 0, *m50_enc_sem = 0;
static volatile int m50_dma_done = 0;
static volatile uint32_t m50_enc_done = 0, m50_enc_handle = 0, m50_enc_late = 0;
static int m50_broken = 0;                   /* a timeout happened: never reuse the input buffer */
int lossless_m50_last_us[4];                 /* edmac16, encode, copy, total (for logs) */

extern int lossless_m50_mode;
static int m50_ptr_ok(uint32_t p)
{
    return !(p & 3) && ((p >= 0x1000 && p < 0x40000000) || (p >= 0x40001000 && p < 0x80000000));
}

static void m50_dma_cbr(void *arg)
{
    m50_dma_done = 1;
    give_semaphore(m50_dma_sem);
}

static void m50_enc_cbr(void *ctx, uint32_t handle, uint32_t flag)
{
    m50_enc_handle = handle;
    m50_enc_done = 1;
    if (m50_enc_late)
    {
        if (handle && !(handle & 1)) M50_FreeEncBuffer(handle, 0, 0);
        return;
    }
    give_semaphore(m50_enc_sem);
}

static void m50_restore_pack()
{
    MEM(M50_PACK_RD61 + 0x24) = m50_saved_pack[0];
    MEM(M50_PORT_RD61 + 0xD0) = m50_saved_pack[1];
    MEM(M50_PACK_WR24 + 0x24) = m50_saved_pack[2];
    MEM(M50_PORT_WR24 + 0xD0) = m50_saved_pack[3];
}

/* one EDMAC mem2mem copy (task context, needs the lock); 0 = ok */
static int m50_m2m(void *src, void *dst, struct m50_edmac_info *si, struct m50_edmac_info *di, int pack16)
{
    static uint32_t chans[2] = { 0x3D, 0x18 };
    static uint32_t size_arg[3];
    static void *addr_arg[2];
    static uint32_t unpack14[4] = { 0, 2, 0, 0 };
    static uint32_t pack16cfg[5] = { 1, 1, 2, 0, 0 };
    /* crxd2: {1,1,1,0,0} gives 16-bit samples = pixel >> 2 (12-bit, exact); {1,1,2,0,0} = pixel << 2 */
    pack16cfg[2] = (lossless_m50_mode == 0) ? 2 : 1;
    size_arg[0] = (uint32_t) si; size_arg[1] = (uint32_t) di; size_arg[2] = 0;
    addr_arg[0] = UNCACHEABLE(src); addr_arg[1] = UNCACHEABLE(dst);
    while (take_semaphore_now(m50_dma_sem) == 0) ;   /* no wait: run 1 lost ~6 ticks per frame here */
    m50_dma_done = 0;
    M50_m2m5_Setup(chans);
    M50_m2m5_RegisterCBR(m50_dma_cbr, 0);
    M50_m2m5_SetAddress(addr_arg);
    M50_m2m5_SetSize(size_arg);
    if (pack16)
    {
        M50_EdmacSetUnpack(0x3D, unpack14);
        M50_EdmacSetPack(0x18, pack16cfg);
    }
    else
    {
        m50_restore_pack();     /* plain copy: Canon's default pack state */
    }
    M50_m2m5_Start();
    take_semaphore(m50_dma_sem, 500);
    M50_m2m5_ResetCBR();
    M50_m2m5_ClearEdmac();
    return m50_dma_done ? 0 : -1;
}

static int m50_copy_chunk(void *src, void *dst, int size)
{
    static struct m50_edmac_info si, di;
    int rows = size / 4096, tail = size % 4096;
    if (rows)
    {
        memset(&si, 0, sizeof(si)); memset(&di, 0, sizeof(di));
        si.xb = di.xb = 4096; si.yb = di.yb = rows - 1;
        if (m50_m2m(src, dst, &si, &di, 0)) return -1;
    }
    if (tail) memcpy(UNCACHEABLE((uint8_t *) dst + rows * 4096), UNCACHEABLE((uint8_t *) src + rows * 4096), tail);
    return 0;
}

/* CRX rounded bits (raw_tests/crxr1): Id block +0x214+plane*4 goes into the plane header and the
 * encoder register on every frame (FUN_e02aa8e6, FUN_e02aae50). Our samples are pixel << 2, so
 * field 1 (step 2, error <= 1) is still exact: pixel = (v + 1) >> 2, 0 wrong of 1743872, 11 %
 * smaller. Field 2 is NOT exact (ties, 12 % of the pixels off by 1). algsmgrEncInit writes 0 here,
 * so the first frame of a take stays field 0. Skipped unless the block holds our size. */
/* 1 = exact (default, "14-bit lossless"); 3 = "13-bit compact": step 8 on pixel << 2, pixel error
 * at most 1 in 14-bit units, about 26 % smaller (raw_tests/crxr2). Set by mlv_lite at REC start. */
int lossless_m50_round_field = 1;
/* 0 = 14-bit lossless (pixel << 2, field 1), 1 = 12-bit lossless (pixel >> 2, field 0, exact 12-bit),
 * 2 = 10-bit (pixel >> 2 with field 2: error <= 2 in 12-bit units, like 10-bit rounding). Set at REC start. */
int lossless_m50_mode = 0;
static int m50_last_w = 0, m50_last_h = 0;

static void m50_set_rounded(int width, int height, int field)
{
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    uint32_t cg = *(volatile uint32_t *) 0x111b8;
    if (!m50_ptr_ok(am) || !m50_ptr_ok(cg)) return;
    if (((volatile uint32_t *) (am + 0x108))[M50_ENC_ID] != 1) return;   /* EncInit still to run */
    uint32_t blk = ((volatile uint32_t *) cg)[0x28 / 4];
    if (!m50_ptr_ok(blk)) return;
    uint32_t idb = blk + M50_ENC_ID * 0x288;
    if (((volatile uint16_t *) (idb + 0x200))[0] != width || ((volatile uint16_t *) (idb + 0x200))[1] != height) return;
    volatile uint32_t *r = (volatile uint32_t *) (idb + 0x214);
    for (int p = 0; p < 4; p++) if (r[p] > 3) return;
    for (int p = 0; p < 4; p++) r[p] = field;
}

static int m50_init()
{
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    if (!m50_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f) return 0;   /* ALGSMGR_Initialize(0x3f, ..) at boot */
    m50_dma_sem = create_named_semaphore("lossless_m50_dma", 0);
    m50_enc_sem = create_named_semaphore("lossless_m50_enc", 0);
    if (!m50_dma_sem || !m50_enc_sem) return 0;
    m50_mode = 1;
    return 1;
}

/* the 16-bit encoder input buffer (5 MB) is taken only when lossless is selected, before
 * mlv_lite holds the shoot memory, so uncompressed recording keeps all of it */
int lossless_m50_prealloc()
{
    if (!m50_mode) return 1;
    if (!m50_in_raw)
    {
        /* big enough for the x5 readout; if that much is not free, the old size (Open Gate still works) */
        m50_in_raw = fio_malloc(M50_IN_BIG + 128);
        m50_in_cap = M50_IN_BIG;
        if (!m50_in_raw) { m50_in_raw = fio_malloc(M50_IN_SMALL + 128); m50_in_cap = M50_IN_SMALL; }
        if (!m50_in_raw) m50_in_cap = 0;
        printf("CRX input buffer: %d bytes at %x\n", m50_in_cap, m50_in_raw);
    }
    if (m50_in_raw) m50_in = (void *)(((uintptr_t) m50_in_raw + 63) & ~63);
    return m50_in != 0;
}

int lossless_ready()
{
    return !m50_mode || m50_in != 0;
}

/* READ-ONLY test (2026-11-07): can ALGSMGR's second output buffer become extra recording memory?
 * am+0x154 / am+0x1e4 are two output rings of am+0x274 bytes each (raw_tests/crxmem1). Per take this
 * counts where the encoder's chunks land and compares a sparse sample of both rings at take start
 * and stop. Nothing here writes to Canon's memory. */
#define M50_RING_STEP 0x10000
#define M50_RING_SAMPLES 1024
static uint32_t m50_ring_base[2], m50_ring_size;
static uint32_t m50_ring_n[3], m50_ring_min[3], m50_ring_max[3];
static uint32_t m50_ring_sum[2][M50_RING_SAMPLES];
static int m50_ring_ok = 0, m50_ring_frames = 0;
static int m50_extra_in_use = 0;   /* our frame slots live in ring0 / ring1 during this take */

static uint32_t m50_alias(uint32_t a) { return a & 0x3FFFFFFF; }   /* cached and uncached views */

static uint32_t m50_ring_block_sum(uint32_t base, uint32_t off)
{
    volatile uint32_t *w = (volatile uint32_t *) UNCACHEABLE(base + off);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum = (sum << 1 | sum >> 31) + w[i];
    return sum;
}

static void m50_ring_reset_counts()
{
    for (int i = 0; i < 3; i++) { m50_ring_n[i] = 0; m50_ring_min[i] = 0xFFFFFFFF; m50_ring_max[i] = 0; }
}

static void m50_ring_sample()
{
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    if (!m50_ptr_ok(am)) return;
    m50_ring_base[0] = *(volatile uint32_t *) (am + 0x154);
    m50_ring_base[1] = *(volatile uint32_t *) (am + 0x1e4);
    m50_ring_size = *(volatile uint32_t *) (am + 0x274);
    printf("CRX rings: %x %x size %x count %d/%d\n", m50_ring_base[0], m50_ring_base[1], m50_ring_size,
        *(volatile uint32_t *) (am + 0x394), *(volatile uint32_t *) (am + 0x398));
    /* only the layout measured in crxmem1: 0x2ab1000 per ring */
    if (m50_ring_size != 0x2ab1000 || !m50_ptr_ok(m50_ring_base[0]) || !m50_ptr_ok(m50_ring_base[1])) return;
    for (int r = 0; r < 2; r++)
        for (uint32_t k = 0; k < M50_RING_SAMPLES && k * M50_RING_STEP + 64 <= m50_ring_size; k++)
            m50_ring_sum[r][k] = m50_ring_block_sum(m50_ring_base[r], k * M50_RING_STEP);
    m50_ring_ok = 1;
    m50_ring_reset_counts();   /* count from here, when the ring addresses are known */
    printf("CRX rings sampled at frame %d\n", m50_ring_frames);
}

static void m50_ring_start()
{
    m50_ring_ok = 0; m50_ring_frames = 0;
    m50_ring_base[0] = m50_ring_base[1] = m50_ring_size = 0;
    m50_ring_reset_counts();
    m50_ring_sample();
}

static void m50_ring_note(uint32_t a)
{
    int w = 2;
    uint32_t x = m50_alias(a);
    for (int r = 0; r < 2; r++)
    {
        uint32_t b = m50_alias(m50_ring_base[r]);
        if (m50_ring_size && x >= b && x < b + m50_ring_size) w = r;
    }
    m50_ring_n[w]++;
    if (x < m50_ring_min[w]) m50_ring_min[w] = x;
    if (x > m50_ring_max[w]) m50_ring_max[w] = x;
}

static void m50_ring_stop()
{
    static const char *names[3] = { "ring0", "ring1", "other" };
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    printf("CRX ring use, %d frames:\n", m50_ring_frames);
    if (m50_ptr_ok(am))
        printf("  now: %x %x size %x count %d/%d\n", *(volatile uint32_t *) (am + 0x154), *(volatile uint32_t *) (am + 0x1e4),
            *(volatile uint32_t *) (am + 0x274), *(volatile uint32_t *) (am + 0x394), *(volatile uint32_t *) (am + 0x398));
    for (int i = 0; i < 3; i++)
        printf("  %s: %d chunks, %x..%x\n", names[i], m50_ring_n[i], m50_ring_n[i] ? m50_ring_min[i] : 0, m50_ring_max[i]);
    if (!m50_ring_ok) { printf("  ring samples skipped\n"); return; }
    for (int r = 0; r < 2; r++)
    {
        int total = 0, changed = 0;
        for (uint32_t k = 0; k < M50_RING_SAMPLES && k * M50_RING_STEP + 64 <= m50_ring_size; k++)
        {
            total++;
            if (m50_ring_sum[r][k] != m50_ring_block_sum(m50_ring_base[r], k * M50_RING_STEP)) changed++;
        }
        printf("  %s samples changed: %d of %d\n", names[r], changed, total);
    }
}

/* recording start/stop (mlv_lite compress_task INT_MAX / INT_MIN) */
void lossless_rec_start()
{
    if (!m50_mode || m50_lock) return;
    if (m50_broken && m50_enc_late && m50_enc_done)
    {
        /* the late cbr arrived and freed Canon's buffer: the encoder no longer reads m50_in */
        m50_broken = 0;
        m50_enc_late = 0;
        printf("CRX: encoder finished late, enabled again\n");
    }
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    int t0 = get_ms_clock();
    void *lock = M50_CreateResLockEntry(res_ids, 2);
    if (!lock) { printf("CRX: no lock entry\n"); return; }
    if (M50_LockEngineResources(lock) & 1) { M50_DeleteResLockEntry(lock); printf("CRX: lock failed\n"); return; }
    printf("CRX rec start: lock %d ms\n", get_ms_clock() - t0);
    M50_PwrMngEnableSubChip(subchips);
    m50_saved_pack[0] = MEM(M50_PACK_RD61 + 0x24); m50_saved_pack[1] = MEM(M50_PORT_RD61 + 0xD0);
    m50_saved_pack[2] = MEM(M50_PACK_WR24 + 0x24); m50_saved_pack[3] = MEM(M50_PORT_WR24 + 0xD0);
    m50_lock = lock;
    m50_ring_start();
    /* set up encoder Id 1 again with our size at the first frame (EncInit runs when the flag is 0) */
    if (m50_ptr_ok(am)) ((volatile uint32_t *) (am + 0x108))[M50_ENC_ID] = 0;
}

void lossless_rec_stop()
{
    if (!m50_mode || !m50_lock) return;
    static const uint32_t subchips[2] = { 4, 7 };
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    m50_restore_pack();
    if (m50_last_w) { m50_set_rounded(m50_last_w, m50_last_h, 0); m50_last_w = 0; }
    M50_PwrMngDisableSubChip(subchips);
    M50_UnLockEngineResources(m50_lock);
    M50_DeleteResLockEntry(m50_lock);
    m50_lock = 0;
    printf("CRX rec stop\n");
    m50_ring_stop();
    m50_extra_in_use = 0;
    /* Canon re-initializes Id 1 with its own size at its next photo */
    if (m50_ptr_ok(am)) ((volatile uint32_t *) (am + 0x108))[M50_ENC_ID] = 0;
}

/* ALGSMGR gets its output buffers at the first encode after boot (am+0x394 = 0 before it,
 * raw_tests/crxrec10), so encode one empty frame before asking where they are. Same conditions as a
 * normal first frame: shoot memory already held. Id 1 is set up again at the real first frame. */
static int m50_warm_encode(int width, int height)
{
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    if (!m50_ptr_ok(am)) return 0;
    memset(UNCACHEABLE(m50_in), 0, width * height * 2);
    ((volatile uint32_t *) (am + 0x108))[M50_ENC_ID] = 0;
    uint32_t entry[8] = { M50_ENC_TYPE, width, height, 0, 0, 16, (uint32_t) UNCACHEABLE(m50_in), width };
    while (take_semaphore_now(m50_enc_sem) == 0) ;
    m50_enc_done = 0; m50_enc_handle = 0; m50_enc_late = 0;
    uint32_t t0 = get_us_clock();
    if (M50_StartEncode(entry, 1, m50_enc_cbr, (void *) 0x4d353043) & 1) { printf("CRX warm encode: start failed\n"); return 0; }
    take_semaphore(m50_enc_sem, 2000);
    if (!m50_enc_done)
    {
        m50_enc_late = 1;
        m50_broken = 1;
        printf("CRX warm encode: timeout\n");
        return 0;
    }
    uint32_t h = m50_enc_handle;
    if (h && !(h & 1)) M50_FreeEncBuffer(h, 0, 0);
    ((volatile uint32_t *) (am + 0x108))[M50_ENC_ID] = 0;
    printf("CRX warm encode: %d us\n", (int)(get_us_clock() - t0));
    return 1;
}

/* Extra recording memory: ring0 and ring1 of ALGSMGR. In 6 logged takes the encoder's chunks never landed
 * there and no sample changed (raw_tests/crxrec11, its chunks sit right after each ring).
 * Returns how many regions were filled in (0 = use the shoot memory only). */
int lossless_m50_extra_regions(int width, int height, uint32_t *addr, int *size)
{
    m50_extra_in_use = 0;
    if (!m50_mode || m50_broken || !m50_in || is_movie_mode()) return 0;
    if ((width & 7) || (height & 1) || width < 64 || height < 64 || width > M50_ONE_TILE_W || width * height * 2 > m50_in_cap) return 0;
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    if (!m50_ptr_ok(am)) return 0;
    if (*(volatile uint32_t *) (am + 0x394) == 0 && !m50_warm_encode(width, height)) return 0;
    uint32_t r0 = *(volatile uint32_t *) (am + 0x154), r1 = *(volatile uint32_t *) (am + 0x1e4);
    uint32_t sz = *(volatile uint32_t *) (am + 0x274), n = *(volatile uint32_t *) (am + 0x394);
    /* only the exact layout that was measured */
    if (r0 != 0x47997c00 || r1 != 0x7492dc00 || sz != 0x2ab1000 || n != 2)
    {
        printf("CRX extra buffer off: %x %x %x %d\n", r0, r1, sz, n);
        return 0;
    }
    addr[0] = r0 + 0x1000; size[0] = sz - 0x2000;
    addr[1] = r1 + 0x1000; size[1] = sz - 0x2000;
    m50_extra_in_use = 1;
    return 2;
}

static int m50_compress(struct memSuite *dst_suite, void *src, int src_width, int src_x, int src_y, int width, int height)
{
    static struct m50_edmac_info si, di;
    if (!m50_mode || m50_broken || !m50_in) return -10;
    if (is_movie_mode()) return -21;     /* movie mode: not supported yet, and must not break the encoder */
    int started_here = 0;
    if (!m50_lock) { lossless_rec_start(); started_here = 1; }
    if (!m50_lock) return -11;
    int ret = -12;
    if ((width & 7) || (src_x & 7) || (height & 1) || width < 64 || height < 64 || width * height * 2 > m50_in_cap)
        goto out;
    if (width > M50_ONE_TILE_W) { ret = -22; goto out; }   /* 2 CRX tiles: not supported yet */

    uint8_t *out = GetMemoryAddressOfMemoryChunk(GetFirstChunkFromSuite(dst_suite));
    int out_max = GetSizeOfMemoryChunk(GetFirstChunkFromSuite(dst_suite)) - 128;   /* suite = slot size from out_ptr */
    uint32_t t0 = get_us_clock();

    /* 1. crop + 14 -> 16-bit samples (pixel << 2) */
    int pitch = src_width * 14 / 8;
    memset(&si, 0, sizeof(si)); memset(&di, 0, sizeof(di));
    si.xb = width * 14 / 8; si.off1b = pitch - si.xb; si.yb = height - 1;
    di.xb = width * 2; di.yb = height - 1;
    uint8_t *src_adj = (uint8_t *) src + src_y * pitch + src_x * 14 / 8;
    if (m50_m2m(src_adj, m50_in, &si, &di, 1)) { ret = -13; goto out; }
    m50_restore_pack();
    uint32_t t1 = get_us_clock();

    /* 2. encode */
    m50_set_rounded(width, height, lossless_m50_mode == 1 ? 0 : lossless_m50_mode == 2 ? 2 : (lossless_m50_round_field == 3) ? 3 : 1);
    m50_last_w = width; m50_last_h = height;
    uint32_t entry[8] = { M50_ENC_TYPE, width, height, 0, 0, 16, (uint32_t) UNCACHEABLE(m50_in), width };
    while (take_semaphore_now(m50_enc_sem) == 0) ;
    m50_enc_done = 0; m50_enc_handle = 0; m50_enc_late = 0;
    if (M50_StartEncode(entry, 1, m50_enc_cbr, (void *) 0x4d353043) & 1) { ret = -14; goto out; }
    take_semaphore(m50_enc_sem, 500);
    uint32_t t2 = get_us_clock();
    if (!m50_enc_done)
    {
        m50_enc_late = 1;               /* the late cbr frees Canon's buffer */
        m50_broken = 1;                 /* the encoder may still read m50_in: stop using it */
        ret = -15; goto out;
    }
    uint32_t h = m50_enc_handle;
    if (!h || (h & 1)) { ret = -16; goto out; }

    /* 3. output chunks -> dst, 64-byte aligned */
    int n = 0; uint32_t *addrs = 0, *sizes = 0;
    int total = 0;
    ret = -17;
    if (M50_GetRingList(h, &n, &addrs, &sizes) == 0 && n > 0 && n <= 64 && m50_ptr_ok((uint32_t) addrs) && m50_ptr_ok((uint32_t) sizes))
    {
        ret = 0;
        for (int i = 0; i < n; i++)
        {
            uint32_t a = addrs[i], s = sizes[i];
            if (!a || !s) continue;
            if (!m50_ptr_ok(a & ~3) || total + (int)((s + 63) & ~63) > out_max) { ret = -18; break; }
            m50_ring_note(a);
            if (m50_ring_frames == 0)
                printf("CRX f%d chunk %d: %x size %x\n", m50_ring_frames, i, a, s);
            if (m50_extra_in_use && m50_ring_size)
            {
                /* never measured (raw_tests/crxrec11: 0 chunks in the rings), but it would mean the
                 * encoder and our frame slots share memory: stop the take instead of recording garbage */
                uint32_t x = m50_alias(a), b0 = m50_alias(m50_ring_base[0]), b1 = m50_alias(m50_ring_base[1]);
                if ((x >= b0 && x < b0 + m50_ring_size) || (x >= b1 && x < b1 + m50_ring_size))
                {
                    printf("CRX: encoder output %x inside the extra buffer\n", a);
                    ret = -20; break;
                }
            }
            if (m50_copy_chunk((void *) a, out + total, s)) { ret = -19; break; }
            /* zero the alignment padding: stale bytes there could hold mlv_lite's 0xA5A5A5A5 frame
             * sentinel and trigger a false "Frame incomplete" (crxrec3: frames 46/623/1072 ...) */
            for (uint32_t q = s; q < ((s + 63) & ~63); q++)
                ((volatile uint8_t *) UNCACHEABLE(out + total))[q] = 0;
            total += (s + 63) & ~63;
        }
        if (ret == 0) ret = total;
    }
    m50_ring_frames++;
    if (!m50_ring_ok && m50_ring_frames == 1) m50_ring_sample();   /* once: it prints */
    M50_FreeEncBuffer(h, 0, 0);
    uint32_t t3 = get_us_clock();
    lossless_m50_last_us[0] = t1 - t0; lossless_m50_last_us[1] = t2 - t1;
    lossless_m50_last_us[2] = t3 - t2;
    lossless_m50_last_us[3] = lossless_m50_last_us[0] + lossless_m50_last_us[1] + lossless_m50_last_us[2];
out:
    if (started_here) lossless_rec_stop();
    return ret;
}

static void decompress_init();
int lossless_init()
{
    if (is_camera("M50", "1.1.0"))
    {
        /* CRX backend above; no LJ92 decoder either */
        return m50_init();
    }

    if (is_camera("5D3", "1.1.3"))
    {
        /* ProcessTwoInTwoOutLosslessPath, 5D3 1.1.3 */
        TTL_SetArgs     = (void *) 0xFF32330C;  /* fills TTL_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF3D4680;  /* called right after ProcessTwoInTwoOutLosslessPath(R) Start; */
                                                /* calls [TTL] GetPathResources and sets up the encoder for RAW/SRAW/MRAW */
        TTL_RegisterCBR = (void *) 0xFF3D3774;  /* RegisterTwoInTwoOutLosslessPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF32B418;  /* called next, with PictureType as arguments */
        TTL_Start       = (void *) 0xFF3D46F0;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF3D4728;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF3D4760;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }

    if (is_camera("5D3", "1.2.3"))
    {
        /* ProcessTwoInTwoOutLosslessPath, 5D3 1.2.3 */
        TTL_SetArgs     = (void *) 0xFF327DE8;  /* fills TTL_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF3DD574;  /* called right after ProcessTwoInTwoOutLosslessPath(R) Start; */
                                                /* calls [TTL] GetPathResources and sets up the encoder for RAW/SRAW/MRAW */
        TTL_RegisterCBR = (void *) 0xFF3DC668;  /* RegisterTwoInTwoOutLosslessPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF32FF2C;  /* called next, with PictureType as arguments */
        TTL_Start       = (void *) 0xFF3DD5E4;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF3DD61C;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF3DD654;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }

    if (is_camera("700D", "1.1.5"))
    {
        /* ProcessTwoInTwoOutJpegath, 700D 1.1.5 */
        TTL_SetArgs     = (void *) 0xFF35F510;  /* fills TTJ_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF424BA4;  /* called right after ProcessTwoInTwoOutJpegath(R) Start(%d); */
                                                /* calls [TTJ] GetPathResources and sets up the encoder for RAW */
        TTL_RegisterCBR = (void *) 0xFF423B88;  /* RegisterTwoInTwoOutJpegPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF36B2D8;  /* alternate StartTwoInTwoOutJpegPath http://www.magiclantern.fm/forum/index.php?topic=18443.msg188721#msg188721 */
        TTL_Start       = (void *) 0xFF424C4C;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF423DD4;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF424CBC;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }

    if (is_camera("650D", "1.0.4"))
    {
        /* ProcessTwoInTwoOutJpegath, 650D 1.0.4 */
        TTL_SetArgs     = (void *) 0xFF35C9C0;  /* fills TTJ_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF4210BC;  /* called right after ProcessTwoInTwoOutJpegath(R) Start(%d); */
                                                /* calls [TTJ] GetPathResources and sets up the encoder for RAW */
        TTL_RegisterCBR = (void *) 0xFF4200A0;  /* RegisterTwoInTwoOutJpegPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF368788;  /* alternate StartTwoInTwoOutJpegPath http://www.magiclantern.fm/forum/index.php?topic=18443.msg188721#msg188721 */
        TTL_Start       = (void *) 0xFF421164;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF4202EC;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF4211D4;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }

    if (is_camera("EOSM", "2.0.2"))
    {
        /* ProcessTwoInTwoOutJpegath, EOSM 2.0.2 */
        TTL_SetArgs     = (void *) 0xFF361498;  /* fills TTJ_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF429210;  /* called right after ProcessTwoInTwoOutJpegath(R) Start(%d); */
                                                /* calls [TTJ] GetPathResources and sets up the encoder for RAW */
        TTL_RegisterCBR = (void *) 0xFF4281F4;  /* RegisterTwoInTwoOutJpegPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF36D124;  /* called next, with PictureType as arguments */
        TTL_Start       = (void *) 0xFF4292B8;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF428440;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF429328;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }
    if (is_camera("100D", "1.0.1"))
    {
        /* ProcessTwoInTwoOutJpegath, 100D 1.0.1 */
        TTL_SetArgs     = (void *) 0xFF3647D0;  /* fills TTJ_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF42Bf8C;  /* called right after ProcessTwoInTwoOutJpegath(R) Start(%d); */
                                                /* calls [TTJ] GetPathResources and sets up the encoder for RAW */
        TTL_RegisterCBR = (void *) 0xFF42AF70;  /* RegisterTwoInTwoOutJpegPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF363148;  /* called next, with PictureType as arguments */ 
        TTL_Start       = (void *) 0xFF42c034;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF42B1BC;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF42C0A4;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }

    if (is_camera("6D", "1.1.6"))
    {
        /* ProcessTwoInTwoOutLosslessPath, 6D 1.1.6 */
        TTL_SetArgs     = (void *) 0xFF3491C8;  /* fills TTL_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF4129BC;  /* called right after ProcessTwoInTwoOutLosslessPath(R) Start; */
                                                /* calls [TTL] GetPathResources and sets up the encoder for RAW/SRAW/MRAW */
        TTL_RegisterCBR = (void *) 0xFF411A44;  /* RegisterTwoInTwoOutLosslessPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF359C78;  /* called next, with PictureType as arguments */
        TTL_Start       = (void *) 0xFF412A2C;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF412A64;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF412A9C;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }

    if (is_camera("70D", "1.1.2"))
    {
        /* ProcessTwoInTwoOutLosslessPath, 70D 1.1.2 */
        TTL_SetArgs     = (void *) 0xFF362174;  /* fills TTL_Args struct; PictureSize(Mem1ToRaw) */
        TTL_Prepare     = (void *) 0xFF438404;  /* called right after ProcessTwoInTwoOutLosslessPath(R) Start; */
                                                /* calls [TTL] GetPathResources and sets up the encoder for RAW/SRAW/MRAW */
        TTL_RegisterCBR = (void *) 0xFF43748C;  /* RegisterTwoInTwoOutLosslessPathCompleteCBR */
        TTL_SetFlags    = (void *) 0xFF372AE8;  /* called next, with PictureType as arguments */
        TTL_Start       = (void *) 0xFF438474;  /* called next; starts the EDmac transfers */
        TTL_Stop        = (void *) 0xFF4384AC;  /* called right after sssStopMem1ToRawPath */
        TTL_Finish      = (void *) 0xFF4384E4;  /* called next; calls UnlockEngineResources and returns output size from JpCoreCompleteCBR */
    }

    lossless_sem = create_named_semaphore(NULL, SEM_CREATE_LOCKED);
    
    if (is_camera("700D", "*") || is_camera("650D", "*") || is_camera("EOSM", "*") || is_camera("100D", "*"))
    {
        uint32_t resources[] = {
            0x00000 | edmac_channel_to_index(edmac_write_chan),
            0x10000 | edmac_channel_to_index(edmac_read_chan),
            0x20005,
            0x20016,
            0x30002,
            0x50034,
            0x5002d,
            0x50010,
            0x90001,
            0x90000,
            0xa0000,
            0x160000,
            0x260000,
            0x260001,
            0x260002,
            0x260003,
        };

        TTL_ResLock = CreateResLockEntry(resources, COUNT(resources));
    }
    else if (is_camera("5D3", "*") || is_camera("6D", "*"))
    {
        uint32_t resources[] = {
            0x00000 | edmac_channel_to_index(edmac_write_chan),
            0x10000 | edmac_channel_to_index(edmac_read_chan),
            0x30001,    /* Read connection 1 (uncompressed input) */
            0x2002d,    /* Write connection 45 (compressed output) */
          //0x20016,    /* Write connection 22 (for WR2 - not used) */
            0x50034,
            0x5002d,
            0x50010,
            0x90001,
            0x230000,
            0x160000,
            0x260000,
            0x260001,
            0x260002,
            0x260003,
        };
        
        TTL_ResLock = CreateResLockEntry(resources, COUNT(resources));
    }
    else if (is_camera("70D", "*"))
    {
        uint32_t resources[] = {
            0x00000 | edmac_channel_to_index(edmac_write_chan),
            0x10000 | edmac_channel_to_index(edmac_read_chan),
            0x30001,    /* Read connection 1 (uncompressed input) */
            0x2002d,    /* Write connection 45 (compressed output) */
          //0x20016,    /* Write connection 22 (for WR2 - not used) */
            0x50034,
          //0x5002d,    /* workaround, TTL_Prepare stucks otherwise if called in LV */
            0x50010,
            0x90001,
            0x230000,
            0x160000,
            0x260000,
            0x260001,
            0x260002,
            0x260003,
        };
        
        TTL_ResLock = CreateResLockEntry(resources, COUNT(resources));
    }
    
    /* optionally initialize the decompression routines */
    decompress_init();

    /* return 1 if everything looks alright */
    return TTL_Start && lossless_sem && TTL_ResLock;
}

static uint32_t start_time = 0;

/* returns output size if successful, negative on error */
int lossless_compress_raw_rectangle(
    struct memSuite * dst_suite, void * src,
    int src_width, int src_x, int src_y,
    int width, int height
)
{
    if (m50_mode)
    {
        return m50_compress(dst_suite, src, src_width, src_x, src_y, width, height);
    }

    if (!TTL_ResLock || !lossless_sem || !TTL_Start)
    {
        /* not initialized */
        return -1;
    }

    /* setup photo quality (valid values: 0=RAW, 1=MRAW, 2=SRAW, 14, 15) */
    TTL_SetArgs(0, &TTL_Args, 0);

    /* trick the encoder so it configures slice width = image width */
    /* we'll have two slices on top of each other; this will give
     * valid lossless DNG as well, if we prepend a header :)
     */
    TTL_Args.xRes = width;
    TTL_Args.yRes = height;

    /* Default EDMAC channels may be used in LiveView;
     * use the ones from edmac_memcpy instead */
    TTL_Args.RD1_Channel = edmac_read_chan;
    TTL_Args.WR1_Channel = edmac_write_chan;

    /* set starting point (top-left corner) */
    /* we need to skip a multiple of 8 pixels horizontally for raw_pixblock alignment
     * and an even number of pixels vertically, to preserve the Bayer pattern
     */
    uint32_t src_adjusted = ((uint32_t)src & 0x1FFFFFFF)
        + ((src_x/8*8) + (src_y/2*2) * src_width) * 14/8;

    /* set buffers */
    TTL_Args.WR1_MemSuite = dst_suite;
    TTL_Args.WR2_Address  = 0;
    TTL_Args.RD1_Address  = (void *) src_adjusted;

    /* to check whether the compression was successful */
    /* note: in dummy mode, dst_suite is NULL - don't check this case */
    void * WR1_Address = (TTL_Args.WR1_MemSuite)
        ? GetMemoryAddressOfMemoryChunk(GetFirstChunkFromSuite(TTL_Args.WR1_MemSuite))
        : 0;

    if (WR1_Address)
    {
        MEM(WR1_Address) = 0;
    }

    /* configure the processing modules */
    TTL_Prepare(TTL_ResLock, &TTL_Args);

    if (is_camera("6D", "*") ||
        is_camera("650D", "*"))
    {
        /* not sure what exactly these do, but they seem to be required to get correct image
         * taken from register log diffs: http://www.magiclantern.fm/forum/index.php?topic=18443.msg197987#msg197987 */
        EngDrvOut(0xC0F37610, 0);       /* 0x11 on 650D, 0 on all other D5 (not sure if needed) */
        EngDrvOut(0xC0F37628, 0x71000); /* 72000 on 650D, 71000 on all other D5 */
        EngDrvOut(0xC0F3762C, 0x71000); /* 72000 on 650D, 71000 on all other D5 */
        EngDrvOut(0xC0F37630, 0x71000); /* 72000 on 650D, 71000 on all other D5 */
        EngDrvOut(0xC0F37634, 0x71000); /* 72000 on 6D and 650D, 71000 on all other D5 */
        EngDrvOut(0xC0F3763C, 0);       /* 0x1000000 on 6D and 650D, 0 on all other D5 */
        EngDrvOut(0xC0F37640, 0);       /* 0x2000000 on 6D and 650D, 0 on all other D5 */
        EngDrvOut(0xC0F37644, 0);       /* 0x4000000 on 6D and 650D, 0 on all other D5 */
        EngDrvOut(0xC0F37648, 0);       /* 0x8000000 on 6D and 650D, 0 on all other D5 */
    }

    if (is_camera("70D", "*"))
    {
        /* 70D is different */
        // EngDrvOut(0xC0F373F4, 0x00000000);  /* alternative fixing method; 0x7FFF7FFF on 70D */
        EngDrvOut(0xC0F373B4, 0);              /* 0x11 on 70D */
        EngDrvOut(0xC0F37300, PACK32(width    - 1,  height/2  - 1));  /* 0xE7B0ADF on 70D */
        EngDrvOut(0xC0F373E8, PACK32(width    - 1,  height/2  - 1));  /* 0xE7B0ADF on 70D */
    }
    else if (is_camera("DIGIC", "5"))
    {
        /* all other D5 models use this register instead */
        /* the hardware encoder (and other image processing modules that might be used)
         * expect slice width and slice height for these registers
         * Canon configuration: slice width = image width / 2, slice height = image height
         * our configuration:   slice width = image width, slice height = image height / 2 */
        EngDrvOut(0xC0F375B4, PACK32(width    - 1,  height/2  - 1));  /* 0xF6D0B8F on 5D3 */
    }

    if (is_camera("DIGIC", "5"))
    {
        /* required for full-res silent pictures? */
        /* this one doesn't seem to refer to slice size, but to total image size */
        /* sht_mirror shows 0xC0F13000 - 0xC0F13064 as RABBIT */
        EngDrvOut(0xC0F13068, PACK32(width    - 1,  height    - 1));  /* 0xF6D171F on 5D3 */
    }

    /* there are a few registers possibly related to RD2/WR2 - we don't use them */
    /* 5D3: [0xC0F375B4] <- 0xF6D0B8F, [0xC0F12020] <- 0x18A0127  =>  slice width / 10, slice height / 10 */
    /* 6D:  [0xC0F375B4] <- 0xE7B0ADF, [0xC0F12020] <- 0x13400E7  =>  slice width / 12, slice height / 12 */
    /* 70D: [0xC0F37300] <- 0xE7B0ADF, [0xC0F12020] <- 0x13400E7  =>  slice width / 12, slice height / 12 */
    /* 650D, 700D, 100D, EOSM, EOSM2:
     *      [0xC0F375B4] <- 0xDC70A4F, [0xC0F12020] <- 0x1B80149  =>  slice width / 8, slice height / 8 */
    if (0)
    {
        /* values for 5D3 - just for future reference */
        EngDrvOut(0xC0F12010,        width    - 1                 );  /* 0xB8F     */
        EngDrvOut(0xC0F12014, PACK32(width    - 1,  height/2  - 1));  /* 0xF6D0B8F */
        EngDrvOut(0xC0F1201C,        width/10 - 1                 );  /* 0x127     */
        EngDrvOut(0xC0F12020, PACK32(width/10 - 1,  height/20 - 1));  /* 0x18A0127 */
    }

    /* need to read the image data in 2 slices
     * default configuration is 2 vertical slices;
     * however, using 2 horizontal slices makes it easy
     * to just slap a DNG header, resulting in valid output.
     * 
     * => the input EDMAC will simply read the image as usual.
     */
    struct edmac_info RD1_info = {
        .xb     = width * 14/8,
        .yb     = height - 1,
        .off1b  = src_width * 14/8 - width * 14/8,
    };

    SetEDmac(TTL_Args.RD1_Channel, TTL_Args.RD1_Address, &RD1_info, TTL_Args.RD1_Flags);

    if (verbose >= 2)
    {
        const char * WR1_SizeFmt = format_memory_size(GetSizeOfMemoryChunk(GetFirstChunkFromSuite(dst_suite)));
        printf("[TTL] %dx%d %dbpp\n", TTL_Args.xRes, TTL_Args.yRes, TTL_Args.SamplePrecision);
        printf(" WR1: %x EDMAC#%d<%d> (%x %s)\n",  WR1_Address,  TTL_Args.WR1_Channel, TTL_Args.WR1_Connection, TTL_Args.WR1_MemSuite, WR1_SizeFmt);
        printf(" WR2: %x EDMAC#%d<%d>\n", TTL_Args.WR2_Address,  TTL_Args.WR2_Channel, TTL_Args.WR2_Connection);
        printf(" RD1: %x EDMAC#%d<%d>\n", TTL_Args.RD1_Address,  TTL_Args.RD1_Channel, TTL_Args.RD1_Connection);
        printf(" RD2: %x EDMAC#%d<%d>\n", TTL_Args.RD2_Address,  TTL_Args.RD1_Channel, TTL_Args.RD2_Connection);
    }

    /* register our CBR, to be called when finished */
    TTL_RegisterCBR(4, LosslessCompleteCBR, 0);
    
    /* this changes a few registers that appear to be bit fields */
    TTL_SetFlags(0x10000);

    /* time the operation */
    start_time = get_us_clock();

    /* this starts the EDmac channels */
    TTL_Start(&TTL_Args);

    /* wait until finished */
    int err = take_semaphore(lossless_sem, 1000);

    if (verbose >= 2)
    {
        uint32_t stop_time = get_us_clock();
        printf("[TTL] Elapsed time: %d us\n", (int)(stop_time - start_time));
    }

    /* stop processing; this will report output size */
    uint32_t output_size = 0;
    TTL_Stop(&TTL_Args);
    TTL_Finish(TTL_ResLock, &TTL_Args, &output_size);

    if (verbose >= 1)
    {
        /* compute input size (uncompressed) */
        uint32_t input_size = RD1_info.xb * (RD1_info.yb + 1);

        int ratio_x100 = output_size * 10000.0 / input_size;
        printf("[TTL] Output size : %s (%s%d.%02d%%)\n", format_memory_size(output_size), FMT_FIXEDPOINT2(ratio_x100));
    }

    if (err)
    {
        return -2;
    }

    /* do we have valid JPEG data in the output buffer? */
    if (WR1_Address && MEM(WR1_Address) != 0xC4FFD8FF)
    {
        return -3;
    }

    return output_size;
}

int lossless_compress_raw(struct raw_info * raw_info, struct memSuite * output_memsuite)
{
    return lossless_compress_raw_rectangle(
        output_memsuite, raw_info->buffer,
        raw_info->width, 0, 0,
        raw_info->width, raw_info->height
    );
}


/* decompression stuff wizardry goes here */
struct DecodeLossless_args
{
    void *address;
    int depth_type;
    int is_not_16bpp;
    int x_2;
    int x_1;
    int x_mul;
    int y_2;
    int ysize_raw;
    int y_mul;
};

static void (*Setup_DecodeLosslessRawPath) (struct DecodeLossless_args *args, void (*read_cbr)(int *), void (*done_cbr)(int *), int *cbr_ctx, int *a5) = NULL;
static void (*Start_DecodeLosslessPath) (struct memSuite *a1) = NULL;
static void (*Cleanup_DecodeLosslessPath) (void) = NULL;
static struct semaphore *decompress_sem = NULL;

static void decompress_init()
{
    /* now check for the needed decompression functions */
    if (is_camera("5D3", "1.1.3"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xFF3CB010;
        Start_DecodeLosslessPath    = (void *) 0xFF3CB0D8;
        Cleanup_DecodeLosslessPath  = (void *) 0xFF3CB23C;
    }

    if (is_camera("5D3", "1.2.3"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xff3d3f04;
        Start_DecodeLosslessPath    = (void *) 0xff3d3fcc;
        Cleanup_DecodeLosslessPath  = (void *) 0xff3d4130;
    }
    
    if (is_camera("700D", "1.1.5"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xFF4294DC;
        Start_DecodeLosslessPath    = (void *) 0xFF4295A4;
        Cleanup_DecodeLosslessPath  = (void *) 0xFF429708;
    }

    if (is_camera("650D", "1.0.4"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xFF4259F4;
        Start_DecodeLosslessPath    = (void *) 0xFF425ABC;
        Cleanup_DecodeLosslessPath  = (void *) 0xFF425C20;
    }

    if (is_camera("EOSM", "2.0.2"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xFF42DBD0;
        Start_DecodeLosslessPath    = (void *) 0xFF42DC98;
        Cleanup_DecodeLosslessPath  = (void *) 0xFF42DDFC;
    }

    if (is_camera("100D", "1.0.1"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xFF42F4C8;
        Start_DecodeLosslessPath    = (void *) 0xFF42F590;
        Cleanup_DecodeLosslessPath  = (void *) 0xFF42F6F4;
    }

    if (is_camera("6D", "1.1.6"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xFF409218;
        Start_DecodeLosslessPath    = (void *) 0xFF4092E0;
        Cleanup_DecodeLosslessPath  = (void *) 0xFF409444;
    }

    if (is_camera("70D", "1.1.2"))
    {
        Setup_DecodeLosslessRawPath = (void *) 0xFF4309A4;
        Start_DecodeLosslessPath    = (void *) 0xFF430A6C;
        Cleanup_DecodeLosslessPath  = (void *) 0xFF430BD0;
    }

    /* all functions known? having the semaphore is an indicator we can decompress */
    if (Setup_DecodeLosslessRawPath && Start_DecodeLosslessPath && Cleanup_DecodeLosslessPath)
    {
        decompress_sem = create_named_semaphore("decompress_sem", SEM_CREATE_LOCKED);
    }
}

/* this one is called when decompression is done */
static void DecodeLossless_DoneCBR(int *done)
{
    give_semaphore(decompress_sem);
}

/* the read cbr is not used */
static void DecodeLossless_ReadCBR(int *done)
{
}

int lossless_decompress_raw(
    struct memSuite * src, void * dst,
    int width, int height,
    int output_bpp
)
{
    if (!decompress_sem)
    {
        return -1;
    }

    struct DecodeLossless_args decode_opts;

    decode_opts.address = dst;
    decode_opts.depth_type = 4;
    decode_opts.is_not_16bpp = (output_bpp == 16) ? 0 : 1;
    decode_opts.x_1 = width;
    decode_opts.x_2 = 0;
    decode_opts.x_mul = 0;
    decode_opts.ysize_raw = height;
    decode_opts.y_2 = 0;
    decode_opts.y_mul = 0;
    
    /* we dont use that one */
    int done = 0;
    Setup_DecodeLosslessRawPath(&decode_opts, DecodeLossless_ReadCBR, DecodeLossless_DoneCBR, &done, &done);
    Start_DecodeLosslessPath(src);
    
    /* wait for decompression to finish */
    take_semaphore(decompress_sem, 0);
    
    /* clean up */
    Cleanup_DecodeLosslessPath();

    return 0;
}
