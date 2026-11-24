/** \file
 * EDMAC mem2mem copy for EOS M50 1.1.0 (DIGIC 8), after kitor's R.180 edmac-d8.c.
 *
 * The M50 has no MemToMemE1 path. Canon's own M50 mem2mem users
 * (CapCommon VShd_ExpandData and DarkCurCor_CopyOB) go through
 * EHD/General/MemoryToMemoryEsub5, and this file repeats that recipe:
 *   lock resources {0x5001F, 0x50023}, wake sub-chips {4, 7},
 *   Setup({rd 0x3D, wr 0x18}), RegisterCBR, SetAddress, SetSize, Start,
 *   wait for the CBR, ResetCBR, ClearEdmacCBR, sleep sub-chips, unlock.
 * Read channel 61 (0xD0487600) and write channel 24 (0xD0487100) were checked
 * idle on the camera (read-only) before first use, and port 3 (LV RAW) is never touched.
 * Tested from the rawdump module: 64 KB + 4 MB x5 bit-exact, 12 LV RAW frames at 24p.
 */
/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <dryos.h>
#include <edmac.h>

#ifdef CONFIG_EDMAC_MEMCPY_M50

/* ELD/PwrMng/PwrMng.c: list of sub-chip IDs terminated by 7, like the R's PwrMng_*SubChips */
extern void PwrMng_WakeSubChips(const uint32_t *list);
extern void PwrMng_SuspendSubChips(const uint32_t *list);

/* EHD/General/MemoryToMemoryEsub5 (Canon's names unknown, named after kitor's MemToMemE1_*) */
extern void MemToMemE5_SetupEDMAC(uint32_t *channels);              /* {rd, wr} */
extern void MemToMemE5_RegisterCBR(void (*cbr)(void *), void *arg);
extern void MemToMemE5_ResetCBR(void);
extern void MemToMemE5_ClearEdmacCBR(void);
extern void MemToMemE5_SetAddress(void **src_dst);                  /* {src, dst} */
extern void MemToMemE5_SetSizeAndMode(uint32_t *src_dst_mode);      /* {src_info*, dst_info*, mode} */
extern void MemToMemE5_Start(void);

extern unsigned int DeleteResLockEntry(struct LockEntry *lockEntry);

/* ELD/Edmac/Edmac.c Pack/Unpack units (bit depth conversion during the copy).
 * Pack (WRITE ports, Edmac.c asserts otherwise): cfg {0, 1, depth, 0, 0} = Canon's LV RAW
 * writer value 0x00100020 for 14-bit. Unpack (READ ports): cfg {0, depth, 0, 0}.
 * depth 0/1/2 = 10/12/14-bit (two Canon callers each). Needs transfer mode 0.
 * Camera test: 14->12 and 14->10 output == src >> 2 / >> 4 for 122880/122880 px. */
extern void ELD_EdmacSetPack(int channel, uint32_t *cfg);
extern void ELD_EdmacSetUnpack(int channel, uint32_t *cfg);
#define M2M_RD_PORT      0xD0487600   /* ch 61 */
#define M2M_WR_PORT      0xD0487100   /* ch 24 */
#define M2M_RD_PACKUNIT  0xD0487D00
#define M2M_WR_PACKUNIT  0xD0487900

static uint32_t mem2mem_channels[2] = { 0x3D, 0x18 };              /* read 61, write 24 */
static uint32_t mem2mem_resources[2] = { 0x5001F, 0x50023 };
static const uint32_t mem2mem_subchips[2] = { 4, 7 };
#define MEM2MEM_MODE     1                                         /* what Canon uses */
#define MEM2MEM_WAIT_MS  500

static struct LockEntry *mem2mem_lock = 0;
static int mem2mem_out_bpp = 14;            /* 10/12: convert with the pack units */
static uint32_t saved_pack[4];              /* Canon's pack state of ch61/ch24, restored on unlock */
static struct semaphore *mem2mem_done_sem = 0;
static volatile int mem2mem_done = 0;

static void mem2mem_copy_comp_CBR(void *arg)
{
    mem2mem_done = 1;
    give_semaphore(mem2mem_done_sem);
}

/* one rectangle copy; blocks until the CBR (a few ms) or the timeout. Task context only. */
static uint32_t mem2mem_edmac_copy_m50(void *src, void *dst,
                                       struct edmac_info *src_info, struct edmac_info *dst_info,
                                       int out_bpp)
{
    static void *addr[2];
    static uint32_t size_mode[3];

    if (!mem2mem_done_sem)
        mem2mem_done_sem = create_named_semaphore("Mem2MemM50Done", SEM_CREATE_LOCKED);
    while (take_semaphore_now(mem2mem_done_sem) == 0)
        ; /* drop a stale give */

    mem2mem_done = 0;
    addr[0] = UNCACHEABLE(src);
    addr[1] = UNCACHEABLE(dst);
    size_mode[0] = (uint32_t)src_info;
    size_mode[1] = (uint32_t)dst_info;
    size_mode[2] = (out_bpp == 14) ? MEM2MEM_MODE : 0;

    MemToMemE5_SetupEDMAC(mem2mem_channels);
    MemToMemE5_RegisterCBR(mem2mem_copy_comp_CBR, NULL);
    MemToMemE5_SetAddress(addr);
    MemToMemE5_SetSizeAndMode(size_mode);
    if (out_bpp != 14)
    {
        static uint32_t unpack14[4] = { 0, 2, 0, 0 };
        static uint32_t pack[5]     = { 0, 1, 2, 0, 0 };
        pack[2] = (out_bpp == 12) ? 1 : 0;
        ELD_EdmacSetUnpack(mem2mem_channels[0], unpack14);
        ELD_EdmacSetPack(mem2mem_channels[1], pack);
    }
    MemToMemE5_Start();

    take_semaphore(mem2mem_done_sem, MEM2MEM_WAIT_MS);

    MemToMemE5_ResetCBR();
    MemToMemE5_ClearEdmacCBR();
    return mem2mem_done ? 0 : 1;
}

void edmac_memcpy_res_lock()
{
    mem2mem_lock = CreateResLockEntry(mem2mem_resources, COUNT(mem2mem_resources));
    LockEngineResources(mem2mem_lock);
    PwrMng_WakeSubChips(mem2mem_subchips);

    /* Canon's VShd/DarkCur paths use ch61/24 without setting a pack config */
    saved_pack[0] = MEM(M2M_RD_PACKUNIT + 0x24);
    saved_pack[1] = MEM(M2M_RD_PORT + 0xD0);
    saved_pack[2] = MEM(M2M_WR_PACKUNIT + 0x24);
    saved_pack[3] = MEM(M2M_WR_PORT + 0xD0);
}

void edmac_memcpy_res_unlock()
{
    MEM(M2M_RD_PACKUNIT + 0x24) = saved_pack[0];
    MEM(M2M_RD_PORT + 0xD0)     = saved_pack[1];
    MEM(M2M_WR_PACKUNIT + 0x24) = saved_pack[2];
    MEM(M2M_WR_PORT + 0xD0)     = saved_pack[3];
    PwrMng_SuspendSubChips(mem2mem_subchips);
    UnLockEngineResources(mem2mem_lock);
    DeleteResLockEntry(mem2mem_lock);
    mem2mem_lock = 0;
}

/* 10 or 12: later copies convert the 14-bit source; anything else: plain copy */
void edmac_copy_set_output_bpp(int bpp)
{
    mem2mem_out_bpp = (bpp == 10 || bpp == 12) ? bpp : 14;
}

int edmac_copy_output_bpp_supported()
{
    return 1;
}

/* same contract as src/edmac-memcpy.c; the copy is synchronous, cbr_w is called when done.
 * With an output bpp of 10/12, w and src_x are in 14-bit source bytes and dst_width,
 * dst_x in output bytes; the output line is w * bpp / 14 bytes. */
void* edmac_copy_rectangle_cbr_start(void *dst, void *src,
                                     int src_width, int src_x, int src_y,
                                     int dst_width, int dst_x, int dst_y,
                                     int w, int h,
                                     void (*cbr_r)(void *), void (*cbr_w)(void *), void *cbr_ctx)
{
    if ((src == NULL) || (dst == NULL) || !mem2mem_lock)
        return NULL;

    /* write back dirty lines: the DMA reads RAM, and no dirty dst line may land on its result later */
    sync_caches();

    uint8_t *src_adj = (uint8_t *)src + src_x + src_y * src_width;
    uint8_t *dst_adj = (uint8_t *)dst + dst_x + dst_y * dst_width;

    struct edmac_info src_region = {
        .off1b = src_width - w,
        .xb = w,
        .yb = h - 1,
    };

    int out_bpp = mem2mem_out_bpp;
    int dst_w = w * out_bpp / 14;
    struct edmac_info dst_region = {
        .off1b = dst_width - dst_w,
        .xb = dst_w,
        .yb = h - 1,
    };

    uint32_t err = mem2mem_edmac_copy_m50(src_adj, dst_adj, &src_region, &dst_region, out_bpp);
    if (cbr_r) cbr_r(cbr_ctx);
    if (cbr_w) cbr_w(cbr_ctx);   /* clears edmac_active in mlv_lite */
    return err ? NULL : dst;
}

void* edmac_copy_rectangle_adv_start(void *dst, void *src,
                                     int src_width, int src_x, int src_y,
                                     int dst_width, int dst_x, int dst_y,
                                     int w, int h)
{
    return edmac_copy_rectangle_cbr_start(dst, src,
                                          src_width, src_x, src_y,
                                          dst_width, dst_x, dst_y,
                                          w, h, NULL, NULL, NULL);
}

void edmac_copy_rectangle_adv_cleanup()
{
}

void edmac_copy_rectangle_adv_finish()
{
}

void* edmac_copy_rectangle_adv(void *dst, void *src,
                               int src_width, int src_x, int src_y,
                               int dst_width, int dst_x, int dst_y,
                               int w, int h)
{
    return edmac_copy_rectangle_adv_start(dst, src,
                                          src_width, src_x, src_y,
                                          dst_width, dst_x, dst_y,
                                          w, h);
}

/* mlv_lite links these; the M50 path doesn't use ML-chosen channels */
uint32_t edmac_read_chan = 0x3D;
uint32_t edmac_write_chan = 0x18;

#endif // CONFIG_EDMAC_MEMCPY_M50
