/**
 * rawdump: one-shot LiveView RAW frame dump for EOS M50 bring-up.
 *
 * Uses only Canon's own named event procedures (no hardcoded addresses):
 *   lv_set_mm(1)    select RAW instead of YUV for lv_save_raw (DIGIC 8, see src/raw.c)
 *   lv_save_raw(1)  enable LiveView RAW capture
 *   lv_raw_dump     Canon debug routine: dump_file("%08lx.mm1|.mmr", raw_buffer, size)
 * The dumped file name is the raw buffer address; its size gives the buffer size.
 */

#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <bmp.h>
#include <raw.h>
#include <lens.h>
#include <exmem.h>
#include <patch.h>

static volatile int rawdump_running = 0;

static void rawdump_task()
{
    /* give the user time to close the ML menu, so LiveView is running */
    msleep(10000);

    NotifyBox(2000, "rawdump: enabling LV RAW...");
    call("lv_set_mm", 1);
    call("lv_save_raw", 1);
    msleep(1000);

    NotifyBox(2000, "rawdump: dumping frame...");
    call("lv_raw_dump");
    msleep(2000);

    /* back to Canon's defaults (YUV, raw capture off) */
    call("lv_save_raw", 0);
    call("lv_set_mm", 0);

    NotifyBox(5000, "rawdump: done. Check card for *.mm1 / *.mmr");
    rawdump_running = 0;
}

/* Canon's LiveView RAW state struct on M50 1.1.0 (read-only here).
 * Found via lv_raw_dump: get_raw_buf()=MEM(s+0x58), size=MEM(s+0x10)*MEM(s+0x14)*14/8,
 * lv_save_raw writes s+0x40, lv_set_mm writes s+0x44. Pointer 0x12748 is loaded by
 * both FUN_e080d4ec and lv_set_mm (two independent references). */
#define M50_LV_RAW_STRUCT 0x12748

static void rawinfo_task()
{
    msleep(3000);

    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/RAWINFO.TXT");
    if (f) my_fprintf(f, "=== rawinfo movie=%d video_res=%d fps_x100=%d lv=%d dispsize=%d shooting=%x custom=%x\n",
                      is_movie_mode(), video_mode_resolution, video_mode_fps_x100, lv, lv_dispsize, shooting_mode, shooting_mode_custom);
    NotifyBox(2000, "rawinfo: enabling LV RAW...");
    call("lv_set_mm", 1);
    call("lv_save_raw", 1);

    for (int i = 0; i < 10; i++)
    {
        msleep(500);
        uint32_t s = M50_LV_RAW_STRUCT;
        uint32_t buf = MEM(s + 0x58), a = MEM(s + 0x10), b = MEM(s + 0x14);
        uint32_t en = MEM(s + 0x40), mm = MEM(s + 0x44);
        NotifyBox(2000, "buf=%x  %dx%d  raw=%d mm=%d", buf, a, b, en, mm);
        if (f) my_fprintf(f, "t=%d buf=%08x f10=%d f14=%d save_raw=%d mm=%d\n",
                          get_ms_clock(), buf, a, b, en, mm);
    }

    raw_lv_request();
    msleep(300);
    int rok = raw_update_params();
    if (f) my_fprintf(f, "raw_update_params=%d raw_info %dx%d pitch %d bpp %d active %d,%d..%d,%d jpeg %dx%d at %d,%d\n", rok,
                      raw_info.width, raw_info.height, raw_info.pitch, raw_info.bits_per_pixel,
                      raw_info.active_area.x1, raw_info.active_area.y1, raw_info.active_area.x2, raw_info.active_area.y2,
                      raw_info.jpeg.width, raw_info.jpeg.height, raw_info.jpeg.x, raw_info.jpeg.y);
    raw_lv_release();
    call("lv_save_raw", 0);
    call("lv_set_mm", 0);
    if (f) FIO_CloseFile(f);
    NotifyBox(5000, "rawinfo: done. ML/LOGS/RAWINFO.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(rawinfo_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("rawinfo_task", 0x1e, 0x2000, rawinfo_task, 0);
}

extern int dng_write_step;   /* src/chdk-dng.c: last write_dng step, debug */
extern int dng_write_ret;    /* src/chdk-dng.c: return of the raw data write */

/* Uses ML's own raw.c path (CONFIG_RAW_LIVEVIEW + LV_RAW_STRUCT_ADDR) and saves a DNG. */
static void rawsnap_task()
{
    msleep(10000);

    FILE *f = FIO_CreateFile("ML/LOGS/RAWSNAP.TXT");
    NotifyBox(2000, "rawsnap: raw_lv_request...");
    raw_lv_request();
    msleep(1000);

    int ok = raw_update_params();
    if (f)
    {
        my_fprintf(f, "raw_update_params=%d lv_raw_enabled=%d\n", ok, raw_lv_is_enabled());
        my_fprintf(f, "buffer=%x %dx%d pitch=%d bpp=%d frame_size=%d\n",
                   raw_info.buffer, raw_info.width, raw_info.height, raw_info.pitch,
                   raw_info.bits_per_pixel, raw_info.frame_size);
        my_fprintf(f, "black=%d white=%d active=%d,%d-%d,%d dr=%d\n",
                   raw_info.black_level, raw_info.white_level,
                   raw_info.active_area.x1, raw_info.active_area.y1,
                   raw_info.active_area.x2, raw_info.active_area.y2, raw_info.dynamic_range);
    }

    if (ok)
    {
        int size = raw_info.frame_size;

        /* Earlier runs: a big write at file offset 0 works from any buffer, but write_dng's
         * raw write (after a 512-aligned header+thumbnail) fails. Test offset vs alignment. */
        void *copy = fio_malloc(size + 64);
        void *pad  = fio_malloc(32768);
        if (f) my_fprintf(f, "fio_malloc(%d)=%x pad=%x\n", size + 64, copy, pad);
        if (copy && pad)
        {
            void *aligned = (void *)(((uintptr_t)copy + 63) & ~63);
            memcpy(aligned, raw_info.buffer, size);
            memset(pad, 0, 32768);

            /* D: write at offset 32768 from a buffer misaligned by 4 */
            NotifyBox(2000, "rawsnap: D offset + misaligned...");
            FILE *fd = FIO_CreateFile("ML/LOGS/RAW_D.BIN");
            int rd0 = fd ? FIO_WriteFile(fd, pad, 32768) : -1;
            int rd1 = fd ? FIO_WriteFile(fd, (char *)aligned + 4, size) : -1;
            if (fd) FIO_CloseFile(fd);
            if (f) my_fprintf(f, "D pad=%d raw(ptr %x)=%d of %d\n", rd0, (char *)aligned + 4, rd1, size);

            /* E: write at offset 32768 from a 64-byte aligned buffer */
            NotifyBox(2000, "rawsnap: E offset + aligned...");
            FILE *fe = FIO_CreateFile("ML/LOGS/RAW_E.BIN");
            int re0 = fe ? FIO_WriteFile(fe, pad, 32768) : -1;
            int re1 = fe ? FIO_WriteFile(fe, aligned, size) : -1;
            if (fe) FIO_CloseFile(fe);
            if (f) my_fprintf(f, "E pad=%d raw(ptr %x)=%d of %d\n", re0, aligned, re1, size);

            /* F: DNG from the aligned copy (save_dng byte-swaps the buffer in place) */
            NotifyBox(2000, "rawsnap: F DNG from aligned copy...");
            struct raw_info ri = raw_info;
            ri.buffer = aligned;
            int rc = save_dng("ML/LOGS/RAWSNAP.DNG", &ri);
            if (f) my_fprintf(f, "F save_dng(aligned)=%d step=%d ret=%d\n", rc, dng_write_step, dng_write_ret);
            NotifyBox(5000, "rawsnap: done (DNG %s). ML/LOGS/", rc ? "ok" : "failed");
        }
        if (copy) fio_free(copy);
        if (pad) fio_free(pad);
        if (!copy || !pad)
        {
            NotifyBox(5000, "rawsnap: fio_malloc failed. See RAWSNAP.TXT");
        }
    }
    else
    {
        NotifyBox(5000, "rawsnap: raw_update_params failed. See RAWSNAP.TXT");
    }

    raw_lv_release();
    if (f) FIO_CloseFile(f);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(rawsnap_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("rawsnap_task", 0x1e, 0x4000, rawsnap_task, 0);
}

/* First RAW video test: capture a short burst into RAM with CPU memcpy (no EDMAC
 * copy on M50 yet), then save it as a DNG sequence (opens as CinemaDNG in Resolve).
 * Logs timing and memory so we learn the real copy speed and frame rate. */
#define BURST_MAX_FRAMES 12      /* run 1 with 48 (~147 MB) froze LV and crashed; run 2 with 8 was ok */
#define BURST_FRAME_US   41708   /* 23.976 fps */

extern void dcache_clean(uint32_t addr, uint32_t size);   /* Canon stub, M50 stubs.S */
extern void *memcpy64(void *dest, void *srce, size_t n);   /* src/stdio.c */

/* ARMv7 L1 data cache invalidate by MVA (DCIMVAC) over a range, then DSB, so the
 * cached alias of a DMA-written buffer reads fresh data. Only used on Canon's LV
 * raw buffer, which the CPU never writes through the cached alias. */
static void dcache_invalidate_range(void *p, int size)
{
    uint32_t a = (uint32_t)p & ~31u;
    uint32_t end = (uint32_t)p + size;
    for (; a < end; a += 32)
        asm volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(a) : "memory");
    asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

/* append one line to BURST.TXT and close it, so the log survives a crash */
static void blog(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/BURST.TXT");
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void rawburst_task()
{
    static void *frames[BURST_MAX_FRAMES];
    static int t_start_us[BURST_MAX_FRAMES];
    static int t_copy_us[BURST_MAX_FRAMES];
    char line[128];

    msleep(10000);

    FILE *f0 = FIO_CreateFile("ML/LOGS/BURST.TXT");   /* truncate */
    if (f0) FIO_CloseFile(f0);
    blog("1 start");

    raw_lv_request();
    msleep(1000);

    int n = 0;
    if (!raw_update_params())
    {
        NotifyBox(5000, "rawburst: raw_update_params failed");
        blog("2 raw_update_params failed");
        goto end;
    }

    int size = raw_info.frame_size;
    snprintf(line, sizeof(line), "2 frame %dx%d bpp=%d size=%d src=%x",
             raw_info.width, raw_info.height, raw_info.bits_per_pixel, size,
             UNCACHEABLE(raw_info.buffer));
    blog(line);

    for (n = 0; n < BURST_MAX_FRAMES; n++)
    {
        frames[n] = fio_malloc(size);
        if (!frames[n]) break;
    }
    snprintf(line, sizeof(line), "3 allocated %d frames (%d KB each)", n, size / 1024);
    blog(line);
    if (n == 0) { NotifyBox(5000, "rawburst: no memory"); goto end; }

    /* Run 2: uncached -> uncached memcpy was ~4 MB/s (0.8 s/frame), which tears
     * moving areas. Measure the cached variants on one frame each, use the faster. */
    NotifyBox(2000, "rawburst: copy test...");
    void *src_unc = UNCACHEABLE(raw_info.buffer);
    void *src_c   = CACHEABLE(raw_info.buffer);

    /* V1: uncached source -> cached destination */
    int ts = get_us_clock();
    memcpy(CACHEABLE(frames[0]), src_unc, size);
    dcache_clean((uint32_t)CACHEABLE(frames[0]), size);
    int v1 = get_us_clock() - ts;
    msleep(100);

    /* V2: invalidate source lines, then cached source -> cached destination */
    ts = get_us_clock();
    dcache_invalidate_range(src_c, size);
    memcpy(CACHEABLE(frames[0]), src_c, size);
    dcache_clean((uint32_t)CACHEABLE(frames[0]), size);
    int v2 = get_us_clock() - ts;

    int mode = (v2 < v1) ? 2 : 1;
    snprintf(line, sizeof(line), "4 V1 unc->cached %d us (%d MB/s), V2 inval+cached %d us (%d MB/s), using V%d",
             v1, v1 ? size / v1 : 0, v2, v2 ? size / v2 : 0, mode);
    blog(line);
    msleep(100);

    /* Run 3: V2 was ~270 ms/frame (~11 MB/s); time each part of it separately */
    void *dst0 = CACHEABLE(frames[0]);
    int a = get_us_clock();
    dcache_invalidate_range(src_c, size);
    int b = get_us_clock();
    memcpy(dst0, src_c, size);
    int c = get_us_clock();
    dcache_clean((uint32_t)dst0, size);
    int d = get_us_clock();
    memcpy64(dst0, src_c, size);           /* ML's word-wise copy */
    int e = get_us_clock();
    if (n > 1) memcpy(dst0, CACHEABLE(frames[1]), size);   /* cached RAM -> cached RAM */
    int g = get_us_clock();
    snprintf(line, sizeof(line), "4b invalidate %d us, memcpy %d us, dcache_clean %d us, memcpy64 %d us, ram->ram memcpy %d us",
             b - a, c - b, d - c, e - d, g - e);
    blog(line);
    msleep(500);

    /* the burst, always yielding at least 10 ms after each frame */
    NotifyBox(2000, "rawburst: recording %d frames (V%d)...", n, mode);
    int t0 = get_us_clock();
    for (int i = 0; i < n; i++)
    {
        ts = get_us_clock();
        void *dst = CACHEABLE(frames[i]);
        if (mode == 2)
        {
            dcache_invalidate_range(src_c, size);
            memcpy(dst, src_c, size);
        }
        else
        {
            memcpy(dst, src_unc, size);
        }
        dcache_clean((uint32_t)dst, size);   /* save_dng byte-swaps via the uncached alias */
        int te = get_us_clock();
        t_start_us[i] = ts - t0;
        t_copy_us[i]  = te - ts;

        int next = t0 + (i + 1) * BURST_FRAME_US;
        int wait = next - get_us_clock();
        msleep(wait > 10000 ? wait / 1000 : 10);
    }
    int t_total = get_us_clock() - t0;
    snprintf(line, sizeof(line), "5 burst %d frames in %d ms", n, t_total / 1000);
    blog(line);
    for (int i = 0; i < n; i++)
    {
        snprintf(line, sizeof(line), "  frame %02d start %7d us copy %7d us", i, t_start_us[i], t_copy_us[i]);
        blog(line);
    }

    /* save as DNG sequence */
    FIO_CreateDirectory("ML/RAWVID");
    int saved = 0;
    for (int i = 0; i < n; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "ML/RAWVID/F%04d.DNG", i);
        NotifyBox(1000, "rawburst: saving %d/%d", i + 1, n);
        struct raw_info ri = raw_info;
        ri.buffer = frames[i];
        saved += save_dng(name, &ri) ? 1 : 0;
    }
    snprintf(line, sizeof(line), "6 saved %d/%d DNG", saved, n);
    blog(line);
    NotifyBox(5000, "rawburst: done, %d DNG in ML/RAWVID", saved);

end:
    for (int i = 0; i < n; i++) fio_free(frames[i]);
    raw_lv_release();
    blog("7 end");
    rawdump_running = 0;
}

/* READ-ONLY check of ELD EDMAC port 3 (LV RAW write, found in ROM: CapPath Mem1 setup,
 * channel table 0xE0DD7BA0, same as EOS R). Only reads registers Canon itself writes:
 * +0xA0 address, +0x48..+0x70 size/offset. No writes. */
#define EDMAC_PORT3 0xD0420200

static void edmacinfo_task()
{
    char line[160];
    msleep(10000);
    FILE *f0 = FIO_CreateFile("ML/LOGS/EDMAC3.TXT");
    if (f0) FIO_CloseFile(f0);

    raw_lv_request();
    msleep(1000);
    snprintf(line, sizeof(line), "lv struct buf=%x  w=%d h=%d", MEM(M50_LV_RAW_STRUCT + 0x58),
             MEM(M50_LV_RAW_STRUCT + 0x10), MEM(M50_LV_RAW_STRUCT + 0x14));
    {
        FILE *f = FIO_CreateFileOrAppend("ML/LOGS/EDMAC3.TXT");
        if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
    }
    for (int i = 0; i < 20; i++)
    {
        uint32_t b = EDMAC_PORT3;
        snprintf(line, sizeof(line), "t=%d addr(A0)=%x 48=%x 4c=%x 50=%x 54=%x 58=%x 5c=%x 60=%x 68=%x 6c=%x 70=%x",
                 get_ms_clock(), MEM(b + 0xA0), MEM(b + 0x48), MEM(b + 0x4C), MEM(b + 0x50), MEM(b + 0x54),
                 MEM(b + 0x58), MEM(b + 0x5C), MEM(b + 0x60), MEM(b + 0x68), MEM(b + 0x6C), MEM(b + 0x70));
        FILE *f = FIO_CreateFileOrAppend("ML/LOGS/EDMAC3.TXT");
        if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
        if (i == 0) NotifyBox(3000, "%x (LV buf %x)", MEM(b + 0xA0), MEM(M50_LV_RAW_STRUCT + 0x58));
        msleep(50);
    }
    raw_lv_release();
    NotifyBox(5000, "edmac3: done. ML/LOGS/EDMAC3.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(edmacinfo_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("edmacinfo_task", 0x1e, 0x2000, edmacinfo_task, 0);
}

/* READ-ONLY scan for EDMAC channels Canon never uses (candidates for our mem2mem copy).
 * Only the 0xD042xxxx block (EDMAC_1, same block as port 3): it is proven awake in LV.
 * Reading MMIO of a sleeping block can hard-lock the camera, so other blocks are skipped.
 * Channel index = position in the ROM channel table 0xE0DD7BA0. No writes. */
static const struct { int ch; uint32_t base; } edmac_scan_list[] = {
    { 1, 0xD0420000}, { 2, 0xD0420100}, { 3, 0xD0420200}, { 4, 0xD0420300}, { 5, 0xD0420400},
    { 6, 0xD0420500}, { 7, 0xD0420600}, { 8, 0xD0420700}, { 9, 0xD0420800}, {10, 0xD0420900},
    {11, 0xD0420A00}, {12, 0xD0420B00}, {13, 0xD0420C00},
    {40, 0xD0421000}, {41, 0xD0421100}, {42, 0xD0421200}, {43, 0xD0421300},
    {44, 0xD0421400}, {45, 0xD0421500}, {46, 0xD0421600}, {47, 0xD0421700},
};
#define EDMAC_SCAN_N     COUNT(edmac_scan_list)
#define EDMAC_SCAN_LOG   "ML/LOGS/EDMACSCN.TXT"

static void edmacscan_task()
{
    static uint32_t first[EDMAC_SCAN_N], last[EDMAC_SCAN_N], size_seen[EDMAC_SCAN_N];
    static int nonzero[EDMAC_SCAN_N], changes[EDMAC_SCAN_N];
    char line[160];
    msleep(10000);

    /* LV RAW on during the scan (the conditions we'll copy under); port 3 must then show "used" */
    raw_lv_request();
    msleep(1000);

    for (int c = 0; c < EDMAC_SCAN_N; c++)
    {
        first[c] = last[c] = MEM(edmac_scan_list[c].base + 0xA0);
        size_seen[c] = 0; nonzero[c] = 0; changes[c] = 0;
    }

    /* 30 s: start/stop video recording during this window to catch recording-only channels */
    int samples = 0;
    for (int t = 0; t < 600; t++)
    {
        for (int c = 0; c < EDMAC_SCAN_N; c++)
        {
            uint32_t a = MEM(edmac_scan_list[c].base + 0xA0);
            uint32_t s = MEM(edmac_scan_list[c].base + 0x50);
            if (a) nonzero[c]++;
            if (a != last[c]) changes[c]++;
            if (s) size_seen[c] = s;
            last[c] = a;
        }
        samples++;
        if (t % 100 == 0) NotifyBox(1500, "edmac scan %d s", t / 20);
        msleep(50);
    }
    raw_lv_release();

    FILE *f = FIO_CreateFile(EDMAC_SCAN_LOG);
    if (f)
    {
        my_fprintf(f, "samples=%d (50 ms, read-only, block 0xD042xxxx, LV RAW on)\n", samples);
        for (int c = 0; c < EDMAC_SCAN_N; c++)
        {
            int free_ch = !nonzero[c] && !changes[c] && !size_seen[c];
            snprintf(line, sizeof(line), "ch%02d %x first=%x last=%x nonzero=%d changes=%d size50=%x %s",
                     edmac_scan_list[c].ch, edmac_scan_list[c].base, first[c], last[c],
                     nonzero[c], changes[c], size_seen[c], free_ch ? "FREE" : "used");
            my_fprintf(f, "%s\n", line);
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "edmac scan: done. " EDMAC_SCAN_LOG);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(edmacscan_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("edmacscan_task", 0x1e, 0x2000, edmacscan_task, 0);
}

/* READ-ONLY check of the EDMAC_5 pair Canon's own M50 mem2mem recipe uses
 * (CapCommon VShd_ExpandData / DarkCurCor_CopyOB, MemoryToMemoryEsub5):
 * read ch 0x3D = 61 (0xD0487600), write ch 0x18 = 24 (0xD0487100).
 * The block must be awake before any MMIO read, so we do what Canon does first:
 * reserve its resources {0x5001F, 0x50023} and wake sub-chip list {4, 7}.
 * No StartEDmac / set_address / set_size: registers are only read.
 * ROM addresses are called directly so the core build stays unchanged. */
#define M2M_RD_BASE 0xD0487600
#define M2M_WR_BASE 0xD0487100

typedef void *(*create_res_lock_t)(uint32_t *ids, uint32_t count);
typedef unsigned int (*res_lock_op_t)(void *entry);
typedef void (*pwr_subchips_t)(const uint32_t *list);
#define rom_CreateResLockEntry   ((create_res_lock_t) (0xE055819E | 1))
#define rom_LockEngineResources  ((res_lock_op_t)     (0xE0558386 | 1))
#define rom_UnLockEngineResources ((res_lock_op_t)    (0xE0558426 | 1))
#define rom_DeleteResLockEntry   ((res_lock_op_t)     (0xE0558282 | 1))
#define rom_PwrMngEnableSubChip  ((pwr_subchips_t)    (0xE0557ECE | 1))
#define rom_PwrMngDisableSubChip ((pwr_subchips_t)    (0xE0557EFC | 1))

#define M2M_CHECK_LOG "ML/LOGS/M2MCHK.TXT"

static void m2m_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend(M2M_CHECK_LOG);
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void m2mcheck_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    char line[160];
    msleep(10000);
    FILE *f0 = FIO_CreateFile(M2M_CHECK_LOG);
    if (f0) FIO_CloseFile(f0);

    void *lock = rom_CreateResLockEntry(res_ids, 2);
    snprintf(line, sizeof(line), "CreateResLockEntry -> %x", lock);
    m2m_log(line);
    if (!lock) { NotifyBox(5000, "m2m check: no lock entry"); rawdump_running = 0; return; }

    unsigned int r = rom_LockEngineResources(lock);
    snprintf(line, sizeof(line), "LockEngineResources -> %x", r);
    m2m_log(line);
    if (r & 1) { rom_DeleteResLockEntry(lock); NotifyBox(5000, "m2m check: lock failed %x", r); rawdump_running = 0; return; }

    rom_PwrMngEnableSubChip(subchips);
    m2m_log("sub-chip 4 enabled");

    for (int i = 0; i < 20; i++)
    {
        snprintf(line, sizeof(line), "t=%d rd61 A0=%x 50=%x | wr24 A0=%x 50=%x",
                 get_ms_clock(), MEM(M2M_RD_BASE + 0xA0), MEM(M2M_RD_BASE + 0x50),
                 MEM(M2M_WR_BASE + 0xA0), MEM(M2M_WR_BASE + 0x50));
        m2m_log(line);
        msleep(50);
    }

    rom_PwrMngDisableSubChip(subchips);
    snprintf(line, sizeof(line), "UnLock -> %x  Delete -> %x",
             rom_UnLockEngineResources(lock), rom_DeleteResLockEntry(lock));
    m2m_log(line);
    NotifyBox(5000, "m2m check: done. " M2M_CHECK_LOG);
    rawdump_running = 0;
}

/* First real EDMAC copy: RAM -> RAM, Canon's own M50 recipe (MemoryToMemoryEsub5),
 * on the pair checked above (rd 61, wr 24). Never touches port 3 (LV RAW).
 * Every step is logged BEFORE it runs, so a hang shows where it stopped.
 * DIGIC 8 edmac_info layout (verified against the M50 set_size at 0xE054A9BA). */
struct edmac_info_d8
{
    uint32_t off1s, off1a, off1b, off2s, off2a, off2b, off3;
    uint32_t xs, xa, xb, ys, ya, yb, xn, yn;
};

typedef void (*m2m5_setup_t)(uint32_t *chans);              /* {rd, wr} */
typedef void (*m2m5_reg_cbr_t)(void (*cbr)(void *), void *arg);
typedef void (*m2m5_set_addr_t)(void **src_dst);            /* {src, dst} */
typedef void (*m2m5_set_size_t)(void *src_dst_mode);        /* {src_info*, dst_info*, mode} */
typedef void (*m2m5_void_t)(void);
#define rom_m2m5_Setup        ((m2m5_setup_t)    (0xE084DE9C | 1))
#define rom_m2m5_RegisterCBR  ((m2m5_reg_cbr_t)  (0xE084DF40 | 1))
#define rom_m2m5_ResetCBR     ((m2m5_void_t)     (0xE084DF4A | 1))
#define rom_m2m5_ClearEdmac   ((m2m5_void_t)     (0xE084DF26 | 1))
#define rom_m2m5_SetAddress   ((m2m5_set_addr_t) (0xE084DF70 | 1))
#define rom_m2m5_SetSize      ((m2m5_set_size_t) (0xE084DF8A | 1))
#define rom_m2m5_Start        ((m2m5_void_t)     (0xE084DF56 | 1))

#define M2M_COPY_LOG   "ML/LOGS/M2MCOPY.TXT"
#define M2M_XB         2048                    /* bytes per line */
#define M2M_GUARD      4096

static volatile int m2m_done;
static volatile int m2m_start_us, m2m_cbr_us;    /* DMA start / completion (taken in the CBR) */
static struct semaphore *m2m_sem;
static void m2m_copy_cbr(void *arg)
{
    m2m_cbr_us = get_us_clock();
    m2m_done = 1;
    give_semaphore(m2m_sem);                      /* wakes the copying task right away */
}

static void m2mc_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend(M2M_COPY_LOG);
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

/* one copy of rows lines of xb bytes; returns ms taken, -1 on timeout */
static int m2m_copy_rect(void *src, void *dst, int xb, int rows, int log_steps)
{
    static uint32_t chans[2] = { 0x3D, 0x18 };
    static struct edmac_info_d8 info;
    static uint32_t size_arg[3];
    static void *addr_arg[2];

    memset(&info, 0, sizeof(info));
    info.xb = xb;
    info.yb = rows - 1;
    size_arg[0] = (uint32_t) &info; size_arg[1] = (uint32_t) &info; size_arg[2] = 1;
    addr_arg[0] = UNCACHEABLE(src); addr_arg[1] = UNCACHEABLE(dst);

    if (!m2m_sem) m2m_sem = create_named_semaphore("m2m_done", SEM_CREATE_LOCKED);
    while (take_semaphore_now(m2m_sem) == 0) ;   /* drop a stale give */
    m2m_done = 0;
    if (log_steps) m2mc_log("Setup(rd 0x3D, wr 0x18)");
    rom_m2m5_Setup(chans);
    if (log_steps) m2mc_log("RegisterCBR");
    rom_m2m5_RegisterCBR(m2m_copy_cbr, 0);
    if (log_steps) m2mc_log("SetAddress");
    rom_m2m5_SetAddress(addr_arg);
    if (log_steps) m2mc_log("SetSize");
    rom_m2m5_SetSize(size_arg);
    if (log_steps) m2mc_log("Start");
    int t0 = get_ms_clock();
    m2m_start_us = get_us_clock();
    rom_m2m5_Start();
    take_semaphore(m2m_sem, 500);
    int dt = get_ms_clock() - t0;
    if (log_steps) m2mc_log(m2m_done ? "CBR fired" : "TIMEOUT (500 ms), stopping channels");
    rom_m2m5_ResetCBR();
    rom_m2m5_ClearEdmac();
    if (log_steps) m2mc_log("ResetCBR + ClearEdmac ok");
    return m2m_done ? dt : -1;
}

/* EDMAC Pack/Unpack (ELD/Edmac/Edmac.c 0xE054B99A, cfg = 5 words, low byte used).
 * Two Canon callers agree on cfg[2]: 0 = 10-bit, 1 = 12-bit, 2 = 14-bit
 * (FUN_e08d970e and EHD MultipleAddRaw FUN_e084d0e0), cfg[3] = 1, cfg[0] = 1 for 16-bit.
 * Pack units of our mem2mem pair: ch61 -> 0xD0487D00, ch24 -> 0xD0487900 (ROM table 0xE0DD7E00). */
typedef void (*edmac_pack_cfg_t)(int ch, uint32_t *cfg);
#define rom_EdmacSetPack   ((edmac_pack_cfg_t) (0xE054B99A | 1))   /* WRITE ports only (Edmac.c:1659 assert) */
/* READ ports: 0xE054BA44, cfg = 4 words {0, depth, 1, 0}; depth 0/1/2 = 10/12/14-bit
 * (Canon callers, ghidra_scripts/callers_unpack.txt: one maps 12->1 / 14->2, two 10-bit paths use 0) */
#define rom_EdmacSetUnpack ((edmac_pack_cfg_t) (0xE054BA44 | 1))
#define PACK_UNIT_RD61   0xD0487D00
#define PACK_UNIT_WR24   0xD0487900

/* like m2m_copy_rect, but src/dst line sizes differ and both channels get a pack config */
static int m2m_copy_pack(void *src, void *dst, int src_xb, int dst_xb, int rows,
                         uint32_t *rd_cfg, uint32_t *wr_cfg, int mode)
{
    static uint32_t chans[2] = { 0x3D, 0x18 };
    static struct edmac_info_d8 sinfo, dinfo;
    static uint32_t size_arg[3];
    static void *addr_arg[2];

    memset(&sinfo, 0, sizeof(sinfo)); memset(&dinfo, 0, sizeof(dinfo));
    sinfo.xb = src_xb; sinfo.yb = rows - 1;
    dinfo.xb = dst_xb; dinfo.yb = rows - 1;
    /* Canon sets transfer mode 0 after a pack config (FUN_e08d970e: FUN_e054aeb0(ch, 0));
     * mode 1 (32-bit) left every odd 16-bit word empty in tests #2..#4 */
    size_arg[0] = (uint32_t) &sinfo; size_arg[1] = (uint32_t) &dinfo; size_arg[2] = mode;
    addr_arg[0] = UNCACHEABLE(src); addr_arg[1] = UNCACHEABLE(dst);

    if (!m2m_sem) m2m_sem = create_named_semaphore("m2m_done", SEM_CREATE_LOCKED);
    while (take_semaphore_now(m2m_sem) == 0) ;
    m2m_done = 0;
    rom_m2m5_Setup(chans);
    rom_m2m5_RegisterCBR(m2m_copy_cbr, 0);
    rom_m2m5_SetAddress(addr_arg);
    rom_m2m5_SetSize(size_arg);
    if (rd_cfg) rom_EdmacSetUnpack(0x3D, rd_cfg);
    if (wr_cfg) rom_EdmacSetPack(0x18, wr_cfg);
    int t0 = get_ms_clock();
    rom_m2m5_Start();
    take_semaphore(m2m_sem, 500);
    int dt = get_ms_clock() - t0;
    rom_m2m5_ResetCBR();
    rom_m2m5_ClearEdmac();
    return m2m_done ? dt : -1;
}

/* pixel i of a Canon packed buffer: big-endian bit stream inside little-endian 16-bit words */
static int packed_px(const uint8_t *buf, int bpp, int i)
{
    int bit = i * bpp, v = 0;
    for (int k = 0; k < bpp; k++, bit++)
    {
        int word = bit >> 4, b = 15 - (bit & 15);
        int w = buf[word * 2] | (buf[word * 2 + 1] << 8);
        v = (v << 1) | ((w >> b) & 1);
    }
    return v;
}

#define PACKTEST_LOG "ML/LOGS/PACKTEST.TXT"
static void plog(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend(PACKTEST_LOG);
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void packtest_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    char line[200];
    msleep(10000);
    FILE *f0 = FIO_CreateFile(PACKTEST_LOG);
    if (f0) FIO_CloseFile(f0);

    raw_lv_request();
    msleep(1000);
    if (!raw_update_params()) { plog("raw_update_params failed"); goto end_raw; }

    const int W = 1920, H = 64;                     /* 64 lines x 1920 px from the LV frame */
    const int pitch = raw_info.pitch;
    uint8_t *src = fio_malloc(W * 14 / 8 * H);
    uint8_t *dst = fio_malloc(W * 14 / 8 * H + M2M_GUARD);
    if (!src || !dst) { plog("fio_malloc failed"); goto free_bufs; }

    /* snapshot of a real 14-bit block (CPU copy, so src is stable for all variants) */
    uint8_t *lvbuf = UNCACHEABLE(raw_info.buffer);
    for (int y = 0; y < H; y++)
        memcpy(src + y * (W * 14 / 8), lvbuf + (200 + y) * pitch + 64 * 14 / 8, W * 14 / 8);
    dcache_clean((uint32_t)CACHEABLE(src), W * 14 / 8 * H);
    snprintf(line, sizeof(line), "src block %dx%d from LV %dx%d pitch %d", W, H, raw_info.width, raw_info.height, pitch);
    plog(line);

    void *lock = rom_CreateResLockEntry(res_ids, 2);
    unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
    snprintf(line, sizeof(line), "lock %x -> %x", lock, r);
    plog(line);
    if (r & 1) { if (lock) rom_DeleteResLockEntry(lock); goto free_bufs; }
    rom_PwrMngEnableSubChip(subchips);

    /* original pack state, restored at the end */
    uint32_t o_rd24 = MEM(PACK_UNIT_RD61 + 0x24), o_wr24 = MEM(PACK_UNIT_WR24 + 0x24);
    uint32_t o_rdD0 = MEM(M2M_RD_BASE + 0xD0),    o_wrD0 = MEM(M2M_WR_BASE + 0xD0);
    snprintf(line, sizeof(line), "orig: rd61 unit+24=%x port+D0=%x | wr24 unit+24=%x port+D0=%x", o_rd24, o_rdD0, o_wr24, o_wrD0);
    plog(line);
    /* READ-ONLY: how Canon's LV RAW port 3 packs the frame it writes (unit 0xD0422200) */
    snprintf(line, sizeof(line), "port3 (LV RAW): unit+24=%x unit+30=%x port+D0=%x",
             MEM(0xD0422200 + 0x24), MEM(0xD0422200 + 0x30), MEM(0xD0420200 + 0xD0));
    plog(line);

    FILE *fs = FIO_CreateFile("ML/LOGS/PK_SRC.BIN");
    if (fs) { FIO_WriteFile(fs, UNCACHEABLE(src), W * 14 / 8 * H); FIO_CloseFile(fs); }

    /* variants: {name, bpp, rd cfg (or none), wr cfg} */
    /* Canon's LV RAW writer (port 3) uses pack reg 0x00100020 = write cfg {0,1,2,0,0} */
    static uint32_t rdA[4] = { 0, 2, 0, 0 }, rdB[4] = { 0, 2, 1, 0 };          /* reg 0x20 / 0x21 */
    static uint32_t wC14[5] = { 0, 1, 2, 0, 0 }, wC12[5] = { 0, 1, 1, 0, 0 }, wC10[5] = { 0, 1, 0, 0, 0 }; /* 0x1000x0 */
    static uint32_t wN14[5] = { 0, 0, 2, 0, 0 }, wN12[5] = { 0, 0, 1, 0, 0 }, wN10[5] = { 0, 0, 0, 0, 0 }; /* 0x000x0 */
    static uint32_t wO14[5] = { 0, 0, 2, 1, 0 };                                   /* 0x21 */
    struct { const char *name; int bpp; uint32_t *rd; uint32_t *wr; int mode; } v[] = {
        { "mode0 no pack (plain copy control)",   14, 0,    0,    0 },
        { "mode0 rd 0x20 / wr 0x100020 (14)",     14, rdA,  wC14, 0 },
        { "mode0 rd 0x20 / wr 0x20 (14)",         14, rdA,  wN14, 0 },
        { "mode0 rd 0x21 / wr 0x21 (14)",         14, rdB,  wO14, 0 },
        { "mode0 rd 0x20 / wr 0x100010 (12)",     12, rdA,  wC12, 0 },
        { "mode0 rd 0x20 / wr 0x10 (12)",         12, rdA,  wN12, 0 },
        { "mode0 rd 0x20 / wr 0x100000 (10)",     10, rdA,  wC10, 0 },
        { "mode0 rd 0x20 / wr 0x00 (10)",         10, rdA,  wN10, 0 },
    };

    for (int k = 0; k < (int)COUNT(v); k++)
    {
        int dxb = W * v[k].bpp / 8;
        memset(CACHEABLE(dst), 0xAA, W * 14 / 8 * H + M2M_GUARD);
        dcache_clean((uint32_t)CACHEABLE(dst), W * 14 / 8 * H + M2M_GUARD);
        int ms = m2m_copy_pack(src, dst, W * 14 / 8, dxb, H, v[k].rd, v[k].wr, v[k].mode);

        uint8_t *s = UNCACHEABLE(src), *d = UNCACHEABLE(dst);
        int shift = 14 - v[k].bpp, mask = (1 << v[k].bpp) - 1;
        int n = 4096, hi = 0, lo = 0;                 /* first 4096 px of line 0 */
        for (int i = 0; i < n; i++)
        {
            int sv = packed_px(s, 14, i), dv = packed_px(d, v[k].bpp, i);
            if (dv == (sv >> shift)) hi++;
            if (dv == (sv & mask))   lo++;
        }
        int guard = 0;                                /* bytes written past the expected size */
        for (int i = dxb * H; i < dxb * H + 256; i++) if (d[i] != 0xAA) guard++;
        snprintf(line, sizeof(line), "[%s] ms=%d match_top_bits=%d/%d match_low_bits=%d/%d past_end=%d px0..3 src %d %d %d %d dst %d %d %d %d",
                 v[k].name, ms, hi, n, lo, n, guard,
                 packed_px(s, 14, 0), packed_px(s, 14, 1), packed_px(s, 14, 2), packed_px(s, 14, 3),
                 packed_px(d, v[k].bpp, 0), packed_px(d, v[k].bpp, 1), packed_px(d, v[k].bpp, 2), packed_px(d, v[k].bpp, 3));
        plog(line);
        char fname[32];
        snprintf(fname, sizeof(fname), "ML/LOGS/PK_DST%d.BIN", k);
        FILE *fd = FIO_CreateFile(fname);
        if (fd) { FIO_WriteFile(fd, UNCACHEABLE(dst), dxb * H); FIO_CloseFile(fd); }
        snprintf(line, sizeof(line), "   after: rd61 unit+24=%x port+D0=%x | wr24 unit+24=%x port+D0=%x",
                 MEM(PACK_UNIT_RD61 + 0x24), MEM(M2M_RD_BASE + 0xD0), MEM(PACK_UNIT_WR24 + 0x24), MEM(M2M_WR_BASE + 0xD0));
        plog(line);
        /* a timeout is already stopped by ClearEdmac in m2m_copy_pack: try the next variant */
    }

    /* put the pack units back exactly as Canon left them */
    MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
    MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
    snprintf(line, sizeof(line), "restored: rd61 unit+24=%x port+D0=%x | wr24 unit+24=%x port+D0=%x",
             MEM(PACK_UNIT_RD61 + 0x24), MEM(M2M_RD_BASE + 0xD0), MEM(PACK_UNIT_WR24 + 0x24), MEM(M2M_WR_BASE + 0xD0));
    plog(line);

    rom_PwrMngDisableSubChip(subchips);
    rom_UnLockEngineResources(lock);
    rom_DeleteResLockEntry(lock);
    plog("done");
    NotifyBox(5000, "packtest: done. " PACKTEST_LOG);

free_bufs:
    if (src) fio_free(src);
    if (dst) fio_free(dst);
end_raw:
    raw_lv_release();
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(packtest_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("packtest_task", 0x1e, 0x4000, packtest_task, 0);
}

static int m2m_copy_once(void *src, void *dst, int size, int log_steps)
{
    return m2m_copy_rect(src, dst, M2M_XB, size / M2M_XB, log_steps);
}

/* RAW burst like rawburst_task, but each LV RAW frame is copied by EDMAC (ch61 -> ch24).
 * Port 3 is only read from (its buffer is the DMA source); nothing is written to it. */
#define DMABURST_LOG "ML/LOGS/DMABURST.TXT"

static void dlog(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend(DMABURST_LOG);
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

extern void edmac_memcpy_res_lock();
extern void edmac_memcpy_res_unlock();
extern void *edmac_copy_rectangle_cbr_start(void *dst, void *src,
                                            int src_width, int src_x, int src_y,
                                            int dst_width, int dst_x, int dst_y,
                                            int w, int h,
                                            void (*cbr_r)(void *), void (*cbr_w)(void *), void *cbr_ctx);

/* core path: one rectangle through the core API; returns 0 ok, -1 failed */
static int core_copy(void *src, void *dst, int src_w, int sx, int sy, int dst_w, int w, int h)
{
    return edmac_copy_rectangle_cbr_start(dst, src, src_w, sx, sy, dst_w, 0, 0, w, h, 0, 0, 0) ? 0 : -1;
}

/* crop copy (src_x/src_y offsets, w < pitch = what mlv_lite does) checked against the CPU */
static int core_crop_check(uint8_t *src, uint8_t *dst, int pitch, int rows, char *line, int len)
{
    int sx = 88 * 14 / 8, sy = 20, w = 1920 * 14 / 8, h = rows - 40;   /* 154 B, 3360 B per line */
    memset(CACHEABLE(dst), 0xAA, w * h + M2M_GUARD);
    dcache_clean((uint32_t)CACHEABLE(dst), w * h + M2M_GUARD);
    int ts = get_us_clock();
    int r = core_copy(src, dst, pitch, sx, sy, w, w, h);
    int dt = get_us_clock() - ts;
    uint8_t *s = UNCACHEABLE(src), *d = UNCACHEABLE(dst);
    int bad_rows = 0, first_bad = -1;
    for (int y = 0; y < h; y++)
        if (memcmp(d + y * w, s + (sy + y) * pitch + sx, w))
        { if (first_bad < 0) first_bad = y; bad_rows++; }
    int guard_bad = 0;
    for (int i = 0; i < M2M_GUARD; i++) if (d[w * h + i] != 0xAA) guard_bad++;
    snprintf(line, len, "crop %dx%d from (%d B, %d) ret=%d %d us bad_rows=%d first_bad=%d guard_bad=%d",
             w, h, sx, sy, r, dt, bad_rows, first_bad, guard_bad);
    return r == 0 && !bad_rows && !guard_bad;
}

static void dmaburst_task(int use_core)
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    static void *frames[BURST_MAX_FRAMES];
    static int t_start_us[BURST_MAX_FRAMES];
    static int t_copy_us[BURST_MAX_FRAMES];
    static int t_dma_us[BURST_MAX_FRAMES];
    char line[160];
    int n = 0, recorded = 0, raw_released = 0;
    void *lock = 0;

    msleep(10000);
    FILE *f0 = FIO_CreateFile(DMABURST_LOG);
    if (f0) FIO_CloseFile(f0);
    dlog("1 start");

    raw_lv_request();
    msleep(1000);
    if (!raw_update_params()) { dlog("2 raw_update_params failed"); goto end; }

    int size = raw_info.frame_size;
    int xb = raw_info.pitch, rows = raw_info.height;
    if (xb * rows > size) rows = size / xb;
    snprintf(line, sizeof(line), "2 frame %dx%d bpp=%d size=%d pitch=%d rows=%d src=%x",
             raw_info.width, raw_info.height, raw_info.bits_per_pixel, size, xb, rows,
             UNCACHEABLE(raw_info.buffer));
    dlog(line);

    for (n = 0; n < BURST_MAX_FRAMES; n++)
    {
        frames[n] = fio_malloc(size);
        if (!frames[n]) break;
        dcache_clean((uint32_t)CACHEABLE(frames[n]), size);
    }
    snprintf(line, sizeof(line), "3 allocated %d frames", n);
    dlog(line);
    if (n == 0) goto end;

    if (use_core)
    {
        dlog("4 core path: edmac_memcpy_res_lock()");
        edmac_memcpy_res_lock();
    }
    else
    {
        lock = rom_CreateResLockEntry(res_ids, 2);
        unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
        snprintf(line, sizeof(line), "4 lock %x -> %x", lock, r);
        dlog(line);
        if (r & 1) { if (lock) rom_DeleteResLockEntry(lock); lock = 0; goto end; }
        rom_PwrMngEnableSubChip(subchips);
    }

    /* first frame with every step logged */
    int ms;
    if (use_core)
    {
        int ts0 = get_us_clock();
        ms = core_copy(raw_info.buffer, frames[0], xb, 0, 0, xb, xb, rows) < 0 ? -1 : (int)((get_us_clock() - ts0) / 1000);
    }
    else ms = m2m_copy_rect(raw_info.buffer, frames[0], xb, rows, 1);
    snprintf(line, sizeof(line), "5 first DMA frame: %d ms", ms);
    dlog(line);
    if (ms < 0) goto unlock;

    if (use_core && n > 1)
    {
        int ok = core_crop_check(frames[0], frames[1], xb, rows, line, sizeof(line));
        dlog(line);
        dlog(ok ? "5b crop copy: PASS" : "5b crop copy: FAIL");
    }

    NotifyBox(2000, "dmaburst: recording %d frames...", n);
    int t0 = get_us_clock();
    for (int i = 0; i < n; i++)
    {
        int ts = get_us_clock();
        int ok = use_core ? core_copy(raw_info.buffer, frames[i], xb, 0, 0, xb, xb, rows)
                          : m2m_copy_rect(raw_info.buffer, frames[i], xb, rows, 0);
        int te = get_us_clock();
        t_start_us[i] = ts - t0;
        t_copy_us[i]  = ok < 0 ? -1 : te - ts;
        t_dma_us[i]   = ok < 0 ? -1 : use_core ? te - ts : m2m_cbr_us - m2m_start_us;
        if (ok < 0) break;
        recorded = i + 1;

        int next = t0 + (i + 1) * BURST_FRAME_US;
        int wait = next - get_us_clock();
        msleep(wait > 10000 ? wait / 1000 : 10);
    }
    snprintf(line, sizeof(line), "6 burst %d frames in %d ms", recorded, (get_us_clock() - t0) / 1000);
    dlog(line);
    for (int i = 0; i < recorded; i++)
    {
        snprintf(line, sizeof(line), "  frame %02d start %7d us total %7d us dma %7d us wake %6d us",
                 i, t_start_us[i], t_copy_us[i], t_dma_us[i], t_copy_us[i] - t_dma_us[i]);
        dlog(line);
    }

unlock:
    if (use_core)
    {
        edmac_memcpy_res_unlock();
        dlog("7 edmac_memcpy_res_unlock()");
    }
    else
    {
        rom_PwrMngDisableSubChip(subchips);
        snprintf(line, sizeof(line), "7 UnLock -> %x  Delete -> %x",
                 rom_UnLockEngineResources(lock), rom_DeleteResLockEntry(lock));
        dlog(line);
        lock = 0;
    }

    /* run 2 hung while saving frame 10: release LV RAW first so LV is idle during card writes */
    raw_lv_release();
    raw_released = 1;
    msleep(500);

    /* save as DNG sequence (separate folder, the CPU burst stays in ML/RAWVID) */
    FIO_CreateDirectory("ML/RAWVID2");
    int saved = 0;
    for (int i = 0; i < recorded; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "ML/RAWVID2/D%04d.DNG", i);
        NotifyBox(1000, "dmaburst: saving %d/%d", i + 1, recorded);
        snprintf(line, sizeof(line), "  saving %s", name);
        dlog(line);
        struct raw_info ri = raw_info;
        ri.buffer = frames[i];
        saved += save_dng(name, &ri) ? 1 : 0;
    }
    snprintf(line, sizeof(line), "8 saved %d/%d DNG", saved, recorded);
    dlog(line);
    NotifyBox(5000, "dmaburst: done, %d DNG in ML/RAWVID2", saved);

end:
    for (int i = 0; i < n; i++) fio_free(frames[i]);
    if (!raw_released) raw_lv_release();
    dlog("9 end");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(dmaburst_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("dmaburst_task", 0x19, 0x4000, dmaburst_task, 0);   /* mlv_lite raw_rec_task prio */
}

static MENU_SELECT_FUNC(coreburst_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("dmaburst_task", 0x19, 0x4000, dmaburst_task, (void *)1);
}

/* fills src with a pattern, dst+guard with 0xAA/0x55, copies, verifies through uncached reads */
static int m2m_copy_verify(uint8_t *src, uint8_t *dst, int size, int log_steps, char *line, int len)
{
    uint32_t *s = CACHEABLE(src);
    for (int i = 0; i < size / 4; i++) s[i] = i * 2654435761u;
    memset(CACHEABLE(dst), 0xAA, size);
    memset(CACHEABLE(dst) + size, 0x55, M2M_GUARD);
    /* write back so the DMA sees src, and no dirty dst line can overwrite its result later */
    dcache_clean((uint32_t)CACHEABLE(src), size);
    dcache_clean((uint32_t)CACHEABLE(dst), size + M2M_GUARD);

    int dt = m2m_copy_once(src, dst, size, log_steps);

    uint32_t *su = UNCACHEABLE(src), *du = UNCACHEABLE(dst);
    int bad = 0, first_bad = -1;
    for (int i = 0; i < size / 4; i++)
        if (du[i] != su[i]) { if (first_bad < 0) first_bad = i * 4; bad++; }
    uint8_t *g = UNCACHEABLE(dst + size);
    int guard_bad = 0;
    for (int i = 0; i < M2M_GUARD; i++) if (g[i] != 0x55) guard_bad++;
    snprintf(line, len, "size=%d ms=%d bad_words=%d first_bad=%d guard_bad=%d dst0=%x src0=%x",
             size, dt, bad, first_bad, guard_bad, du[0], su[0]);
    return (dt >= 0 && !bad && !guard_bad);
}

static void m2mcopy_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    char line[200];
    const int big = 4 * 1024 * 1024;
    msleep(10000);
    FILE *f0 = FIO_CreateFile(M2M_COPY_LOG);
    if (f0) FIO_CloseFile(f0);

    uint8_t *src = fio_malloc(big);
    uint8_t *dst = fio_malloc(big + M2M_GUARD);
    snprintf(line, sizeof(line), "src=%x dst=%x (unc %x / %x)", src, dst, UNCACHEABLE(src), UNCACHEABLE(dst));
    m2mc_log(line);
    if (!src || !dst) { NotifyBox(5000, "m2m copy: fio_malloc failed"); goto free_bufs; }

    void *lock = rom_CreateResLockEntry(res_ids, 2);
    snprintf(line, sizeof(line), "CreateResLockEntry -> %x", lock);
    m2mc_log(line);
    if (!lock) goto free_bufs;
    unsigned int r = rom_LockEngineResources(lock);
    snprintf(line, sizeof(line), "LockEngineResources -> %x", r);
    m2mc_log(line);
    if (r & 1) { rom_DeleteResLockEntry(lock); goto free_bufs; }
    rom_PwrMngEnableSubChip(subchips);
    m2mc_log("sub-chip 4 enabled");

    /* A: 64 KB, every step logged */
    int ok = m2m_copy_verify(src, dst, 64 * 1024, 1, line, sizeof(line));
    m2mc_log(line);
    m2mc_log(ok ? "A 64KB: PASS" : "A 64KB: FAIL");

    /* B: 4 MB x5, timed (only if A passed) */
    if (ok)
    {
        for (int k = 0; k < 5 && ok; k++)
        {
            ok = m2m_copy_verify(src, dst, big, 0, line, sizeof(line));
            m2mc_log(line);
        }
        m2mc_log(ok ? "B 4MB x5: PASS" : "B 4MB: FAIL");
    }

    rom_PwrMngDisableSubChip(subchips);
    snprintf(line, sizeof(line), "UnLock -> %x  Delete -> %x",
             rom_UnLockEngineResources(lock), rom_DeleteResLockEntry(lock));
    m2mc_log(line);
    NotifyBox(5000, ok ? "m2m copy: PASS. " M2M_COPY_LOG : "m2m copy: FAIL. " M2M_COPY_LOG);

free_bufs:
    if (src) fio_free(src);
    if (dst) fio_free(dst);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(m2mcopy_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("m2mcopy_task", 0x1e, 0x2000, m2mcopy_task, 0);
}

static MENU_SELECT_FUNC(m2mcheck_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("m2mcheck_task", 0x1e, 0x2000, m2mcheck_task, 0);
}

/* READ-ONLY: logs the mode variables mlv_lite's "movie mode" check depends on,
 * every 0.5 s for 20 s, while the user turns the mode dial (M -> movie -> M). */
extern int shooting_mode, shooting_mode_custom, lv_movie_select, video_mode_resolution, lv;

static void modeinfo_task()
{
    char line[160];
    msleep(3000);
    FILE *f0 = FIO_CreateFile("ML/LOGS/MODEINFO.TXT");
    if (f0) FIO_CloseFile(f0);
    for (int i = 0; i < 40; i++)
    {
        snprintf(line, sizeof(line), "t=%d shooting_mode=%x custom=%x lv_movie_select=%x lv=%d video_res=%d is_movie_mode=%d",
                 i * 500, shooting_mode, shooting_mode_custom, lv_movie_select, lv, video_mode_resolution, is_movie_mode());
        FILE *f = FIO_CreateFileOrAppend("ML/LOGS/MODEINFO.TXT");
        if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
        if (i % 4 == 0) NotifyBox(1500, "mode %x movie=%d (%d s)", shooting_mode, is_movie_mode(), i / 2);
        msleep(500);
    }
    NotifyBox(5000, "modeinfo: done. ML/LOGS/MODEINFO.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(modeinfo_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("modeinfo_task", 0x1e, 0x2000, modeinfo_task, 0);
}

/* READ-ONLY: samples the EOS R FPS timer block (0xD0406190 confirm, A +8, B +0x14;
 * M50 ROM has per-mode tables writing d0406198 and d04061a4=ffff). 0xD0006000 read 0.
 * Appends one block per run: run it in movie 24p / 30p / 60p and photo mode LV,
 * timer B should scale with 1/fps. Nothing is written. ML/LOGS/FPSREG2.TXT */
extern int video_mode_fps_x100;
extern int lv_vsync_fps_x1000();

static void fpsreg_task()
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/FPSREG2.TXT");
    if (f)
    {
        my_fprintf(f, "=== movie=%d video_fps_x100=%d vsync_fps_x1000=%d lv=%d\n",
                   is_movie_mode(), video_mode_fps_x100, lv_vsync_fps_x1000(), lv);
        for (int s = 0; s < 5; s++)
        {
            my_fprintf(f, "s%d:", s);
            /* EOS R: confirm 0xD0406190, timer A +8, timer B +0x14 */
            for (uint32_t a = 0xD0406180; a <= 0xD04061E0; a += 4)
                my_fprintf(f, " %08x", *(volatile uint32_t *) a);
            my_fprintf(f, " | 000:");
            for (uint32_t a = 0xD0406000; a <= 0xD0406038; a += 4)
                my_fprintf(f, " %08x", *(volatile uint32_t *) a);
            my_fprintf(f, "\n");
            msleep(200);
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "fpsreg: done (%d.%03d fps). ML/LOGS/FPSREG2.TXT",
              lv_vsync_fps_x1000() / 1000, lv_vsync_fps_x1000() % 1000);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(fpsreg_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("fpsreg_task", 0x1e, 0x2000, fpsreg_task, 0);
}

/* READ-ONLY: Canon's sensor-drive RAM state behind the photo-LV 24p dark band.
 * ROM: FUN_e044fe7a returns m_VSize = *(0xef24 + 0x10); FUN_e00db6dc computes
 * "Accum MicroSec[VD:%d AC:%d]" from u16 VD = *(0xa3594), u16 AC = *(0xa3594 + 0xe)
 * times *(0x123c8 + 0x7c), and starts the exposure at VD - AC. Canon's VD stays
 * 60p when we stretch timer B. Run in movie 24/30/60 and photo M at 2 ISOs.
 * Nothing is written. ML/LOGS/FPSRAM2.TXT */
static void fpsram_dump(FILE *f, const char *name, uint32_t addr, int words)
{
    my_fprintf(f, "  %s %x:", name, addr);
    for (int i = 0; i < words; i++)
        my_fprintf(f, " %08x", ((volatile uint32_t *) addr)[i]);
    my_fprintf(f, "\n");
}

static void fpsram_task()
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/FPSRAM2.TXT");
    if (f)
    {
        my_fprintf(f, "=== movie=%d video_fps_x100=%d vsync_fps_x1000=%d iso=%d shutter=%d timerA=%x timerB=%x\n",
                   is_movie_mode(), video_mode_fps_x100, lv_vsync_fps_x1000(),
                   lens_info.raw_iso, lens_info.raw_shutter,
                   *(volatile uint32_t *) 0xD0406198, *(volatile uint32_t *) 0xD04061A4);
        for (int s = 0; s < 3; s++)
        {
            volatile uint16_t *vd = (volatile uint16_t *) 0xa3594;
            my_fprintf(f, "s%d: m_VSize=%d VD=%d AC=%d us_factor=%d\n", s,
                       *(volatile uint32_t *) (0xef24 + 0x10), vd[0], vd[7],
                       *(volatile uint32_t *) (0x123c8 + 0x7c));
            fpsram_dump(f, "vsz", 0xef24, 16);
            fpsram_dump(f, "vd ", 0xa3594, 16);
            fpsram_dump(f, "seq", 0x123c8, 32);
            /* ESM/ESync/ESyncControl.c: struct 0x1770c (+0x24 BaseVD FrameRatePtrn,
             * table +0x20*0x40 + rate*0x10 + 0x30), data 0xb041c (fields at -0xa8..-0x14) */
            fpsram_dump(f, "esy", 0x1770c, 80);
            fpsram_dump(f, "esd", 0xb0374, 44);
            msleep(300);
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "fpsram: done. ML/LOGS/FPSRAM2.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(fpsram_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("fpsram_task", 0x1e, 0x2000, fpsram_task, 0);
}

/* FIRST WRITE TEST (2 signs: ROM tables + FPSREG2 read: B = 0x617 / 0x4df / 0x26f
 * at 23.976 / 29.97 / 59.94, A = 0x2d1 everywhere, 27 MHz).
 * Photo mode LV only: timer B 0x26f -> 0x617 (movie 24p value), confirm via
 * 0xD0406190 = 1 (EOS R CONFIRM_CHANGES), measure fps, then restore. ML/LOGS/FPS24.TXT */
#define M50_FPS_CONFIRM 0xD0406190
#define M50_FPS_TIMER_A 0xD0406198
#define M50_FPS_TIMER_B 0xD04061A4

/* run 1: a single write was undone by Canon after ~1 frame (fps dip 59.03, B read 26f)
 * -> run 2: rewrite B after Canon on every frame, from both EvfState hooks */
static volatile int fps24_active = 0;
static volatile int fps24_setparam_count = 0;
static volatile int fps24_vsync_count = 0;

/* run 3 (dark band fix): also give Canon's sensor drive the movie-24p frame length,
 * so it schedules the exposure start (VD - AC) for 41.7 ms frames.
 * FPSRAM.TXT movie 24p vs photo LV: m_VSize 0xef24+0x10 = 0x618 vs 0x270,
 * 0xef24+0x1c = VSize-38 (0x5f2 vs 0x24a), VD 0xa3594 = 0x618 vs 0x270,
 * 0xa3594+8 = VSize-38. Values below = exactly Canon's movie 24p values. */
#define M50_RAM_VSIZE      (0xef24 + 0x10)
#define M50_RAM_VSIZE_MAX  (0xef24 + 0x1c)
#define M50_RAM_VD         (0xa3594)
#define M50_RAM_VD_MAX     (0xa3594 + 8)
/* run 4: also switch the ESync BaseVD kick pattern (ESM_VD_KICK_BASE_FR handler
 * FUN_e081b248 reads ESync+0x24 on every kick; FPSRAM2: 2 @ movie 24p, 1 @ 30p,
 * 0 in photo LV). fps24_vd: 1 = run 3 (B + VSize/VD), 2 = run 4 (+ rate 2) */
#define M50_RAM_ESYNC_RATE (0x1770c + 0x24)
static volatile int fps24_vd = 0;
static uint32_t fps24_ram_orig[5];

static void fps24_write()
{
    *(volatile uint32_t *) M50_FPS_TIMER_B = 0x617;
    *(volatile uint32_t *) M50_FPS_CONFIRM = 1;
    if (fps24_vd)
    {
        *(volatile uint32_t *) M50_RAM_VSIZE = 0x618;
        *(volatile uint32_t *) M50_RAM_VSIZE_MAX = 0x5f2;
        *(volatile uint32_t *) M50_RAM_VD = 0x618;
        *(volatile uint32_t *) M50_RAM_VD_MAX = 0x5f2;
    }
    if (fps24_vd == 2)
    {
        *(volatile uint32_t *) M50_RAM_ESYNC_RATE = 2;
    }
}

/* READ-ONLY FrameData timing log. evfSetParamInterrupt (FUN_e0057874, EvfState.c) gets
 * FrameData = *(0x7180 + 0x40) (FUN_e0540e9a), u16 [0] ISO, [4] Acc ("FA_ContinousForTg
 * Acc %lx ISO"), [6] AccumHForFrame0, and passes it to SetTgParam (FUN_e00da8b4), which
 * copies Acc into the VD struct (0xa3594+0xe). Our CBRs run after that transition, so
 * log FrameData at both hooks to see when Canon refills it relative to SetParam. */
#define M50_FRAMEDATA_PTR (0x7180 + 0x40)
#define FDLOG_N 96
struct fdlog_entry { uint32_t us; uint8_t which; uint32_t ptr; uint16_t fd[8]; uint16_t vd_ac; };
static struct fdlog_entry fdlog[FDLOG_N];
static volatile int fdlog_pos = -1;   /* -1 = off */

static void fdlog_add(int which)
{
    int i = fdlog_pos;
    if (i < 0 || i >= FDLOG_N) return;
    struct fdlog_entry *e = &fdlog[i];
    e->us = get_us_clock();
    e->which = which;
    e->ptr = *(volatile uint32_t *) M50_FRAMEDATA_PTR;
    for (int k = 0; k < 8; k++)
        e->fd[k] = (e->ptr > 0x1000 && e->ptr < 0x40000000) ? ((volatile uint16_t *) e->ptr)[k] : 0;
    e->vd_ac = ((volatile uint16_t *) 0xa3594)[7];
    fdlog_pos = i + 1;
}

/* WRITE test of TG register 0xD04115F0/F4 (sign 1: ROM default 0x1b88 in 8 mode tables;
 * sign 2: TGSNAP photo60 f/4 0x1b88 vs f/16 0x1930, movie 24p 0x15ec, photo24 0x1939,
 * changes between frames). FrameData / AC overrides did not change the RAW exposure
 * (angle_sweep1, expcode1) -> try the TG side. Written every frame while tgw_value != 0. */
#define M50_TG_15F0 0xD04115F0
#define M50_TG_15F4 0xD04115F4
static volatile uint32_t tgw_value = 0;

static void tgw_write()
{
    if (!tgw_value) return;
    *(volatile uint32_t *) M50_TG_15F0 = tgw_value;
    *(volatile uint32_t *) M50_TG_15F4 = tgw_value;
}

static unsigned int fps24_setparam_cbr(unsigned int unused)
{
    if (fps24_active) { fps24_write(); fps24_setparam_count++; }
    tgw_write();
    fdlog_add(0);
    return CBR_RET_CONTINUE;
}

static unsigned int fps24_vsync_cbr(unsigned int unused)
{
    if (fps24_active) { fps24_write(); fps24_vsync_count++; }
    tgw_write();
    fdlog_add(1);
    return 0;
}

static void fdlog_task()
{
    msleep(3000);
    fdlog_pos = 0;
    for (int t = 0; t < 100 && fdlog_pos < FDLOG_N; t++) msleep(50);
    int n = fdlog_pos;
    fdlog_pos = -1;
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/FRAMEDAT.TXT");
    if (f)
    {
        my_fprintf(f, "=== movie=%d iso=%d shutter=%d timerB=%x n=%d\n", is_movie_mode(),
                   lens_info.raw_iso, lens_info.raw_shutter, *(volatile uint32_t *) 0xD04061A4, n);
        my_fprintf(f, "  7180:");
        for (int k = 0; k < 24; k++) my_fprintf(f, " %08x", ((volatile uint32_t *) 0x7180)[k]);
        my_fprintf(f, "\n");
        for (int i = 0; i < n; i++)
        {
            struct fdlog_entry *e = &fdlog[i];
            my_fprintf(f, "%8d %s ptr=%x fd=%x %x %x %x %x %x %x %x vdAC=%d\n",
                       e->us - fdlog[0].us, e->which ? "VS" : "SP", e->ptr,
                       e->fd[0], e->fd[1], e->fd[2], e->fd[3], e->fd[4], e->fd[5], e->fd[6], e->fd[7],
                       e->vd_ac);
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "framedata: done (%d). ML/LOGS/FRAMEDAT.TXT", n);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(fdlog_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("fdlog_task", 0x1e, 0x2000, fdlog_task, 0);
}

/* READ-ONLY: snapshot of every TG/SSG/sensor-interface register Canon programs from its
 * per-mode tables (tg_regs.h, 655 addresses). Our timer B / RAM overrides give real 24p
 * frames but not the exposure (angle_sweep1: same RAW brightness at 90/180/270 deg), so
 * the exposure must be in one of these. Compare photo LV at two apertures (Canon AE moves
 * the exposure) with movie 24p. 3 samples; later samples list only changed registers. */
#include "tg_regs.h"
#define TG_N ((int) COUNT(tg_regs))
static uint32_t tg_snap[3][TG_N];

static void tgsnap_task()
{
    msleep(3000);
    for (int s = 0; s < 3; s++)
    {
        for (int i = 0; i < TG_N; i++)
            tg_snap[s][i] = *(volatile uint32_t *) tg_regs[i];
        msleep(300);
    }
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/TGSNAP.TXT");
    if (f)
    {
        uint32_t fd = *(volatile uint32_t *) M50_FRAMEDATA_PTR;
        my_fprintf(f, "=== movie=%d vsync_fps_x1000=%d iso=%d shutter=%d aperture=%d AC=%d fd4=%x n=%d\n",
                   is_movie_mode(), lv_vsync_fps_x1000(), lens_info.raw_iso, lens_info.raw_shutter,
                   lens_info.raw_aperture, ((volatile uint16_t *) 0xa3594)[7],
                   (fd >= 0x1000 && fd < 0x40000000) ? ((volatile uint16_t *) fd)[4] : 0, TG_N);
        for (int i = 0; i < TG_N; i++)
        {
            my_fprintf(f, "%08x %08x", tg_regs[i], tg_snap[0][i]);
            if (tg_snap[1][i] != tg_snap[0][i] || tg_snap[2][i] != tg_snap[0][i])
                my_fprintf(f, " ~ %08x %08x", tg_snap[1][i], tg_snap[2][i]);
            my_fprintf(f, "\n");
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "tgsnap: done. ML/LOGS/TGSNAP.TXT");
    rawdump_running = 0;
}

/* mean of a pixel grid of the live LV raw frame, rows y0..y1 of the active area (black subtracted) */
static int tgw_meter(int y0, int y1)
{
    int sum = 0, n = 0;
    int ax = raw_info.active_area.x1, ay = raw_info.active_area.y1;
    for (int y = y0; y < y1; y += 16)
        for (int x = 100; x < raw_info.active_area.x2 - ax - 100; x += 64)
        {
            sum += raw_get_pixel(ax + x, ay + y) - raw_info.black_level;
            n++;
        }
    return n ? sum / n : 0;
}

static void tgw_task()
{
    static const uint32_t phases[] = { 0, 0x0c00, 0x1b88, 0x2a00, 0 };
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/TGW.TXT");
    if (!f) { rawdump_running = 0; return; }
    my_fprintf(f, "=== movie=%d lv=%d timerB=%x vsync_fps_x1000=%d\n", is_movie_mode(), lv,
               *(volatile uint32_t *) 0xD04061A4, lv_vsync_fps_x1000());
    if (!lv || is_movie_mode())
    {
        my_fprintf(f, "abort: photo LV only\n");
        FIO_CloseFile(f);
        NotifyBox(5000, "tgw: abort (photo mode M, LiveView)");
        rawdump_running = 0;
        return;
    }
    raw_lv_request();
    msleep(500);
    if (!raw_update_params())
    {
        my_fprintf(f, "abort: raw_update_params failed\n");
        raw_lv_release();
        FIO_CloseFile(f);
        rawdump_running = 0;
        return;
    }
    for (int p = 0; p < (int) COUNT(phases); p++)
    {
        tgw_value = phases[p];
        msleep(1200);
        int top = 0, bottom = 0;
        for (int k = 0; k < 3; k++)
        {
            top += tgw_meter(100, 400);
            bottom += tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90,
                                raw_info.active_area.y2 - raw_info.active_area.y1 - 10);
            msleep(60);
        }
        my_fprintf(f, "phase %d write=%x read=%x/%x timerB=%x fps=%d top=%d bottom=%d\n", p, phases[p],
                   *(volatile uint32_t *) M50_TG_15F0, *(volatile uint32_t *) M50_TG_15F4,
                   *(volatile uint32_t *) 0xD04061A4, lv_vsync_fps_x1000(), top / 3, bottom / 3);
    }
    tgw_value = 0;
    raw_lv_release();
    FIO_CloseFile(f);
    NotifyBox(5000, "tgw: done. ML/LOGS/TGW.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(tgw_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("tgw_task", 0x1e, 0x2000, tgw_task, 0);
}

/* READ-ONLY: the CMOS register lists Canon sends to the sensor over SIO0 (FUN_e0824d02,
 * "[REG] CMOS:[0x%08x]"). Entries are (reg << 22) | value, list ends with 0xffffffff.
 * ImageSequenceLv.c, base struct 0xeedc, P = *(0xeedc + 0x18):
 *  - FUN_e08340b8 ("3-3 RODone"): P + mode*0x180 + 0x2314 .. regs 0x41-0x46, 0x50 = VSize,
 *    0x9d = VSize-2/3 (sensor-side frame length)
 *  - FUN_e0834604 -> FUN_e08342a8 ("3-4 PreAccumH AccumH VD"): P + mode*0x6c + 0x2f14,
 *    [0] reg 0x29 = VSize - AccumH + 1 (exposure start), [1] reg 0x2a
 *  AccumH = m_ShutterSpeed (0xef44), VSize = m_VSize (0xef34). Lists are built and sent only
 *  at LV start / mode change / LV_FramerateChg, so photo LV keeps 60p values in the sensor.
 *  mode = *(0xe0b3ab64 + SensorMode*4 + 0xa8), SensorMode = *(0xef24 + 8) (FUN_e045002a). */
static void cmosl_dump_list(FILE *f, const char *name, uint32_t addr, int max)
{
    my_fprintf(f, "  %s %x:", name, addr);
    for (int i = 0; i < max; i++)
    {
        uint32_t w = ((volatile uint32_t *) addr)[i];
        if (w == 0xffffffff) { my_fprintf(f, " END"); break; }
        my_fprintf(f, " %02x=%x", w >> 22, w & 0x3fffff);
    }
    my_fprintf(f, "\n");
}

static void cmosl_task()
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/CMOSL.TXT");
    if (f)
    {
        uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
        uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
        uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
        my_fprintf(f, "=== movie=%d fps_x1000=%d timerB=%x m_VSize=%x m_Shutter=%x SensorMode=%d mode=%d P=%x\n",
                   is_movie_mode(), lv_vsync_fps_x1000(), *(volatile uint32_t *) 0xD04061A4,
                   *(volatile uint32_t *) 0xef34, *(volatile uint32_t *) 0xef44, smode, mode, P);
        /* P is in the uncached RAM alias (CMOSL run 1: 0x405fccac) */
        if (P > 0x1000 && P < 0x60000000 && mode < 14)
        {
            cmosl_dump_list(f, "vsync(cur)", P + mode * 0x180 + 0x2314, 96);
            cmosl_dump_list(f, "accum(cur)", P + mode * 0x6c + 0x2f14, 27);
            /* movie 1080p 24p was mode 3, photo LV mode 4 (CMOSL run 1) */
            cmosl_dump_list(f, "vsync(m3)", P + 3 * 0x180 + 0x2314, 96);
            cmosl_dump_list(f, "accum(m3)", P + 3 * 0x6c + 0x2f14, 27);
            cmosl_dump_list(f, "vsync(m4)", P + 4 * 0x180 + 0x2314, 96);
            cmosl_dump_list(f, "accum(m4)", P + 4 * 0x6c + 0x2f14, 27);
            for (int m = 0; m < 14; m++)
            {
                uint32_t *v = (uint32_t *) (P + m * 0x180 + 0x2314);
                uint32_t *a = (uint32_t *) (P + m * 0x6c + 0x2f14);
                my_fprintf(f, "  mode %2d: 50=%x 9d=%x 41=%x 46=%x | 29=%x 2a=%x\n", m,
                           v[(0x2344 - 0x2314) / 4] & 0x3fffff, v[(0x23f4 - 0x2314) / 4] & 0x3fffff,
                           v[(0x2320 - 0x2314) / 4] & 0x3fffff, v[(0x2334 - 0x2314) / 4] & 0x3fffff,
                           a[0] & 0x3fffff, a[1] & 0x3fffff);
            }
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "cmosl: done. ML/LOGS/CMOSL.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(cmosl_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("cmosl_task", 0x1e, 0x2000, cmosl_task, 0);
}

/* WRITE test: give the SENSOR the movie-24p frame length. CMOSL run 2 (raw_tests/cmosl2):
 * accum list of movie 24p (mode 3): 50=618 9d=616 29=336; photo LV (mode 4): 50=270 9d=26e
 * 29=ff. 29 = VSize - AccumH + 9 checks exactly in both (1560-747+9, 624-378+9) -> sign 2.
 * Sign 1: FUN_e08342a8 / FUN_e08340b8 build these words, Canon sends them with the SIO0
 * CMOS driver FUN_e0824d02 ("[REG] CMOS:[0x%08x]"), list format (reg << 22) | value,
 * end 0xffffffff. Photo LV never resends them, so the sensor stays on its 624-line frame
 * while timer B stretches the frame to 1560 lines. Needs mlv_lite FPS 23.976 (timer B). */
#define M50_CMOS_WRITE_LIST 0xe0824d03   /* Thumb */
#define CMOS_W(reg, val) ((((uint32_t) (reg)) << 22) | ((val) & 0x3fffff))
static uint32_t cmos24_list[8];

static void cmos24_send3(uint32_t vsize, uint32_t r9d, uint32_t r29)
{
    cmos24_list[0] = CMOS_W(0x50, vsize);
    cmos24_list[1] = CMOS_W(0x9d, r9d);
    cmos24_list[2] = CMOS_W(0x29, r29);
    cmos24_list[3] = 0xffffffff;
    ((void (*)(uint32_t *)) M50_CMOS_WRITE_LIST)(cmos24_list);
}

static void cmos24_task()
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/CMOS24.TXT");
    if (!f) { rawdump_running = 0; return; }

    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    int b24 = 0;
    for (int i = 0; i < 20; i++) { if (*(volatile uint32_t *) 0xD04061A4 == 0x617) b24++; msleep(5); }
    my_fprintf(f, "=== movie=%d lv=%d mode=%d P=%x timerB 617 seen %d/20 fps=%d\n",
               is_movie_mode(), lv, mode, P, b24, lv_vsync_fps_x1000());

    /* Canon's current sensor values from the photo LV accum list */
    uint32_t o50 = 0, o9d = 0, o29 = 0;
    if (P > 0x1000 && P < 0x60000000 && mode == 4)
    {
        volatile uint32_t *l = (volatile uint32_t *) (P + mode * 0x6c + 0x2f14);
        for (int i = 0; i < 27 && l[i] != 0xffffffff; i++)
        {
            if ((l[i] >> 22) == 0x50) o50 = l[i] & 0x3fffff;
            if ((l[i] >> 22) == 0x9d) o9d = l[i] & 0x3fffff;
            if ((l[i] >> 22) == 0x29) o29 = l[i] & 0x3fffff;
        }
    }
    my_fprintf(f, "Canon list: 50=%x 9d=%x 29=%x\n", o50, o9d, o29);
    if (!lv || is_movie_mode() || mode != 4 || o50 != 0x270 || !o9d || !o29 || b24 == 0)
    {
        my_fprintf(f, "abort: need photo LV mode 4 (50=270) with mlv_lite 23.976 active\n");
        FIO_CloseFile(f);
        NotifyBox(5000, "cmos24: abort (photo M + Photo mode LV FPS 23.976)");
        rawdump_running = 0;
        return;
    }

    raw_lv_request();
    msleep(500);
    raw_update_params();

    /* 0: Canon, 1: 180 deg, 2: 90 deg, 3: 270 deg, 4: Canon values back */
    static const int accum[] = { 0, 780, 390, 1170, 0 };
    for (int p = 0; p < 5; p++)
    {
        if (p >= 1 && p <= 3)
            cmos24_send3(0x618, 0x616, 0x618 - accum[p] + 9);
        else if (p == 4)
            cmos24_send3(o50, o9d, o29);
        msleep(1500);
        int top = 0, bottom = 0;
        for (int k = 0; k < 3; k++)
        {
            top += tgw_meter(100, 400);
            bottom += tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90,
                                raw_info.active_area.y2 - raw_info.active_area.y1 - 10);
            msleep(60);
        }
        my_fprintf(f, "phase %d accum=%d sent29=%x fps=%d top=%d bottom=%d\n", p, accum[p],
                   (p >= 1 && p <= 3) ? 0x618 - accum[p] + 9 : (p == 4 ? o29 : 0),
                   lv_vsync_fps_x1000(), top / 3, bottom / 3);
    }
    raw_lv_release();
    FIO_CloseFile(f);
    NotifyBox(5000, "cmos24: done. ML/LOGS/CMOS24.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(cmos24_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("cmos24_task", 0x1e, 0x2000, cmos24_task, 0);
}

/* WRITE test (session 8, v2): Open Gate with the movie FHD 3x3 sensor readout.
 * Run 1 (raw_tests/og33_1) aborted at the gate: the live photo list (mode 4) is 64=211012 65=912,
 * not 222203/202502 -> list mode = SensorMode - 3 (ROM 0xe0b3ab64+0xa8), ROM vsync lists at
 * 0xe19e2fc0 + mode*0x180 are in that order (RAM mode 3 / 4 match ROM blocks 3 / 4 exactly).
 * Known readouts: mode 3 = movie FHD 24p, 3x3, 1164 lines (accum 93 = 23c = 1164/2 - 10, 68 =
 * 5c756 = 16:9 band); mode 4 = photo LV, 3x5, 838 lines, full height (68 = e7f5, 93 = 199).
 * Register meaning is not decoded, so do not guess single registers: send Canon's OWN movie
 * mode-3 vsync list (the 3x3 readout setup), then Canon's own photo mode-4 accum list (24p frame
 * length 50=618 9d=616, exposure 29, full-height window 68=e7f5). Canon sends lists in the same
 * order (vsync, then accum). Restore = Canon's photo mode-4 vsync list + accum list unchanged.
 * Phase B adds 93=2b0 (~1397 lines, unverified meaning). The DIGIC still captures 838 lines, so
 * success = the top ~60 % of the scene, NOT squeezed. Needs mlv_lite photo LV 23.976. */
#define OG33_MAX 100
static uint32_t og33_v3[OG33_MAX], og33_v4[OG33_MAX], og33_a4[32], og33_tmp[OG33_MAX + 4];

static int og33_copy(uint32_t *dst, uint32_t src, int max)
{
    volatile uint32_t *s = (volatile uint32_t *) src;
    for (int i = 0; i < max - 1; i++)
    {
        dst[i] = s[i];
        if (s[i] == 0xffffffff) return i;
    }
    dst[max - 1] = 0xffffffff;
    return -1;   /* no end marker found */
}

static uint32_t og33_get(const uint32_t *l, uint32_t reg)
{
    for (int i = 0; l[i] != 0xffffffff; i++) if ((l[i] >> 22) == reg) return l[i] & 0x3fffff;
    return 0xffffffff;
}

static void og33_send_list(const uint32_t *l, uint32_t extra)
{
    int k = 0;
    for (; l[k] != 0xffffffff && k < OG33_MAX; k++) og33_tmp[k] = l[k];
    if (extra) og33_tmp[k++] = extra;
    og33_tmp[k] = 0xffffffff;
    ((void (*)(uint32_t *)) M50_CMOS_WRITE_LIST)(og33_tmp);
}

static void og33_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/OG33.TXT");
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void og33_task()
{
    char buf[256];
    msleep(3000);

    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;

    /* copies of Canon's live lists: movie mode 3 vsync, photo mode 4 vsync + accum */
    int n3 = -1, n4 = -1, na = -1;
    if (P > 0x1000 && P < 0x60000000 && mode == 4)
    {
        n3 = og33_copy(og33_v3, P + 3 * 0x180 + 0x2314, OG33_MAX);
        n4 = og33_copy(og33_v4, P + 4 * 0x180 + 0x2314, OG33_MAX);
        na = og33_copy(og33_a4, P + 4 * 0x6c + 0x2f14, 32);
    }
    uint32_t a50 = (na > 0) ? og33_get(og33_a4, 0x50) : 0, a68 = (na > 0) ? og33_get(og33_a4, 0x68) : 0;
    snprintf(buf, sizeof(buf), "=== OG33 v2 movie=%d lv=%d SensorMode=%d mode=%d P=%x fps=%d timerB=%x n3=%d n4=%d na=%d accum 50=%x 68=%x 93=%x 29=%x",
             is_movie_mode(), lv, smode, mode, P, lv_vsync_fps_x1000(), *(volatile uint32_t *) 0xD04061A4,
             n3, n4, na, a50, a68, (na > 0) ? og33_get(og33_a4, 0x93) : 0, (na > 0) ? og33_get(og33_a4, 0x29) : 0);
    og33_log(buf);
    if (n3 > 0 && n4 > 0)
    {
        snprintf(buf, sizeof(buf), "m3: 2c=%x 51=%x 63=%x 64=%x 65=%x 66=%x 68=%x | m4: 2c=%x 51=%x 63=%x 64=%x 65=%x 66=%x 68=%x",
                 og33_get(og33_v3, 0x2c), og33_get(og33_v3, 0x51), og33_get(og33_v3, 0x63), og33_get(og33_v3, 0x64),
                 og33_get(og33_v3, 0x65), og33_get(og33_v3, 0x66), og33_get(og33_v3, 0x68),
                 og33_get(og33_v4, 0x2c), og33_get(og33_v4, 0x51), og33_get(og33_v4, 0x63), og33_get(og33_v4, 0x64),
                 og33_get(og33_v4, 0x65), og33_get(og33_v4, 0x66), og33_get(og33_v4, 0x68));
        og33_log(buf);
    }

    /* gate: photo LV mode 4 with Canon's photo list (skip 5), movie list (3x3) as in ROM, 24p */
    int fps = lv_vsync_fps_x1000();
    if (!lv || is_movie_mode() || mode != 4 || n3 < 40 || n4 < 40 || na < 10
        || og33_get(og33_v4, 0x64) != 0x211012 || og33_get(og33_v4, 0x65) != 0x912
        || og33_get(og33_v3, 0x64) != 0x222204 || og33_get(og33_v3, 0x65) != 0x303703
        || a50 != 0x618 || a68 != 0xe7f5 || fps < 23000 || fps > 25000)
    {
        og33_log("abort: need photo M + mlv_lite Photo mode LV FPS 23.976; lists must match ROM (m4 211012/912, m3 222204/303703, accum 50=618 68=e7f5)");
        NotifyBox(5000, "og33: abort (photo M + mlv_lite 23.976). ML/LOGS/OG33.TXT");
        rawdump_running = 0;
        return;
    }

    raw_lv_request();
    msleep(500);
    raw_update_params();
    snprintf(buf, sizeof(buf), "raw %dx%d active %d,%d-%d,%d frame_size %d buffer %x",
             raw_info.width, raw_info.height, raw_info.active_area.x1, raw_info.active_area.y1,
             raw_info.active_area.x2, raw_info.active_area.y2, raw_info.frame_size, raw_info.buffer);
    og33_log(buf);

    int size = raw_info.frame_size;
    void *mem[3] = { fio_malloc(size + 64), fio_malloc(size + 64), fio_malloc(size + 64) };
    void *cp[3];
    for (int i = 0; i < 3; i++) cp[i] = mem[i] ? (void *)(((uintptr_t) mem[i] + 63) & ~63) : 0;
    snprintf(buf, sizeof(buf), "copies %x %x %x", mem[0], mem[1], mem[2]);
    og33_log(buf);

    /* phase 0: Canon (skip 5) */
    if (cp[0]) memcpy(cp[0], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase 0 Canon: fps=%d top=%d bottom=%d", lv_vsync_fps_x1000(),
             tgw_meter(100, 400), tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90,
                                            raw_info.active_area.y2 - raw_info.active_area.y1 - 10));
    og33_log(buf);

    /* phase A: Canon's movie 3x3 readout list, then Canon's photo accum list (24p, exposure, full height) */
    og33_log("phase A: sending m3 vsync + m4 accum");
    og33_send_list(og33_v3, 0);
    og33_send_list(og33_a4, 0);
    msleep(1000);
    if (cp[1]) memcpy(cp[1], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase A m3 vsync + m4 accum: fps=%d timerB=%x top=%d bottom=%d",
             lv_vsync_fps_x1000(), *(volatile uint32_t *) 0xD04061A4, tgw_meter(100, 400),
             tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90,
                       raw_info.active_area.y2 - raw_info.active_area.y1 - 10));
    og33_log(buf);

    /* phase B: same + 93=2b0 (~1397 lines if 93 = lines/2 - 10) */
    og33_send_list(og33_v3, 0);
    og33_send_list(og33_a4, CMOS_W(0x93, 0x2b0));
    msleep(1000);
    if (cp[2]) memcpy(cp[2], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase B + 93=2b0: fps=%d timerB=%x top=%d bottom=%d",
             lv_vsync_fps_x1000(), *(volatile uint32_t *) 0xD04061A4, tgw_meter(100, 400),
             tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90,
                       raw_info.active_area.y2 - raw_info.active_area.y1 - 10));
    og33_log(buf);

    /* restore: Canon's own photo lists, unchanged */
    og33_send_list(og33_v4, 0);
    og33_send_list(og33_a4, 0);
    msleep(1000);
    snprintf(buf, sizeof(buf), "restored: fps=%d timerB=%x top=%d", lv_vsync_fps_x1000(),
             *(volatile uint32_t *) 0xD04061A4, tgw_meter(100, 400));
    og33_log(buf);
    raw_lv_release();

    /* save the three frames (save_dng byte-swaps the buffer in place) */
    static const char *names[3] = { "ML/LOGS/OG33_0.DNG", "ML/LOGS/OG33_A.DNG", "ML/LOGS/OG33_B.DNG" };
    for (int i = 0; i < 3; i++)
    {
        if (!cp[i]) continue;
        NotifyBox(3000, "og33: saving DNG %d of 3...", i + 1);
        struct raw_info ri = raw_info;
        ri.buffer = cp[i];
        int rc = save_dng((char *) names[i], &ri);
        snprintf(buf, sizeof(buf), "%s save_dng=%d", names[i], rc);
        og33_log(buf);
    }
    for (int i = 0; i < 3; i++) if (mem[i]) fio_free(mem[i]);
    og33_log("done");
    NotifyBox(5000, "og33: done. ML/LOGS/OG33.TXT + 3 DNG");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(og33_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("og33_task", 0x1e, 0x4000, og33_task, 0);
}

/* READ-ONLY (session 8): which Canon sensor setup is running. ROM (research/opengate/
 * SENSORMODE_SELECTION.md): SetTgParam FUN_e00da8b4 copies Canon's seq mode / rate to the ctrl
 * struct 0x123c8 (+0x60 / +0x64) and writes the VD struct 0xa3594 (+0x24 SensorMode from
 * FUN_e00da4d0, +0x28 rate code); ImgSeqCoop m_SensorMode = *(0xef24+8); CMOS list mode =
 * *(0xe0b3ab64 + SensorMode*4 + 0xa8). Run in photo M, movie FHD 23.976 and photo x5. */
static void smi_dump(FILE *f, const char *name, uint32_t addr, int words)
{
    my_fprintf(f, "  %s %x:", name, addr);
    for (int i = 0; i < words; i++)
    {
        if (i % 8 == 0) my_fprintf(f, "\n    +%02x:", i * 4);
        my_fprintf(f, " %08x", ((volatile uint32_t *) addr)[i]);
    }
    my_fprintf(f, "\n");
}

static uint32_t smi_list_get(uint32_t addr, int max, uint32_t reg)
{
    volatile uint32_t *l = (volatile uint32_t *) addr;
    for (int i = 0; i < max && l[i] != 0xffffffff; i++) if ((l[i] >> 22) == reg) return l[i] & 0x3fffff;
    return 0xffffffff;
}

/* remembered from a "Sensor mode info" run in movie 4K (SensorMode 9) in this boot:
 * LV raw buffer address and EDMAC port 3 rows, used by og3_task as a buffer-size proof */
static uint32_t og4k_addr = 0, og4k_rows = 0;

static void smi_task()
{
    msleep(3000);
    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    if (*(volatile uint32_t *) (0xef24 + 8) == 12)   /* movie 4K = SensorMode 12 (og3_1 run) */
    {
        og4k_addr = *(volatile uint32_t *) 0xD04202A0;
        og4k_rows = (*(volatile uint32_t *) 0xD0420250 >> 16) + 1;
    }
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/SMODE.TXT");
    if (f)
    {
        uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
        uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
        uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
        my_fprintf(f, "=== SMODE movie=%d lv=%d dispsize=%d res=%d fps_x100=%d vsync_fps=%d timerA=%x timerB=%x\n",
                   is_movie_mode(), lv, lv_dispsize, video_mode_resolution, video_mode_fps_x100,
                   lv_vsync_fps_x1000(), *(volatile uint32_t *) 0xD0406198, *(volatile uint32_t *) 0xD04061A4);
        my_fprintf(f, "  ctrl 0x123c8: seq(+60)=%d rate(+64)=%d mode(+c)=%d rate16(+2)=%d | VD 0xa3594: VD=%x AC=%x SensorMode(+24)=%d +28=%d | m_SensorMode=%d list mode=%d P=%x\n",
                   *(volatile uint32_t *) (0x123c8 + 0x60), *(volatile uint32_t *) (0x123c8 + 0x64),
                   *(volatile uint32_t *) (0x123c8 + 0xc), *(volatile uint16_t *) (0x123c8 + 2),
                   *(volatile uint16_t *) 0xa3594, *(volatile uint16_t *) (0xa3594 + 0xe),
                   *(volatile uint32_t *) (0xa3594 + 0x24), *(volatile uint32_t *) (0xa3594 + 0x28),
                   smode, mode, P);
        my_fprintf(f, "  raw ok=%d %dx%d active %d,%d-%d,%d pitch %d\n", ok, raw_info.width, raw_info.height,
                   raw_info.active_area.x1, raw_info.active_area.y1, raw_info.active_area.x2,
                   raw_info.active_area.y2, raw_info.pitch);
        if (P > 0x1000 && P < 0x60000000 && mode < 11)
        {
            uint32_t v = P + mode * 0x180 + 0x2314, a = P + mode * 0x6c + 0x2f14;
            my_fprintf(f, "  vsync list: 2c=%x 50=%x 51=%x 64=%x 65=%x 68=%x 93=%x 9d=%x | accum: 29=%x 50=%x 68=%x 93=%x 9d=%x\n",
                       smi_list_get(v, 96, 0x2c), smi_list_get(v, 96, 0x50), smi_list_get(v, 96, 0x51),
                       smi_list_get(v, 96, 0x64), smi_list_get(v, 96, 0x65), smi_list_get(v, 96, 0x68),
                       smi_list_get(v, 96, 0x93), smi_list_get(v, 96, 0x9d),
                       smi_list_get(a, 27, 0x29), smi_list_get(a, 27, 0x50), smi_list_get(a, 27, 0x68),
                       smi_list_get(a, 27, 0x93), smi_list_get(a, 27, 0x9d));
        }
        smi_dump(f, "ctrl", 0x123c8, 0x28);
        smi_dump(f, "vdstruct", 0xa3594, 0x10);
        smi_dump(f, "imgseqcoop", 0xef24, 0x10);
        my_fprintf(f, "  EDMAC port3 size %08x addr %08x\n", *(volatile uint32_t *) 0xD0420250, *(volatile uint32_t *) 0xD04202A0);
        my_fprintf(f, "  Mem1 0x12748: w/h %d/%d +18..+24 %d/%d %d/%d buf %x | SapPath 0x126e0: +10 set=%d +14 mode=%d | ROM size table e3 %d/%d\n",
                   *(volatile uint32_t *) (0x12748 + 0x10), *(volatile uint32_t *) (0x12748 + 0x14),
                   *(volatile uint32_t *) (0x12748 + 0x18), *(volatile uint32_t *) (0x12748 + 0x1c),
                   *(volatile uint32_t *) (0x12748 + 0x20), *(volatile uint32_t *) (0x12748 + 0x24),
                   *(volatile uint32_t *) (0x12748 + 0x58),
                   *(volatile uint32_t *) (0x126e0 + 0x10), *(volatile uint32_t *) (0x126e0 + 0x14),
                   *(volatile uint32_t *) 0xE0F7CDD8, *(volatile uint32_t *) 0xE0F7CDDC);
        FIO_CloseFile(f);
    }
    /* one DNG per run: ML/LOGS/SNAP1.DNG, SNAP2.DNG, ... (counter per boot) */
    static int snap_n = 0;
    if (ok)
    {
        int size = raw_info.frame_size;
        void *mem = fio_malloc(size + 64);
        if (mem)
        {
            void *cp = (void *)(((uintptr_t) mem + 63) & ~63);
            memcpy(cp, raw_info.buffer, size);
            raw_lv_release();
            char name[32];
            snprintf(name, sizeof(name), "ML/LOGS/SNAP%d.DNG", ++snap_n);
            NotifyBox(3000, "smode: saving %s...", name);
            struct raw_info ri = raw_info;
            ri.buffer = cp;
            int rc = save_dng(name, &ri);
            FILE *g = FIO_CreateFileOrAppend("ML/LOGS/SMODE.TXT");
            if (g) { my_fprintf(g, "  %s save_dng=%d\n", name, rc); FIO_CloseFile(g); }
            fio_free(mem);
            NotifyBox(5000, "smode: done. SMODE.TXT + %s", name);
            rawdump_running = 0;
            return;
        }
    }
    raw_lv_release();
    NotifyBox(5000, "smode: done. ML/LOGS/SMODE.TXT");
    rawdump_running = 0;
}

/* WRITE test (session 8), RAM ONLY: nothing is sent to the sensor by us.
 * W2F run: resending Canon's own unchanged movie list by PIO while LV streams HUNG the camera
 * (phase 0 control), so the OG33 hang was the PIO send itself, not the mode mix. Canon sends the
 * vsync list only at LV start / mode change / framerate change. So: change entry 2c in Canon's
 * RAM copy of the mode-3 vsync list (P + 3*0x180 + 0x2314) from 2b2 (16:9 band) to 256 (full
 * height, ROM three-way diff), then let Canon resend it (leave LV with Playback and come back).
 * A reboot restores Canon's list. "Sensor mode info (read)" logs 2c and saves SNAPn.DNG. */
static void rp_task()
{
    msleep(500);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/RPATCH.TXT");
    int done = 0;
    uint32_t before = 0xffffffff, after = 0xffffffff;
    if (P > 0x1000 && P < 0x60000000 && is_movie_mode() && smode == 6 && mode == 3)
    {
        volatile uint32_t *v = (volatile uint32_t *) (P + 3 * 0x180 + 0x2314);
        int n = 0;
        while (n < 96 && v[n] != 0xffffffff) n++;
        if (n > 40 && (v[n - 1] >> 22) == 0x3e2)
        {
            for (int i = 0; i < n; i++)
            {
                if ((v[i] >> 22) == 0x2c)
                {
                    before = v[i] & 0x3fffff;
                    if (before == 0x2b2) { v[i] = CMOS_W(0x2c, 0x256); done = 1; }
                    after = v[i] & 0x3fffff;
                    break;
                }
            }
        }
    }
    if (f)
    {
        my_fprintf(f, "=== RPATCH movie=%d SensorMode=%d mode=%d P=%x vsync(m3) 2c before=%x after=%x patched=%d fps=%d\n",
                   is_movie_mode(), smode, mode, P, before, after, done, lv_vsync_fps_x1000());
        FIO_CloseFile(f);
    }
    NotifyBox(5000, done ? "RAM patch 2c=256 done. Now Playback, then back to LV."
                         : "RAM patch NOT done (movie FHD 23.98 only). ML/LOGS/RPATCH.TXT");
    rawdump_running = 0;
}

/* AUTO version (session 8): no hands between the frames. RAM patch 2c=256 (as rp_task), then
 * ML's PauseLiveView / ResumeLiveView (PROP_LV_ACTION 1 / 0, Canon's own LV stop / start, which
 * resends the sensor lists), snapshot, then 2c=2b2 back + pause / resume again, snapshot.
 * Point at a plain evenly lit wall, out of focus: the lens vignetting peak row moves with the
 * sensor window, not with small camera movement. 3 DNG: RPA_0 (Canon), RPA_1 (2c=256), RPA_2. */
extern void PauseLiveView();
extern int ResumeLiveView();

static void rpa_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/RPA.TXT");
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static volatile uint32_t *rpa_find_2c(uint32_t P)
{
    volatile uint32_t *v = (volatile uint32_t *) (P + 3 * 0x180 + 0x2314);
    int n = 0;
    while (n < 96 && v[n] != 0xffffffff) n++;
    if (n < 40 || (v[n - 1] >> 22) != 0x3e2) return 0;
    for (int i = 0; i < n; i++) if ((v[i] >> 22) == 0x2c) return &v[i];
    return 0;
}

static void rpa_snap(const char *name, const char *tag)
{
    char buf[200];
    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    int h = raw_info.active_area.y2 - raw_info.active_area.y1;
    snprintf(buf, sizeof(buf), "%s: ok=%d raw %dx%d fps=%d top=%d mid=%d bottom=%d port3 %08x", tag, ok, raw_info.width, raw_info.height,
             lv_vsync_fps_x1000(), tgw_meter(100, 300), tgw_meter(h / 2 - 100, h / 2 + 100), tgw_meter(h - 210, h - 10),
             *(volatile uint32_t *) 0xD0420250);
    rpa_log(buf);
    if (ok)
    {
        int size = raw_info.frame_size;
        void *mem = fio_malloc(size + 64);
        if (mem)
        {
            void *cp = (void *)(((uintptr_t) mem + 63) & ~63);
            memcpy(cp, raw_info.buffer, size);
            raw_lv_release();
            struct raw_info ri = raw_info;
            ri.buffer = cp;
            int rc = save_dng((char *) name, &ri);
            snprintf(buf, sizeof(buf), "  %s save_dng=%d", name, rc);
            rpa_log(buf);
            fio_free(mem);
            return;
        }
    }
    raw_lv_release();
}

static void rpa_restart(const char *tag)
{
    char buf[160];
    PauseLiveView();
    msleep(1500);
    snprintf(buf, sizeof(buf), "%s: paused lv=%d lv_paused=%d fps=%d", tag, lv, lv_paused, lv_vsync_fps_x1000());
    rpa_log(buf);
    ResumeLiveView();
    msleep(2500);
    snprintf(buf, sizeof(buf), "%s: resumed lv=%d lv_paused=%d fps=%d", tag, lv, lv_paused, lv_vsync_fps_x1000());
    rpa_log(buf);
}

static void rpa_task()
{
    char buf[200];
    msleep(3000);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    volatile uint32_t *e = (P > 0x1000 && P < 0x60000000 && mode == 3) ? rpa_find_2c(P) : 0;
    int fps = lv_vsync_fps_x1000();
    snprintf(buf, sizeof(buf), "=== RPA movie=%d lv=%d dispsize=%d SensorMode=%d mode=%d fps=%d 2c=%x",
             is_movie_mode(), lv, lv_dispsize, smode, mode, fps, e ? (unsigned)(*e & 0x3fffff) : 0xffffffffu);
    rpa_log(buf);
    if (!e || !lv || !is_movie_mode() || lv_dispsize != 1 || smode != 6 || fps < 23000 || fps > 25000
        || ((*e & 0x3fffff) != 0x2b2 && (*e & 0x3fffff) != 0x256))
    {
        rpa_log("abort: need movie FHD 23.98 LV, mode-3 list ending with 3e2, 2c = 2b2 or 256");
        NotifyBox(5000, "rpa: abort (movie FHD 23.98). ML/LOGS/RPA.TXT");
        rawdump_running = 0;
        return;
    }

    /* 0: Canon's list (undo an earlier manual patch first) */
    *e = CMOS_W(0x2c, 0x2b2);
    rpa_restart("restart 0 (2c=2b2)");
    rpa_snap("ML/LOGS/RPA_0.DNG", "snap 0 Canon 2c=2b2");

    /* 1: 2c=256 in Canon's RAM list, Canon sends it at LV start */
    *e = CMOS_W(0x2c, 0x256);
    rpa_restart("restart 1 (2c=256)");
    rpa_snap("ML/LOGS/RPA_1.DNG", "snap 1 2c=256");

    /* 2: back to Canon's value */
    *e = CMOS_W(0x2c, 0x2b2);
    rpa_restart("restart 2 (2c=2b2)");
    rpa_snap("ML/LOGS/RPA_2.DNG", "snap 2 Canon again");

    rpa_log("done");
    NotifyBox(5000, "rpa: done. ML/LOGS/RPA.TXT + 3 DNG");
    rawdump_running = 0;
}

/* WRITE test (session 9): 3x3 FULL-HEIGHT readout in movie FHD 23.98, sensor AND capture together.
 * RPA run: RAM 2c=256 (full height) sent by Canon at LV start killed the LV raw capture (port 3
 * size 0) because the capture height stayed 1164. ROM chain: SapSetPath FUN_e080c3d8 copies
 * {w,h} from the ROM table 0xe0f7cda8 + mode*16 (entry 3 = 2096x1164, entry 0 = 2096x838) via
 * FUN_e080d4aa into Mem1 struct 0x12748 (+0x18/+0x1c); Mem1 init FUN_e080d36c sets EDMAC port 3
 * rows-1 from it. Camera sign: port 3 size 048b0e54 (1164) movie / 03450e54 (838) photo.
 * The table is ROM -> ML MMU data patch (apply_patches, first use on M50): entry 3 height
 * 1164 -> 1396 (full height 3x3: ~4190 sensor rows / 3), plus RAM list 2c=256, then Canon LV
 * restart (Pause/ResumeLiveView, proven in RPA run). Buffer safety: only if a "Sensor mode info"
 * run in movie 4K in THIS boot saw the same LV raw buffer address with >= 2000 rows. */
#define OG3_TAB_W 0xE0F7CDD8   /* table entry 3 width (2096) */
#define OG3_TAB_H 0xE0F7CDDC   /* table entry 3 height (1164) */
#define OG3_NEW_H 1396

static void og3_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/OG3.TXT");
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void og3_snap(const char *name, const char *tag)
{
    char buf[220];
    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    snprintf(buf, sizeof(buf), "%s: ok=%d raw %dx%d active %d,%d-%d,%d fps=%d port3 size %08x addr %08x Mem1 w/h %d/%d",
             tag, ok, raw_info.width, raw_info.height, raw_info.active_area.x1, raw_info.active_area.y1,
             raw_info.active_area.x2, raw_info.active_area.y2, lv_vsync_fps_x1000(),
             *(volatile uint32_t *) 0xD0420250, *(volatile uint32_t *) 0xD04202A0,
             *(volatile uint32_t *) (0x12748 + 0x10), *(volatile uint32_t *) (0x12748 + 0x14));
    og3_log(buf);
    if (ok)
    {
        int size = raw_info.frame_size;
        void *mem = fio_malloc(size + 64);
        if (mem)
        {
            void *cp = (void *)(((uintptr_t) mem + 63) & ~63);
            memcpy(cp, raw_info.buffer, size);
            raw_lv_release();
            struct raw_info ri = raw_info;
            ri.buffer = cp;
            int rc = save_dng((char *) name, &ri);
            snprintf(buf, sizeof(buf), "  %s save_dng=%d (%d bytes)", name, rc, size);
            og3_log(buf);
            fio_free(mem);
            return;
        }
    }
    raw_lv_release();
}

static void og3_task()
{
    char buf[220];
    msleep(3000);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    volatile uint32_t *e = (P > 0x1000 && P < 0x60000000 && mode == 3) ? rpa_find_2c(P) : 0;
    /* LV raw buffer: Mem1 struct +0x58 and port 3 address read 0 until LV raw is requested
     * (og3_2 run), so request it, read, release */
    raw_lv_request();
    msleep(500);
    uint32_t addr = *(volatile uint32_t *) (0x12748 + 0x58);
    if (!addr) addr = *(volatile uint32_t *) 0xD04202A0;
    raw_lv_release();
    uint32_t tw = *(volatile uint32_t *) OG3_TAB_W, th = *(volatile uint32_t *) OG3_TAB_H;
    int fps = lv_vsync_fps_x1000();
    snprintf(buf, sizeof(buf), "=== OG3 movie=%d lv=%d dispsize=%d SensorMode=%d mode=%d fps=%d 2c=%x table %d/%d LV raw addr %x | 4K seen addr %x rows %d",
             is_movie_mode(), lv, lv_dispsize, smode, mode, fps, e ? (unsigned)(*e & 0x3fffff) : 0xffffffffu,
             tw, th, addr, og4k_addr, og4k_rows);
    og3_log(buf);
    if (!e || !lv || !is_movie_mode() || lv_dispsize != 1 || smode != 6 || fps < 23000 || fps > 25000
        || (*e & 0x3fffff) != 0x2b2 || tw != 2096 || th != 1164)
    {
        og3_log("abort: need movie FHD 23.98 LV, mode-3 list with 2c=2b2, ROM table entry 3 = 2096x1164");
        NotifyBox(5000, "og3: abort (movie FHD 23.98). ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }
    /* buffer proof: og3_1 run (raw_tests/og3_1/SMODE.TXT): movie 4K (SensorMode 12) uses the SAME LV
     * raw buffer 0x6dce7518 with port 3 size 08991ddc = 2202 rows x 7644 bytes (16.8 MB), so this
     * buffer holds far more than 1396 x 3668 bytes. Accept it, or a 4K run in this boot. */
    int buf_ok = (addr == 0x6dce7518) || (og4k_addr != 0 && og4k_addr == addr && og4k_rows >= 2000);
    if (!buf_ok)
    {
        og3_log("abort: LV raw buffer is not the one proven to hold a 4K frame (0x6dce7518)");
        NotifyBox(6000, "og3: abort (unknown LV raw buffer). ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }

    og3_snap("ML/LOGS/OG3_0.DNG", "snap 0 Canon");

    struct patch p = {
        .addr = (uint8_t *) OG3_TAB_H,
        .old_value = 1164,
        .new_value = OG3_NEW_H,
        .size = 4,
        .description = "rawdump OG3: movie FHD capture height",
        .is_instruction = 0,
    };
    int err = apply_patches(&p, 1);
    snprintf(buf, sizeof(buf), "MMU patch table height 1164 -> %d: err=%d, reads now %d", OG3_NEW_H, err,
             *(volatile uint32_t *) OG3_TAB_H);
    og3_log(buf);
    if (err != 0 || *(volatile uint32_t *) OG3_TAB_H != OG3_NEW_H)
    {
        if (err == 0) unpatch_memory(OG3_TAB_H);
        og3_log("abort: ROM patch failed, nothing else changed");
        NotifyBox(5000, "og3: ROM patch failed. ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }

    *e = CMOS_W(0x2c, 0x256);
    og3_log("RAM 2c=256 set, restarting LV");
    rpa_restart("restart 1 (height 1396 + 2c=256)");
    og3_snap("ML/LOGS/OG3_1.DNG", "snap 1 3x3 full height");

    /* back to Canon */
    *e = CMOS_W(0x2c, 0x2b2);
    err = unpatch_memory(OG3_TAB_H);
    snprintf(buf, sizeof(buf), "unpatch err=%d, table reads %d, RAM 2c=2b2", err, *(volatile uint32_t *) OG3_TAB_H);
    og3_log(buf);
    rpa_restart("restart 2 (Canon)");
    og3_snap("ML/LOGS/OG3_2.DNG", "snap 2 Canon again");

    og3_log("done");
    NotifyBox(5000, "og3: done. ML/LOGS/OG3.TXT + 3 DNG");
    rawdump_running = 0;
}

/* ARM / DISARM (session 9): OG3 run 3 showed the MMU ROM patch works on M50 (table read 1396,
 * unpatch back to 1164, no hang) but Pause/ResumeLiveView does not redo SapSetPath (it runs once
 * per path setup: flag SapPath +0x10) nor the sensor list, so nothing changed. ARM leaves both
 * changes active (MMU table height 1396 + RAM list 2c=256); Shosho then changes the Canon movie
 * size (FHD -> 4K -> FHD 23.98) so Canon rebuilds the whole path; "Sensor mode info" logs the
 * result (+ SNAPn.DNG). DISARM undoes both (a reboot also undoes them). */
static int og3_armed = 0;
/* session 10: isolate the FHD -> 4K hang of OG3 ARM run 1: 1 = height only, 2 = 2c only, 3 = both.
 * Bit 4 (what=7, session 10 run 2): arm BOTH while in movie 4K (SensorMode 12), so the next path
 * setup (4K -> FHD 23.98) gets height 1396 and 2c=256 together. iso run 1 (height only): FHD -> 4K
 * fine, 4K -> FHD hung (capture 1396 rows, sensor band still 1164 rows). */
static int og3_arm_what = 3;
/* capture height the ARM writes into ROM table entry 3. iso run 3 (2c only, 2026-11-23): 2c=256
 * applied by Canon at the 4K -> FHD setup did NOT hang (raw 2096x1164, image normal), while every
 * FHD setup with 1396 rows hung. 1396 may simply be more rows than the 3x3 sensor ever sends
 * (full 3:2 = 34 OB + 4020/3 = ~1374), so the in-4K test now asks for 1300 rows. */
static int og3_new_h = OG3_NEW_H;

/* session 10 run 5: the readout WINDOW and ROW COUNT live in the mode-3 ACCUM list (P + 3*0x6c + 0x2f14),
 * which Canon sends after the vsync list at every path setup and AE change.
 * reg 0x68 = (start << 11) | end in 2-sensor-row units: movie band 0x5c756 = 184..1878 (3390 rows / 3 =
 * 1130 active), photo full 0xe7f5 = 28..2037 (4020 rows). reg 0x93 = (active rows + 14) / 2 in skip
 * modes: movie 0x23c = (1130+14)/2, photo 3x5 0x199 = (804+14)/2; 1:1 modes rows+1 (4K 0x89b, x5 0x4b3).
 * Canon's own 3x3 config C2 (ROM e19e1acc) has 68=e7f5 + 93=2a5 = (1340+14)/2 = full 3:2 height in 3x3.
 * iso run 4 A/B: 2c=256 alone changed nothing (same framing) -> the window is NOT 2c. */
#define OG3_A68_OLD 0x5c756
#define OG3_A68_NEW 0xe7f5
#define OG3_A93_OLD 0x23c
#define OG3_A93_NEW 0x2a5

static volatile uint32_t *og3_accum_find(uint32_t P, uint32_t reg)
{
    volatile uint32_t *a = (volatile uint32_t *) (P + 3 * 0x6c + 0x2f14);
    for (int i = 0; i < 27; i++)
    {
        if (a[i] == 0xffffffff) return 0;
        if ((a[i] >> 22) == reg) return &a[i];
    }
    return 0;
}

static void og3arm_task()
{
    char buf[200];
    msleep(500);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    int in4k = (og3_arm_what & 4) != 0;
    /* rpa_find_2c always reads the mode-3 (movie FHD) list; in 4K it is the list of the NEXT FHD setup */
    volatile uint32_t *e = (P > 0x1000 && P < 0x60000000 && (mode == 3 || (in4k && mode == 9))) ? rpa_find_2c(P) : 0;
    raw_lv_request();
    msleep(500);
    uint32_t addr = *(volatile uint32_t *) (0x12748 + 0x58);
    if (!addr) addr = *(volatile uint32_t *) 0xD04202A0;
    raw_lv_release();
    volatile uint32_t *a68 = e ? og3_accum_find(P, 0x68) : 0;
    volatile uint32_t *a93 = e ? og3_accum_find(P, 0x93) : 0;
    snprintf(buf, sizeof(buf), "=== OG3 ARM what=%d (1 height, 2 2c, 4 in 4K, 8 rows) movie=%d SensorMode=%d mode=%d 2c=%x accum 68=%x 93=%x table %d/%d buf %x armed=%d",
             og3_arm_what, is_movie_mode(), smode, mode, e ? (unsigned)(*e & 0x3fffff) : 0xffffffffu,
             a68 ? (unsigned)(*a68 & 0x3fffff) : 0xffffffffu, a93 ? (unsigned)(*a93 & 0x3fffff) : 0xffffffffu,
             *(volatile uint32_t *) OG3_TAB_W, *(volatile uint32_t *) OG3_TAB_H, addr, og3_armed);
    og3_log(buf);
    /* bit 16 (run 12): Canon's per-SensorMode RAM lists for movie FHD (SM6), applied at the next path
     * setup. Q region = ROM mirror from 0xe19e4b9c (checked with TGINFO). TG triples (addr, value, 0):
     * Q+0x123c d04061a4 = 247, Q+0x1278 d04061b8 = 247 (1166 rows = 2 x 0x247 measured in run 10).
     * Front-end pairs: Q+0x544c d041224c = 48d (rows + 1; live read 48d in runs 10/11).
     * Live b8 writes are ignored (run 11: b8 read back 0), so the lists are the only way in. */
    uint32_t Q = *(volatile uint32_t *) (0xeedc + 0x1c);
    if (og3_arm_what & 16)
    {
        int qok = (Q > 0x1000 && Q < 0x60000000);
        volatile uint32_t *q = (volatile uint32_t *) Q;
        snprintf(buf, sizeof(buf), "lists: Q=%x TG %x=%x %x=%x fe %x=%x", Q,
                 qok ? q[0x123c / 4] : 0, qok ? q[0x1240 / 4] : 0, qok ? q[0x1278 / 4] : 0, qok ? q[0x127c / 4] : 0,
                 qok ? q[0x544c / 4] : 0, qok ? q[0x5450 / 4] : 0);
        og3_log(buf);
        if (!qok || q[0x123c / 4] != 0xD04061A4 || q[0x1240 / 4] != 0x247 || q[0x1278 / 4] != 0xD04061B8
            || q[0x127c / 4] != 0x247 || q[0x544c / 4] != 0xD041224C || q[0x5450 / 4] != 0x48d)
        {
            og3_log("arm abort: Canon FHD TG / front-end RAM lists are not where expected, nothing changed");
            NotifyBox(5000, "OG3 arm: abort (lists). ML/LOGS/OG3.TXT");
            rawdump_running = 0;
            return;
        }
    }
    if ((og3_arm_what & 8) && (!a68 || !a93 || (*a68 & 0x3fffff) != OG3_A68_OLD || (*a93 & 0x3fffff) != OG3_A93_OLD))
    {
        og3_log("arm abort: FHD accum list is not Canon's (need 68=5c756 93=23c)");
        NotifyBox(5000, "OG3 arm: abort (accum list). ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }
    if (og3_armed || !e || !is_movie_mode() || smode != (in4k ? 12 : 6) || (*e & 0x3fffff) != 0x2b2
        || *(volatile uint32_t *) OG3_TAB_W != 2096 || *(volatile uint32_t *) OG3_TAB_H != 1164 || addr != 0x6dce7518)
    {
        og3_log(in4k ? "arm abort: need movie 4K 23.98 (SensorMode 12), FHD list 2c=2b2, table 2096/1164, buffer 6dce7518, not armed yet"
                     : "arm abort: need movie FHD 23.98, 2c=2b2, table 2096/1164, buffer 6dce7518, not armed yet");
        NotifyBox(5000, in4k ? "OG3 arm: abort (movie 4K 23.98). ML/LOGS/OG3.TXT"
                             : "OG3 arm: abort (movie FHD 23.98). ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }
    /* bit 32 (session 11): MMU-patch Canon's ROM TG list for SensorMode 6 (movie FHD). b8 matches the
     * readout lines in 4 modes (SM6 0x247 = 1166/2, SM7 0x1a4 = 840/2, SM12 0x89a = 2202, SM13 0x4b4 = 1204)
     * and run 14 measured exactly 1166 lines, but the RAM copy patch (Q+0x1240/0x127c) changed nothing,
     * so the TG code likely reads the ROM list. Triples (addr, value, 0): a4 at 0xe19e5dd8, b8 at 0xe19e5e14. */
    if (og3_arm_what & 32)
    {
        if (*(volatile uint32_t *) 0xe19e5dd8 != 0xD04061A4 || *(volatile uint32_t *) 0xe19e5ddc != 0x247
            || *(volatile uint32_t *) 0xe19e5e14 != 0xD04061B8 || *(volatile uint32_t *) 0xe19e5e18 != 0x247)
        {
            og3_log("arm abort: ROM TG list for SM6 is not where expected, nothing changed");
            NotifyBox(5000, "OG3 arm: abort (ROM TG). ML/LOGS/OG3.TXT");
            rawdump_running = 0;
            return;
        }
        struct patch tp[2] = {
            { .addr = (uint8_t *) 0xe19e5ddc, .old_value = 0x247, .new_value = 0x2ae, .size = 4,
              .description = "rawdump OG3: ROM TG a4 FHD", .is_instruction = 0 },
            { .addr = (uint8_t *) 0xe19e5e18, .old_value = 0x247, .new_value = 0x2ae, .size = 4,
              .description = "rawdump OG3: ROM TG b8 FHD", .is_instruction = 0 },
        };
        int terr = apply_patches(tp, 2);
        snprintf(buf, sizeof(buf), "ROM TG patch err=%d, a4 reads %x, b8 reads %x", terr,
                 *(volatile uint32_t *) 0xe19e5ddc, *(volatile uint32_t *) 0xe19e5e18);
        og3_log(buf);
        if (terr != 0 || *(volatile uint32_t *) 0xe19e5ddc != 0x2ae || *(volatile uint32_t *) 0xe19e5e18 != 0x2ae)
        {
            if (*(volatile uint32_t *) 0xe19e5ddc == 0x2ae) unpatch_memory(0xe19e5ddc);
            if (*(volatile uint32_t *) 0xe19e5e18 == 0x2ae) unpatch_memory(0xe19e5e18);
            og3_log("arm abort: ROM TG patch failed, undone, nothing else changed");
            NotifyBox(5000, "OG3 arm: ROM TG patch failed");
            rawdump_running = 0;
            return;
        }
    }
    if (og3_arm_what & 1)
    {
        struct patch p = {
            .addr = (uint8_t *) OG3_TAB_H, .old_value = 1164, .new_value = og3_new_h,
            .size = 4, .description = "rawdump OG3: movie FHD capture height", .is_instruction = 0,
        };
        int err = apply_patches(&p, 1);
        if (err != 0 || *(volatile uint32_t *) OG3_TAB_H != (uint32_t) og3_new_h)
        {
            if (err == 0) unpatch_memory(OG3_TAB_H);
            snprintf(buf, sizeof(buf), "arm: ROM patch failed err=%d", err);
            og3_log(buf);
            NotifyBox(5000, "OG3 arm: ROM patch failed");
            rawdump_running = 0;
            return;
        }
    }
    if (og3_arm_what & 2) *e = CMOS_W(0x2c, 0x256);
    if (og3_arm_what & 8)
    {
        *a68 = CMOS_W(0x68, OG3_A68_NEW);
        *a93 = CMOS_W(0x93, OG3_A93_NEW);
    }
    if (og3_arm_what & 16)
    {
        volatile uint32_t *q = (volatile uint32_t *) Q;
        q[0x1240 / 4] = 0x2ae;   /* TG a4 base: 686 pairs = 1372 rows */
        q[0x127c / 4] = 0x2ae;   /* TG b8 */
        q[0x5450 / 4] = 0x55b;   /* front-end: 1370 rows + 1 */
        snprintf(buf, sizeof(buf), "lists patched: TG a4/b8 %x/%x, fe %x", q[0x1240 / 4], q[0x127c / 4], q[0x5450 / 4]);
        og3_log(buf);
    }
    og3_armed = 1;
    snprintf(buf, sizeof(buf), "ARMED what=%d: table height %d, RAM 2c=%x, accum 68=%x 93=%x. %s",
             og3_arm_what, *(volatile uint32_t *) OG3_TAB_H, (unsigned)(*e & 0x3fffff),
             (unsigned)(*a68 & 0x3fffff), (unsigned)(*a93 & 0x3fffff),
             in4k ? "Now Canon movie size FHD 23.98 (do not go back to 4K)." : "Now Canon movie size 4K, then FHD 23.98.");
    og3_log(buf);
    if (in4k && (og3_arm_what & 32))
        NotifyBox(6000, "ARMED in 4K: rows + lists + ROM. Now movie FHD 23.98");
    else if (in4k && (og3_arm_what & 16))
        NotifyBox(6000, "ARMED in 4K: rows + lists. Now movie FHD 23.98");
    else if (in4k && !(og3_arm_what & 1))
        NotifyBox(6000, "ARMED in 4K: rows only. Now movie FHD 23.98");
    else if (in4k)
        NotifyBox(6000, "ARMED in 4K: %s + height %d. Now movie FHD 23.98", (og3_arm_what & 8) ? "rows" : "2c", og3_new_h);
    else
        NotifyBox(6000, og3_arm_what == 8 ? "ARMED: rows only. Now movie 4K, then FHD 23.98"
                      : og3_arm_what == 1 ? "ARMED: height only. Now movie 4K, then FHD 23.98"
                      : og3_arm_what == 2 ? "ARMED: 2c only. Now movie 4K, then FHD 23.98"
                      : "ARMED: both. Now movie 4K, then FHD 23.98");
    rawdump_running = 0;
}

/* session 10 run 8: LIVE capture height (no Canon mode change).
 * Run 7 (raw_tests/s10_run7) proved the sensor side: ARM rows only (accum 68=e7f5 93=2a5) + an AE
 * change -> SNAP2 framing moved down 102 rows (predicted 104 = (184-28)*2/3), same scale, LV fine,
 * clean power off. Mode changes after a sensor change hung (runs 1, 6), so stay in FHD.
 * FUN_e080d36c programs EDMAC port 3 from Mem1 0x12748 ([6]/[7] = +0x18/+0x1c -> [4]/[5], rows-1,
 * pitch w*14/8); it runs from FUN_e075b37c, called by Conductor_EvfDev_Controller_Vram
 * (FUN_e00dd2a8), i.e. per LV frame. So set Mem1 +0x1c = 1370 live and check, with row signatures
 * of the raw buffer, whether rows past 1164 start to change frame to frame (= real sensor rows). */
#define OG3_LIVE_H 1370
/* run 9 (raw_tests/s10_run9): Mem1 1370 live worked (no hang, port3 05590e54, OG3L.DNG 2096x1370) but
 * rows 1164..1197 were the NEXT frame's 34 black OB rows and the next frame started at row 1164
 * (corr 1.000): the input stream still ends each frame after 1164 rows. ROM per-SensorMode front-end
 * lists (stride 0x2f0, ROM e19e8e00 + SM*0x2f0, RAM Q + 0x4264 + SM*0x2f0, sent by FLK_MdChg3_B):
 * reg 0xD041224C = rows + 1: SM6 FHD 0x48d (1164), SM7 photo 0x347 (838), SM12 4K 0x89b (2202). */
#define OG3_FE_VSIZE 0xD041224C
static int og3_live_fe = 0;   /* 0 Mem1 only, 1 + front-end, 2 + front-end + TG b8 probe */
/* run 10 (raw_tests/s10_run10): with fe 55b the input frame grew from 1164 to only 1166 rows =
 * 2 x 0x247, the value of Canon's FHD TG list (d04061a4 base and d04061b8, ROM 0xe19e5dd8/0xe19e5e18).
 * Live b8 reads 0 (later lists write b8=0), so test it live: b8 = 0x2ae (686 pairs = 1372 rows),
 * then the TG confirm write fps24_write already uses (0xD0406190 = 1). Restored right after. */
#define OG3_TG_B8_REG 0xD04061B8
#define OG3_TG_B8 0x2ae
static uint32_t og3_rowavg(uint32_t buf, int row)
{
    volatile uint8_t *q = (volatile uint8_t *) (buf + row * 3668 + 400);
    uint32_t sum = 0;
    for (int i = 0; i < 2048; i++) sum += q[i];
    return sum / 2048;
}
static uint32_t og3_rowsig(uint32_t buf, int row)
{
    volatile uint32_t *q = (volatile uint32_t *) (buf + row * 3668 + 400);
    uint32_t sig = 0;
    for (int i = 0; i < 512; i++) sig = sig * 31 + q[i];
    return sig;
}

/* save the raw LV buffer as it is (14-bit packed, pitch 3668), even when raw_update_params fails */
static void og3_rawsave(const char *name, uint32_t src, int rows)
{
    char b[160];
    int size = rows * 3668;
    void *mem = fio_malloc(size + 64);
    if (!mem) { og3_log("rawsave: fio_malloc failed"); return; }
    void *cp = (void *) (((uintptr_t) mem + 63) & ~63);
    memcpy(cp, (void *) src, size);
    FILE *f = FIO_CreateFile(name);
    int ok = 0;
    if (f) { ok = (FIO_WriteFile(f, cp, size) == size); FIO_CloseFile(f); }
    fio_free(mem);
    snprintf(b, sizeof(b), "rawsave %s: %d rows x 3668 = %d bytes, ok=%d", name, rows, size, ok);
    og3_log(b);
}

/* in-camera frame length: first row >= 1100 that starts 8 rows with the OB byte average (+-1) */
static int og3_frame_len(uint32_t buf, int maxrows)
{
    int ob = ((int) og3_rowavg(buf, 8) + (int) og3_rowavg(buf, 16) + (int) og3_rowavg(buf, 24)) / 3;
    for (int r = 1100; r + 8 < maxrows; r++)
    {
        int ok = 1;
        for (int k = 0; k < 8 && ok; k++)
        {
            int v = (int) og3_rowavg(buf, r + k);
            if (v < ob - 1 || v > ob + 1) ok = 0;
        }
        if (ok) return r;
    }
    return -1;
}

static void og3live_task()
{
    char buf[320];
    msleep(1000);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    int pok = (P > 0x1000 && P < 0x60000000);
    volatile uint32_t *a68 = (pok && mode == 3) ? og3_accum_find(P, 0x68) : 0;
    volatile uint32_t *a93 = (pok && mode == 3) ? og3_accum_find(P, 0x93) : 0;
    volatile uint32_t *M = (volatile uint32_t *) 0x12748;
    /* run 13: after a 4K -> FHD switch, port3 reads 0 and Mem1 +10/+14 still hold 4K values until LV raw is
     * requested (FUN_e080d36c only runs with raw on), so turn raw on before reading them */
    raw_lv_request();
    msleep(600);
    uint32_t bufa = M[0x58 / 4];
    uint32_t sz = *(volatile uint32_t *) 0xD0420250;
    snprintf(buf, sizeof(buf), "=== OG3 LIVE movie=%d SensorMode=%d mode=%d armed=%d what=%d accum 68=%x 93=%x Mem1[0]=%x [10]=%x +10/+14 %d/%d +18/+1c %d/%d +20/+24 %d/%d buf %x port3 %08x",
             is_movie_mode(), smode, mode, og3_armed, og3_arm_what, a68 ? (unsigned)(*a68 & 0x3fffff) : 0xffffffffu,
             a93 ? (unsigned)(*a93 & 0x3fffff) : 0xffffffffu, M[0], M[10], M[4], M[5], M[6], M[7], M[8], M[9], bufa, sz);
    og3_log(buf);
    if (!is_movie_mode() || smode != 6 || mode != 3 || !og3_armed || !(og3_arm_what & 8) || !a68 || !a93
        || (*a68 & 0x3fffff) != OG3_A68_NEW || (*a93 & 0x3fffff) != OG3_A93_NEW
        || M[4] != 2096 || M[5] != 1164 || M[6] != 2096 || M[7] != 1164 || bufa != 0x6dce7518 || sz != 0x048b0e54)
    {
        raw_lv_release();
        og3_log("live abort: need movie FHD 23.98 after 'OG3 ARM rows only' (Mem1 2096/1164, port3 048b0e54, buf 6dce7518)");
        NotifyBox(6000, "OG3 live: abort. First 'OG3 ARM rows only' in FHD 23.98");
        rawdump_running = 0;
        return;
    }
    /* run 8 (raw_tests/s10_run8): without raw_lv_request Canon does not capture LV raw at all, so the
     * buffer was static even before the change ("before: row600 changes=0") -> false "stalled". */
    uint32_t r1 = og3_rowsig(bufa, 600), x1 = og3_rowsig(bufa, 1250);
    msleep(200);
    uint32_t r2 = og3_rowsig(bufa, 600), x2 = og3_rowsig(bufa, 1250);
    snprintf(buf, sizeof(buf), "before (raw on): row600 changes=%d, row1250 changes=%d, port3 %08x, Mem1 +10/+14 %d/%d",
             r1 != r2, x1 != x2, *(volatile uint32_t *) 0xD0420250, M[4], M[5]);
    og3_log(buf);
    if (r1 == r2 && !(og3_arm_what & 16))
    {
        raw_lv_release();
        og3_log("live abort: LV raw is not changing even with raw on, nothing changed");
        NotifyBox(6000, "OG3 live: raw not running, nothing changed. ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }

    uint32_t fe0 = *(volatile uint32_t *) OG3_FE_VSIZE;
    snprintf(buf, sizeof(buf), "front-end d041224c reads %x (Canon FHD list 48d = 1164 rows + 1), fe mode=%d, byte avg row600 %d row1180 %d",
             fe0, og3_live_fe, og3_rowavg(bufa, 600), og3_rowavg(bufa, 1180));
    og3_log(buf);
    if (og3_live_fe)
    {
        if (fe0 != 0x48d)
        {
            raw_lv_release();
            og3_log("live abort: front-end register does not read 48d, nothing changed");
            NotifyBox(6000, "OG3 live: abort (front-end reg). ML/LOGS/OG3.TXT");
            rawdump_running = 0;
            return;
        }
        *(volatile uint32_t *) OG3_FE_VSIZE = OG3_LIVE_H + 1;
    }
    M[7] = OG3_LIVE_H;
    msleep(400);
    r1 = og3_rowsig(bufa, 600); x1 = og3_rowsig(bufa, 1250);
    msleep(200);
    r2 = og3_rowsig(bufa, 600); x2 = og3_rowsig(bufa, 1250);
    snprintf(buf, sizeof(buf), "live %d: row600 changes=%d, row1250 changes=%d, port3 %08x, Mem1 +10/+14 %d/%d, fps=%d, fe %x, byte avg row600 %d row1180 %d",
             OG3_LIVE_H, r1 != r2, x1 != x2, *(volatile uint32_t *) 0xD0420250, M[4], M[5], lv_vsync_fps_x1000(),
             *(volatile uint32_t *) OG3_FE_VSIZE, og3_rowavg(bufa, 600), og3_rowavg(bufa, 1180));
    og3_log(buf);
    snprintf(buf, sizeof(buf), "row byte avg: 16=%d 600=%d 1100=%d 1150=%d 1160=%d 1166=%d 1175=%d 1190=%d 1210=%d 1250=%d 1300=%d 1350=%d 1369=%d",
             og3_rowavg(bufa, 16), og3_rowavg(bufa, 600), og3_rowavg(bufa, 1100), og3_rowavg(bufa, 1150), og3_rowavg(bufa, 1160),
             og3_rowavg(bufa, 1166), og3_rowavg(bufa, 1175), og3_rowavg(bufa, 1190), og3_rowavg(bufa, 1210), og3_rowavg(bufa, 1250),
             og3_rowavg(bufa, 1300), og3_rowavg(bufa, 1350), og3_rowavg(bufa, 1369));
    og3_log(buf);
    if (r1 == r2)
    {
        if (og3_live_fe) *(volatile uint32_t *) OG3_FE_VSIZE = fe0;
        M[7] = 1164;
        raw_lv_release();
        og3_log("live: LV raw stopped changing -> front-end and Mem1 back to Canon values");
        NotifyBox(6000, "OG3 live: LV stalled, put back 1164. ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }
    if (og3_live_fe == 2)
    {
        int L1 = og3_frame_len(bufa, OG3_LIVE_H);
        uint32_t b80 = *(volatile uint32_t *) OG3_TG_B8_REG;
        snprintf(buf, sizeof(buf), "probe: frame length %d with fe + Mem1 (run 10: 1166), b8 reads %x, a4 %x, OB byte avg %d",
                 L1, b80, *(volatile uint32_t *) 0xD04061A4, og3_rowavg(bufa, 16));
        og3_log(buf);
        *(volatile uint32_t *) OG3_TG_B8_REG = OG3_TG_B8;
        *(volatile uint32_t *) M50_FPS_CONFIRM = 1;
        msleep(400);
        r1 = og3_rowsig(bufa, 600);
        msleep(200);
        r2 = og3_rowsig(bufa, 600);
        int L2 = (r1 != r2) ? og3_frame_len(bufa, OG3_LIVE_H) : -2;
        snprintf(buf, sizeof(buf), "probe: after b8=%x + confirm: row600 changes=%d, frame length %d, b8 reads %x, a4 %x, fps %d",
                 OG3_TG_B8, r1 != r2, L2, *(volatile uint32_t *) OG3_TG_B8_REG, *(volatile uint32_t *) 0xD04061A4,
                 lv_vsync_fps_x1000());
        og3_log(buf);
        if (r1 == r2)
        {
            *(volatile uint32_t *) OG3_TG_B8_REG = b80;
            *(volatile uint32_t *) M50_FPS_CONFIRM = 1;
            *(volatile uint32_t *) OG3_FE_VSIZE = fe0;
            M[7] = 1164;
            raw_lv_release();
            og3_log("probe: LV stalled after b8 -> b8, fe, Mem1 back to Canon values");
            NotifyBox(6000, "OG3 probe: LV stalled, put back. ML/LOGS/OG3.TXT");
            rawdump_running = 0;
            return;
        }
        og3_snap("ML/LOGS/OG3T.DNG", "snap live + fe + TG b8");
        *(volatile uint32_t *) OG3_TG_B8_REG = b80;
        *(volatile uint32_t *) M50_FPS_CONFIRM = 1;
        msleep(300);
        r1 = og3_rowsig(bufa, 600);
        msleep(200);
        r2 = og3_rowsig(bufa, 600);
        snprintf(buf, sizeof(buf), "probe: b8 back to %x: row600 changes=%d, frame length %d", *(volatile uint32_t *) OG3_TG_B8_REG,
                 r1 != r2, og3_frame_len(bufa, OG3_LIVE_H));
        og3_log(buf);
    }
    else
    {
        og3_snap(og3_live_fe ? "ML/LOGS/OG3F.DNG" : "ML/LOGS/OG3L.DNG", og3_live_fe ? "snap live + front-end" : "snap live");
        og3_rawsave("ML/LOGS/OG3L.RAW", bufa, OG3_LIVE_H);
    }
    if (og3_live_fe) *(volatile uint32_t *) OG3_FE_VSIZE = fe0;
    M[7] = 1164;
    msleep(400);
    r1 = og3_rowsig(bufa, 600);
    msleep(200);
    r2 = og3_rowsig(bufa, 600);
    snprintf(buf, sizeof(buf), "back to 1164: row600 changes=%d, port3 %08x, Mem1 +14 %d, fe %x", r1 != r2,
             *(volatile uint32_t *) 0xD0420250, M[5], *(volatile uint32_t *) OG3_FE_VSIZE);
    og3_log(buf);
    raw_lv_release();
    NotifyBox(6000, og3_live_fe == 2 ? "OG3 probe: done. ML/LOGS/OG3.TXT + OG3T.DNG"
                  : og3_live_fe ? "OG3 live: done. ML/LOGS/OG3.TXT + OG3F.DNG" : "OG3 live: done. ML/LOGS/OG3.TXT + OG3L.DNG");
    rawdump_running = 0;
}

/* session 11: DUAL ISO test (low light goal). M50 sensor gain table in ROM, found from FUN_e0824612
 * (it fills accum list words [2]/[3] from DAT_e0824934 = 0xe0b3a93c + gain step * 8): 5 sections x 7 full
 * stops of {CMOS 0x2d = ISO nibbles ABCD + flags, CMOS 0x33}. 0x2d values 0b400000 / 0b411110 / ... /
 * 0b466660 = ISO 100..6400, exactly the 200D format that ML dual_iso already supports
 * (patch_cmos_iso_values_200d, "0xRRR ABCD 0", reg b4 = 0x2d << 22). Like the 200D code:
 * replace nibbles C,D (mask 0xff0) with the alternate ISO (1600 = 0x440), only entries with 4 equal
 * nibbles (Canon's own mixed entries untouched), one MMU patch of the whole 0x118 bytes. */
#define M50_GAIN_TAB 0xe0b3a93c
#define M50_GAIN_N 35
static int diso_on = 0;

static void diso_task()
{
    char buf[200];
    msleep(300);
    if (diso_on) { NotifyBox(4000, "DISO already ON"); rawdump_running = 0; return; }
    uint32_t size = M50_GAIN_N * 8;
    uint8_t *nv = malloc(size * 2);
    if (!nv) { NotifyBox(4000, "DISO: malloc failed"); rawdump_running = 0; return; }
    uint8_t *ov = nv + size;
    memcpy(nv, (void *) M50_GAIN_TAB, size);
    memcpy(ov, (void *) M50_GAIN_TAB, size);
    int n = 0, bad = 0;
    for (int i = 0; i < M50_GAIN_N; i++)
    {
        uint32_t v = ((uint32_t *) ov)[i * 2], v33 = ((uint32_t *) ov)[i * 2 + 1];
        if ((v >> 22) != 0x2d || (v33 >> 22) != 0x33) { bad++; continue; }
        uint32_t nib = (v >> 4) & 0xffff;
        uint32_t a = nib >> 12;
        if (((nib >> 8) & 15) != a || ((nib >> 4) & 15) != a || (nib & 15) != a || a > 6) continue;
        ((uint32_t *) nv)[i * 2] = (v & ~0xff0u) | 0x440;
        n++;
    }
    snprintf(buf, sizeof(buf), "=== DISO table %x: %d entries -> lower ISO nibbles 44 (ISO 1600), %d bad, first %08x -> %08x",
             M50_GAIN_TAB, n, bad, ((uint32_t *) ov)[0], ((uint32_t *) nv)[0]);
    og3_log(buf);
    if (bad || n < 20)
    {
        free(nv);
        og3_log("DISO abort: gain table not as expected, nothing changed");
        NotifyBox(5000, "DISO: abort (table). ML/LOGS/OG3.TXT");
        rawdump_running = 0;
        return;
    }
    struct patch p = { .addr = (uint8_t *) M50_GAIN_TAB, .old_values = ov, .new_values = nv, .size = size,
                       .description = "rawdump DISO: ISO 1600 on half the lines", .is_instruction = 0 };
    int err = apply_patches(&p, 1);
    free(nv);
    snprintf(buf, sizeof(buf), "DISO patch err=%d, first word now %08x", err, *(volatile uint32_t *) M50_GAIN_TAB);
    og3_log(buf);
    diso_on = (err == 0);
    NotifyBox(6000, err ? "DISO: patch failed. ML/LOGS/OG3.TXT" : "DISO ON. Change ISO 200 then back to 100, then Sensor mode info");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(diso_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("diso_task", 0x1e, 0x2000, diso_task, 0);
}

static MENU_SELECT_FUNC(disooff_start)
{
    if (!diso_on) { NotifyBox(3000, "DISO is OFF"); return; }
    int err = unpatch_memory(M50_GAIN_TAB);
    diso_on = (err != 0);
    char buf[80];
    snprintf(buf, sizeof(buf), "DISO OFF: unpatch err=%d, first word %08x", err, *(volatile uint32_t *) M50_GAIN_TAB);
    og3_log(buf);
    NotifyBox(5000, "DISO OFF. Change ISO once to apply");
}

static MENU_SELECT_FUNC(og3livetg_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_live_fe = 2;
    task_create("og3live_task", 0x1e, 0x2000, og3live_task, 0);
}

static MENU_SELECT_FUNC(og3livefe_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_live_fe = 1;
    task_create("og3live_task", 0x1e, 0x2000, og3live_task, 0);
}

static MENU_SELECT_FUNC(og3live_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_live_fe = 0;
    task_create("og3live_task", 0x1e, 0x2000, og3live_task, 0);
}

static void og3disarm_task()
{
    char buf[160];
    msleep(500);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    int n2c = 0;
    if (P > 0x1000 && P < 0x60000000)
    {
        volatile uint32_t *e = rpa_find_2c(P);
        if (e && (*e & 0x3fffff) == 0x256) { *e = CMOS_W(0x2c, 0x2b2); n2c = 1; }
        volatile uint32_t *a68 = og3_accum_find(P, 0x68), *a93 = og3_accum_find(P, 0x93);
        if (a68 && (*a68 & 0x3fffff) == OG3_A68_NEW) { *a68 = CMOS_W(0x68, OG3_A68_OLD); n2c |= 2; }
        if (a93 && (*a93 & 0x3fffff) == OG3_A93_NEW) { *a93 = CMOS_W(0x93, OG3_A93_OLD); n2c |= 4; }
        uint32_t Q = *(volatile uint32_t *) (0xeedc + 0x1c);
        if (Q > 0x1000 && Q < 0x60000000)
        {
            volatile uint32_t *q = (volatile uint32_t *) Q;
            if (q[0x123c / 4] == 0xD04061A4 && q[0x1240 / 4] == 0x2ae) { q[0x1240 / 4] = 0x247; n2c |= 8; }
            if (q[0x1278 / 4] == 0xD04061B8 && q[0x127c / 4] == 0x2ae) { q[0x127c / 4] = 0x247; n2c |= 8; }
            if (q[0x544c / 4] == 0xD041224C && q[0x5450 / 4] == 0x55b) { q[0x5450 / 4] = 0x48d; n2c |= 16; }
        }
    }
    int err = -1;
    if (*(volatile uint32_t *) OG3_TAB_H != 1164) err = unpatch_memory(OG3_TAB_H);
    if (*(volatile uint32_t *) 0xe19e5ddc == 0x2ae) unpatch_memory(0xe19e5ddc);
    if (*(volatile uint32_t *) 0xe19e5e18 == 0x2ae) unpatch_memory(0xe19e5e18);
    og3_armed = 0;
    snprintf(buf, sizeof(buf), "=== OG3 DISARM: restored (1 2c, 2 a68, 4 a93, 8 TG, 16 fe)=%d, unpatch err=%d, table reads %d", n2c, err,
             *(volatile uint32_t *) OG3_TAB_H);
    og3_log(buf);
    NotifyBox(6000, "OG3 disarmed. Movie size 4K then FHD again to apply");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(og3arm_h_start) __attribute__((unused));
static MENU_SELECT_FUNC(og3arm_h_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_arm_what = 1;
    og3_new_h = OG3_NEW_H;
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

static MENU_SELECT_FUNC(og3arm_c_start) __attribute__((unused));
static MENU_SELECT_FUNC(og3arm_c_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_arm_what = 2;
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

static MENU_SELECT_FUNC(og3arm_4k_start) __attribute__((unused));
static MENU_SELECT_FUNC(og3arm_4k_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_arm_what = 13;   /* 1 height + 4 in 4K + 8 rows */
    og3_new_h = 1370;    /* 34 OB + 1336 active; the sensor should send 34 + 1340 */
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

/* READ-ONLY (session 10 run 6): DIGIC timing generator (TG) registers per SensorMode.
 * ROM per-SensorMode TG lists = (addr, value, 0) triples, stride 0x21c. SM6 (movie FHD) around
 * 0xe19e5d9c: d0406198/19c = 2d1 (timer A), d04061a4 = 247 (timer B base), d04061ac = 20,
 * d04061b4 = 2, d04061b8 = 247 (value at 0xe19e5e18). d04061b8 tracks the capture lines:
 * SM6 247 = 1164/2+1, SM7 1a4 = 838/2+1, SM12 (4K) 89a = 2202, SM13 (x5) 4b4 = 1202+2.
 * Hypothesis: the DIGIC ends each frame there, so every taller capture waited forever (all OG3
 * height hangs). LV_MdChg3B (FUN_e083505c) sends per-mode lists from Q = *(0xeedc + 0x1c)
 * (base 0xeedc confirmed by the ROM literals), so log the live values and look for copies in Q. */
static void tgi_task()
{
    msleep(1000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/TGINFO.TXT");
    if (f)
    {
        uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
        my_fprintf(f, "=== TGINFO movie=%d lv=%d SensorMode=%d fps=%d armed=%d rom(e19e5e18)=%x\n", is_movie_mode(), lv, smode,
                   lv_vsync_fps_x1000(), og3_armed, *(volatile uint32_t *) 0xe19e5e18);
        static const uint32_t regs[] = { 0xD0406198, 0xD040619C, 0xD04061A0, 0xD04061A4, 0xD04061A8,
                                         0xD04061AC, 0xD04061B0, 0xD04061B4, 0xD04061B8, 0xD04061BC };
        my_fprintf(f, "  live:");
        for (int i = 0; i < COUNT(regs); i++)
            my_fprintf(f, " %x=%x", regs[i] & 0xfff, *(volatile uint32_t *) regs[i]);
        my_fprintf(f, "\n");
        uint32_t Q = *(volatile uint32_t *) (0xeedc + 0x1c);
        my_fprintf(f, "  Q=%x\n", Q);
        if (Q > 0x1000 && Q < 0x60000000)
        {
            int n = 0;
            for (uint32_t o = 0; o < 0xA000 && n < 40; o += 4)
            {
                uint32_t v = *(volatile uint32_t *) (Q + o);
                if (v == 0xD04061B8 || v == 0xD04061A4)
                {
                    my_fprintf(f, "  Q+%x: %x %x %x %x\n", o, v, *(volatile uint32_t *) (Q + o + 4),
                               *(volatile uint32_t *) (Q + o + 8), *(volatile uint32_t *) (Q + o + 12));
                    n++;
                }
            }
        }
        FIO_CloseFile(f);
    }
    NotifyBox(4000, "TG info: done. ML/LOGS/TGINFO.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(tgi_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("tgi_task", 0x1e, 0x2000, tgi_task, 0);
}

static MENU_SELECT_FUNC(og3arm_4kr_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_arm_what = 12;   /* 4 in 4K + 8 rows, capture stays 1164 */
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

static MENU_SELECT_FUNC(og3arm_4kl_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_arm_what = 28;   /* 4 in 4K + 8 rows + 16 TG/front-end lists, capture stays 1164 */
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

static MENU_SELECT_FUNC(og3arm_4kx_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_arm_what = 60;   /* 4 in 4K + 8 rows + 16 RAM lists + 32 ROM TG, capture stays 1164 */
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

static MENU_SELECT_FUNC(og3arm_r_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    og3_arm_what = 8;
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

/* READ-ONLY (session 10): SD driver state for the SD speed-up research (research/SD_SPEEDUP.md).
 * Driver struct RAM 0xb130 (DAT_e01732a4, SD init FUN_e0173106: +0x10 device, +0x14, +0x28/+0x2c
 * set to 2, +0x30 bus type); ROM speed table 0xe0ac1f04 {9,8,7,6} = 195/156/130/111 MHz. */
static void sdi_task()
{
    msleep(1000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/SDINFO.TXT");
    if (f)
    {
        my_fprintf(f, "=== SDINFO movie=%d lv=%d\n", is_movie_mode(), lv);
        smi_dump(f, "sd_struct", 0xb130, 0x30);
        uint32_t dev = *(volatile uint32_t *) (0xb130 + 0x10);
        if (dev > 0x1000 && dev < 0x60000000) smi_dump(f, "sd_device", dev, 0x40);
        smi_dump(f, "rom_speed_table", 0xe0ac1f04, 0x10);
        FIO_CloseFile(f);
    }
    NotifyBox(4000, "SD info: done. ML/LOGS/SDINFO.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(sdi_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("sdi_task", 0x1e, 0x2000, sdi_task, 0);
}

static MENU_SELECT_FUNC(og3arm_start) __attribute__((unused));
static MENU_SELECT_FUNC(og3arm_start)
{
    og3_arm_what = 3;
    og3_new_h = OG3_NEW_H;
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("og3arm_task", 0x1e, 0x2000, og3arm_task, 0);
}

static MENU_SELECT_FUNC(og3disarm_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("og3disarm_task", 0x1e, 0x2000, og3disarm_task, 0);
}

static MENU_SELECT_FUNC(og3_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("og3_task", 0x1e, 0x4000, og3_task, 0);
}

static MENU_SELECT_FUNC(rpa_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("rpa_task", 0x1e, 0x4000, rpa_task, 0);
}

static MENU_SELECT_FUNC(rp_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("rp_task", 0x1e, 0x2000, rp_task, 0);
}

static MENU_SELECT_FUNC(smi_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("smi_task", 0x1e, 0x2000, smi_task, 0);
}

/* WRITE test (session 8): ONE sensor register in Canon's own movie FHD 23.976 setup.
 * Movie mode 3 vsync list has 68=e7f4 (full-height style, like photo e7f5), the movie accum list
 * (sent after it, on every AE change) overrides 68=5c756 (16:9 band). Send {68=e7f4} once, copy
 * the LV raw frame after ~150 ms and ~600 ms (Canon may resend 5c756 on its next AE update),
 * then send {68=5c756} back. Expected if 68 is the vertical window: same 2096x1164 capture but a
 * different (taller / shifted) part of the sensor. Nothing else changes, same Canon mode. */
static uint32_t w68_list[4];

static void w68_send(uint32_t val)
{
    w68_list[0] = CMOS_W(0x68, val);
    w68_list[1] = 0xffffffff;
    ((void (*)(uint32_t *)) M50_CMOS_WRITE_LIST)(w68_list);
}

/* send {2c=v2c} and, if v68 != 0, {68=v68} */
static void w2c_send(uint32_t v2c, uint32_t v68)
{
    int k = 0;
    w68_list[k++] = CMOS_W(0x2c, v2c);
    if (v68) w68_list[k++] = CMOS_W(0x68, v68);
    w68_list[k] = 0xffffffff;
    ((void (*)(uint32_t *)) M50_CMOS_WRITE_LIST)(w68_list);
}

static void w68_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/W68.TXT");
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void w68_task()
{
    char buf[256];
    msleep(3000);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    uint32_t v68 = 0xffffffff, a68 = 0xffffffff, a50 = 0xffffffff;
    if (P > 0x1000 && P < 0x60000000 && mode == 3)
    {
        v68 = smi_list_get(P + 3 * 0x180 + 0x2314, 96, 0x68);
        a68 = smi_list_get(P + 3 * 0x6c + 0x2f14, 27, 0x68);
        a50 = smi_list_get(P + 3 * 0x6c + 0x2f14, 27, 0x50);
    }
    int fps = lv_vsync_fps_x1000();
    snprintf(buf, sizeof(buf), "=== W68 movie=%d lv=%d dispsize=%d SensorMode=%d mode=%d fps=%d vsync 68=%x accum 68=%x 50=%x",
             is_movie_mode(), lv, lv_dispsize, smode, mode, fps, v68, a68, a50);
    w68_log(buf);
    if (!lv || !is_movie_mode() || lv_dispsize != 1 || smode != 6 || mode != 3 || v68 != 0xe7f4
        || a68 != 0x5c756 || a50 != 0x618 || fps < 23000 || fps > 25000)
    {
        w68_log("abort: need movie FHD 23.976 LV (SensorMode 6, list mode 3, vsync 68=e7f4, accum 68=5c756 50=618)");
        NotifyBox(5000, "w68: abort (movie FHD 23.976). ML/LOGS/W68.TXT");
        rawdump_running = 0;
        return;
    }

    raw_lv_request();
    msleep(500);
    raw_update_params();
    int size = raw_info.frame_size;
    snprintf(buf, sizeof(buf), "raw %dx%d active %d,%d-%d,%d frame_size %d", raw_info.width, raw_info.height,
             raw_info.active_area.x1, raw_info.active_area.y1, raw_info.active_area.x2, raw_info.active_area.y2, size);
    w68_log(buf);
    void *mem[3] = { fio_malloc(size + 64), fio_malloc(size + 64), fio_malloc(size + 64) };
    void *cp[3];
    for (int i = 0; i < 3; i++) cp[i] = mem[i] ? (void *)(((uintptr_t) mem[i] + 63) & ~63) : 0;

    if (cp[0]) memcpy(cp[0], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase 0 Canon: top=%d bottom=%d", tgw_meter(100, 400),
             tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90, raw_info.active_area.y2 - raw_info.active_area.y1 - 10));
    w68_log(buf);

    w68_log("send 68=e7f4");
    w68_send(0xe7f4);
    msleep(150);
    if (cp[1]) memcpy(cp[1], raw_info.buffer, size);
    msleep(450);
    if (cp[2]) memcpy(cp[2], raw_info.buffer, size);
    w68_send(0x5c756);
    snprintf(buf, sizeof(buf), "back to 68=5c756: fps=%d top=%d bottom=%d accum68 now %x", lv_vsync_fps_x1000(), tgw_meter(100, 400),
             tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90, raw_info.active_area.y2 - raw_info.active_area.y1 - 10),
             smi_list_get(P + 3 * 0x6c + 0x2f14, 27, 0x68));
    w68_log(buf);
    raw_lv_release();

    static const char *names[3] = { "ML/LOGS/W68_0.DNG", "ML/LOGS/W68_A.DNG", "ML/LOGS/W68_B.DNG" };
    for (int i = 0; i < 3; i++)
    {
        if (!cp[i]) continue;
        NotifyBox(3000, "w68: saving DNG %d of 3...", i + 1);
        struct raw_info ri = raw_info;
        ri.buffer = cp[i];
        int rc = save_dng((char *) names[i], &ri);
        snprintf(buf, sizeof(buf), "%s save_dng=%d", names[i], rc);
        w68_log(buf);
    }
    for (int i = 0; i < 3; i++) if (mem[i]) fio_free(mem[i]);
    w68_log("done");
    NotifyBox(5000, "w68: done. ML/LOGS/W68.TXT + 3 DNG");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(w68_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("w68_task", 0x1e, 0x4000, w68_task, 0);
}

/* WRITE test (session 8): movie FHD 3x3 with the FULL-HEIGHT band registers.
 * ROM three-way diff (research/opengate/diff_345.txt) of mode 3 (3x3, 16:9 band), mode 4 (3x5,
 * full height) and mode 5 (3x5, 16:9 band): the ONLY register equal in both band modes and
 * different in the full-height mode is 0x2c (band 2b2, full 256); 0x68 is mixed (5c756 / 5bf5c
 * band, e7f5 full; W68 run: 68 alone moved nothing). About 40 other registers differ only with
 * the skip (3x3 vs 3x5) and stay Canon's movie values. Stay in Canon's own movie setup, send
 * phase A {2c=256}, phase B {2c=256, 68=e7f4}, then Canon's {2c=2b2, 68=5c756} back.
 * Expected if 2c is the band: a different vertical field of view (more of the sensor height)
 * in the same 2096x1164 capture. Sensor may output more lines than the DIGIC takes (risk). */
static void w2c_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/W2C.TXT");
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void w2c_task()
{
    char buf[256];
    msleep(3000);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    uint32_t v2c = 0xffffffff, v68 = 0xffffffff, a68 = 0xffffffff, a50 = 0xffffffff, a2c = 0xffffffff;
    if (P > 0x1000 && P < 0x60000000 && mode == 3)
    {
        v2c = smi_list_get(P + 3 * 0x180 + 0x2314, 96, 0x2c);
        v68 = smi_list_get(P + 3 * 0x180 + 0x2314, 96, 0x68);
        a68 = smi_list_get(P + 3 * 0x6c + 0x2f14, 27, 0x68);
        a50 = smi_list_get(P + 3 * 0x6c + 0x2f14, 27, 0x50);
        a2c = smi_list_get(P + 3 * 0x6c + 0x2f14, 27, 0x2c);
    }
    int fps = lv_vsync_fps_x1000();
    snprintf(buf, sizeof(buf), "=== W2C movie=%d lv=%d dispsize=%d SensorMode=%d mode=%d fps=%d vsync 2c=%x 68=%x accum 2c=%x 68=%x 50=%x",
             is_movie_mode(), lv, lv_dispsize, smode, mode, fps, v2c, v68, a2c, a68, a50);
    w2c_log(buf);
    if (!lv || !is_movie_mode() || lv_dispsize != 1 || smode != 6 || mode != 3 || v2c != 0x2b2 || v68 != 0xe7f4
        || a2c != 0xffffffff || a68 != 0x5c756 || a50 != 0x618 || fps < 23000 || fps > 25000)
    {
        w2c_log("abort: need movie FHD 23.976 LV (SensorMode 6, list mode 3, vsync 2c=2b2 68=e7f4, accum 68=5c756 50=618, no 2c in accum)");
        NotifyBox(5000, "w2c: abort (movie FHD 23.976). ML/LOGS/W2C.TXT");
        rawdump_running = 0;
        return;
    }

    raw_lv_request();
    msleep(500);
    raw_update_params();
    int size = raw_info.frame_size;
    void *mem[3] = { fio_malloc(size + 64), fio_malloc(size + 64), fio_malloc(size + 64) };
    void *cp[3];
    for (int i = 0; i < 3; i++) cp[i] = mem[i] ? (void *)(((uintptr_t) mem[i] + 63) & ~63) : 0;
    int h = raw_info.active_area.y2 - raw_info.active_area.y1;

    if (cp[0]) memcpy(cp[0], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase 0 Canon: raw %dx%d top=%d bottom=%d", raw_info.width, raw_info.height,
             tgw_meter(100, 400), tgw_meter(h - 90, h - 10));
    w2c_log(buf);

    w2c_log("phase A: send 2c=256");
    w2c_send(0x256, 0);
    msleep(250);
    if (cp[1]) memcpy(cp[1], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase A done: fps=%d top=%d bottom=%d", lv_vsync_fps_x1000(), tgw_meter(100, 400), tgw_meter(h - 90, h - 10));
    w2c_log(buf);

    w2c_log("phase B: send 2c=256 68=e7f4");
    w2c_send(0x256, 0xe7f4);
    msleep(250);
    if (cp[2]) memcpy(cp[2], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase B done: fps=%d top=%d bottom=%d", lv_vsync_fps_x1000(), tgw_meter(100, 400), tgw_meter(h - 90, h - 10));
    w2c_log(buf);

    w2c_send(0x2b2, 0x5c756);
    msleep(500);
    snprintf(buf, sizeof(buf), "restored 2c=2b2 68=5c756: fps=%d top=%d", lv_vsync_fps_x1000(), tgw_meter(100, 400));
    w2c_log(buf);
    raw_lv_release();

    static const char *names[3] = { "ML/LOGS/W2C_0.DNG", "ML/LOGS/W2C_A.DNG", "ML/LOGS/W2C_B.DNG" };
    for (int i = 0; i < 3; i++)
    {
        if (!cp[i]) continue;
        NotifyBox(3000, "w2c: saving DNG %d of 3...", i + 1);
        struct raw_info ri = raw_info;
        ri.buffer = cp[i];
        int rc = save_dng((char *) names[i], &ri);
        snprintf(buf, sizeof(buf), "%s save_dng=%d", names[i], rc);
        w2c_log(buf);
    }
    for (int i = 0; i < 3; i++) if (mem[i]) fio_free(mem[i]);
    w2c_log("done");
    NotifyBox(5000, "w2c: done. ML/LOGS/W2C.TXT + 3 DNG");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(w2c_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("w2c_task", 0x1e, 0x4000, w2c_task, 0);
}

/* WRITE test (session 8): full-height band via Canon's OWN movie list, sent whole.
 * Evidence: short PIO lists (CMOS24 50/9d/29, W68 68, W2C 2c) changed nothing; the whole mode-3
 * list (OG33 v2, in photo mode) clearly reached the sensor (hang). Every Canon vsync list ends
 * with reg 0x3e2 (ROM modes 3 and 4: ... 1f5 1f9 1fa 3e2) -> likely the sensor's "apply" latch.
 * Here we stay in movie FHD (Canon's mode 3 is running) and send Canon's own live mode-3 vsync
 * list + mode-3 accum list (exactly what Canon sends at LV start / framerate change):
 * phase 0 = unchanged lists (control: must change nothing), phase A = vsync 2c=256,
 * phase B = vsync 2c=256 + accum 68=e7f4; restore = unchanged lists. */
static uint32_t w2f_v[OG33_MAX], w2f_a[32], w2f_vm[OG33_MAX], w2f_am[32];

static int w2f_set(uint32_t *l, uint32_t reg, uint32_t val)
{
    for (int i = 0; l[i] != 0xffffffff; i++)
        if ((l[i] >> 22) == reg) { l[i] = CMOS_W(reg, val); return 1; }
    return 0;
}

static void w2f_log(const char *line)
{
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/W2F.TXT");
    if (f) { my_fprintf(f, "%s\n", line); FIO_CloseFile(f); }
}

static void w2f_task()
{
    char buf[256];
    msleep(3000);
    uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
    uint32_t smode = *(volatile uint32_t *) (0xef24 + 8);
    uint32_t mode = (smode < 14) ? *(volatile uint32_t *) (0xe0b3ab64 + smode * 4 + 0xa8) : 0xff;
    int nv = -1, na = -1;
    if (P > 0x1000 && P < 0x60000000 && mode == 3)
    {
        nv = og33_copy(w2f_v, P + 3 * 0x180 + 0x2314, OG33_MAX);
        na = og33_copy(w2f_a, P + 3 * 0x6c + 0x2f14, 32);
    }
    int fps = lv_vsync_fps_x1000();
    uint32_t last = (nv > 0) ? (w2f_v[nv - 1] >> 22) : 0;
    snprintf(buf, sizeof(buf), "=== W2F movie=%d lv=%d dispsize=%d SensorMode=%d mode=%d fps=%d nv=%d na=%d last vsync reg=%x 2c=%x 64=%x 65=%x | accum 68=%x 50=%x",
             is_movie_mode(), lv, lv_dispsize, smode, mode, fps, nv, na, last,
             (nv > 0) ? og33_get(w2f_v, 0x2c) : 0, (nv > 0) ? og33_get(w2f_v, 0x64) : 0, (nv > 0) ? og33_get(w2f_v, 0x65) : 0,
             (na > 0) ? og33_get(w2f_a, 0x68) : 0, (na > 0) ? og33_get(w2f_a, 0x50) : 0);
    w2f_log(buf);
    if (!lv || !is_movie_mode() || lv_dispsize != 1 || smode != 6 || mode != 3 || nv < 40 || na < 10
        || last != 0x3e2 || og33_get(w2f_v, 0x2c) != 0x2b2 || og33_get(w2f_v, 0x64) != 0x222204
        || og33_get(w2f_v, 0x65) != 0x303703 || og33_get(w2f_a, 0x68) != 0x5c756 || og33_get(w2f_a, 0x50) != 0x618
        || fps < 23000 || fps > 25000)
    {
        w2f_log("abort: need movie FHD 23.976 (mode 3 lists as in ROM, vsync ends with 3e2, 2c=2b2, accum 68=5c756 50=618)");
        NotifyBox(5000, "w2f: abort (movie FHD 23.976). ML/LOGS/W2F.TXT");
        rawdump_running = 0;
        return;
    }

    raw_lv_request();
    msleep(500);
    raw_update_params();
    int size = raw_info.frame_size;
    int h = raw_info.active_area.y2 - raw_info.active_area.y1;
    void *mem[4] = { fio_malloc(size + 64), fio_malloc(size + 64), fio_malloc(size + 64), fio_malloc(size + 64) };
    void *cp[4];
    for (int i = 0; i < 4; i++) cp[i] = mem[i] ? (void *)(((uintptr_t) mem[i] + 63) & ~63) : 0;

    if (cp[0]) memcpy(cp[0], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "before: raw %dx%d fps=%d top=%d bottom=%d", raw_info.width, raw_info.height,
             lv_vsync_fps_x1000(), tgw_meter(100, 400), tgw_meter(h - 90, h - 10));
    w2f_log(buf);

    /* phase 0: control, Canon's lists unchanged */
    w2f_log("phase 0: resend Canon lists unchanged");
    og33_send_list(w2f_v, 0);
    og33_send_list(w2f_a, 0);
    msleep(300);
    if (cp[1]) memcpy(cp[1], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase 0 done: fps=%d top=%d bottom=%d", lv_vsync_fps_x1000(), tgw_meter(100, 400), tgw_meter(h - 90, h - 10));
    w2f_log(buf);

    /* phase A: vsync 2c=256 */
    memcpy(w2f_vm, w2f_v, sizeof(w2f_v)); memcpy(w2f_am, w2f_a, sizeof(w2f_a));
    w2f_set(w2f_vm, 0x2c, 0x256);
    w2f_log("phase A: vsync 2c=256");
    og33_send_list(w2f_vm, 0);
    og33_send_list(w2f_am, 0);
    msleep(300);
    if (cp[2]) memcpy(cp[2], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase A done: fps=%d top=%d bottom=%d", lv_vsync_fps_x1000(), tgw_meter(100, 400), tgw_meter(h - 90, h - 10));
    w2f_log(buf);

    /* phase B: + accum 68=e7f4 */
    w2f_set(w2f_am, 0x68, 0xe7f4);
    w2f_log("phase B: vsync 2c=256 + accum 68=e7f4");
    og33_send_list(w2f_vm, 0);
    og33_send_list(w2f_am, 0);
    msleep(300);
    if (cp[3]) memcpy(cp[3], raw_info.buffer, size);
    snprintf(buf, sizeof(buf), "phase B done: fps=%d top=%d bottom=%d", lv_vsync_fps_x1000(), tgw_meter(100, 400), tgw_meter(h - 90, h - 10));
    w2f_log(buf);

    /* restore: Canon's lists unchanged */
    og33_send_list(w2f_v, 0);
    og33_send_list(w2f_a, 0);
    msleep(500);
    snprintf(buf, sizeof(buf), "restored: fps=%d top=%d", lv_vsync_fps_x1000(), tgw_meter(100, 400));
    w2f_log(buf);
    raw_lv_release();

    static const char *names[4] = { "ML/LOGS/W2F_S.DNG", "ML/LOGS/W2F_0.DNG", "ML/LOGS/W2F_A.DNG", "ML/LOGS/W2F_B.DNG" };
    for (int i = 0; i < 4; i++)
    {
        if (!cp[i]) continue;
        NotifyBox(3000, "w2f: saving DNG %d of 4...", i + 1);
        struct raw_info ri = raw_info;
        ri.buffer = cp[i];
        int rc = save_dng((char *) names[i], &ri);
        snprintf(buf, sizeof(buf), "%s save_dng=%d", names[i], rc);
        w2f_log(buf);
    }
    for (int i = 0; i < 4; i++) if (mem[i]) fio_free(mem[i]);
    w2f_log("done");
    NotifyBox(5000, "w2f: done. ML/LOGS/W2F.TXT + 4 DNG");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(w2f_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("w2f_task", 0x1e, 0x4000, w2f_task, 0);
}

/* READ-ONLY: the per-frame CMOS path. CMOS24 run 1: our PIO list (FUN_e0824d02) changed
 * nothing -> either the driver was not ready (flag *(0xef84+4) must be 1) or Canon resends
 * the sensor registers every frame by DMA (FUN_e0824e34 "Start CMOSDMA", u16 data, n <= 0x1400):
 * it writes the source to **(0x8ba24-0x20) (-> MMIO 0xC0500040) and n*2 to **(0x8ba24-0x1c).
 * Log driver struct, the pointer table and the last DMA buffer, a few times. */
static void cmosdma_task()
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/CMOSDMA.TXT");
    if (!f) { rawdump_running = 0; return; }
    my_fprintf(f, "=== movie=%d fps_x1000=%d timerB=%x\n", is_movie_mode(), lv_vsync_fps_x1000(),
               *(volatile uint32_t *) 0xD04061A4);
    my_fprintf(f, "  drv ef84:");
    for (int i = 0; i < 12; i++) my_fprintf(f, " %08x", ((volatile uint32_t *) 0xef84)[i]);
    my_fprintf(f, "\n  ptrs 8ba04:");
    for (int i = 0; i < 32; i++) my_fprintf(f, " %08x", ((volatile uint32_t *) 0x8ba04)[i]);
    my_fprintf(f, "\n");
    for (int s = 0; s < 4; s++)
    {
        uint32_t src_reg = ((volatile uint32_t *) 0x8ba04)[0];   /* 0x8ba24 - 0x20 */
        uint32_t len_reg = ((volatile uint32_t *) 0x8ba04)[1];   /* 0x8ba24 - 0x1c */
        uint32_t src = 0, len = 0;
        if ((src_reg >> 24) == 0xc0 || (src_reg >> 24) == 0xd0) src = *(volatile uint32_t *) src_reg;
        if ((len_reg >> 24) == 0xc0 || (len_reg >> 24) == 0xd0) len = *(volatile uint32_t *) len_reg;
        my_fprintf(f, "s%d: src_reg=%x src=%x len_reg=%x len=%x |", s, src_reg, src, len_reg, len);
        int n = (len / 2 > 0 && len / 2 <= 0x200) ? len / 2 : 0;
        if (src > 0x1000 && src < 0x60000000 && !(src & 1))
            for (int i = 0; i < (n ? n : 64); i++)
                my_fprintf(f, " %04x", ((volatile uint16_t *) src)[i]);
        my_fprintf(f, "\n");
        msleep(150);
    }
    FIO_CloseFile(f);
    NotifyBox(5000, "cmosdma: done. ML/LOGS/CMOSDMA.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(cmosdma_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("cmosdma_task", 0x1e, 0x2000, cmosdma_task, 0);
}

/* READ-ONLY: Canon's CRX (CR3) RAW encoder state. M50 has no JPCORE/LJ92 lossless path
 * (no TwoInTwoOut strings); CR3 RAW is encoded by the CRAW/CRX unit (MMIO 0xD0630000,
 * driver "CRaw/AHD", IRQ 0x1c7/0x1d7) via ALGSMGR (AlgsMgr.c, init at boot:
 * ALGSMGR_Initialize(0x3f,0x13,0)). Chain (ROM): ImageCodecEncState "Start Enc" (FUN_e0829428)
 * -> FUN_e083e60c -> ALGSMGR_StartEncode FUN_e0795850(entry[8], 1, cbr, ctx), entry =
 * {type, Ds+0x1b8, Ds+0x1bc, Ds+0x280, Ds+0x284, 14, rawaddr, Ds+0x28}, Ds = *(0xb424).
 * Globals: pAlgsMgrData *(0x5a20); CRaw AHD *(0x111b8) (+0x18 cur id, +0x1a n ids,
 * +0x28 per-id blocks 0x288 B, set only while encoding); ImageCodec enc state 0x1178c.
 * Polls RAM every 5 ms for 30 s, keeps up to 24 snapshots that differ, writes them at
 * the end -> ML/LOGS/CRXST.TXT. No MMIO, no ROM calls, no writes. */
#define CRX_NSNAP 24
#define CRX_DSN   0x100      /* words of Ds */
#define CRX_AMN   0x30       /* words of pAlgsMgrData */
#define CRX_CGN   0x10       /* words of CRaw AHD global */
#define CRX_IDN   (0x288/4)  /* words per id block */
#define CRX_NID   3
struct crx_snap
{
    uint32_t t_ms, am, cg, ds, nid, blk;
    uint32_t amw[CRX_AMN], cgw[CRX_CGN], es[8];
    uint32_t idw[CRX_NID][CRX_IDN];
};
static int crx_ptr_ok(uint32_t p)
{
    /* M50 RAM: cached 0x00000000.., uncached alias 0x40000000.. (Canon LV raw sits at 0x6DCE7518) */
    return !(p & 3) && ((p >= 0x1000 && p < 0x40000000) || (p >= 0x40001000 && p < 0x80000000));
}
static void crx_take(struct crx_snap *s, uint32_t t0)
{
    memset(s, 0, sizeof(*s));
    s->t_ms = get_ms_clock() - t0;
    s->am = *(volatile uint32_t *) 0x5a20;
    s->cg = *(volatile uint32_t *) 0x111b8;
    s->ds = *(volatile uint32_t *) 0xb424;
    for (int i = 0; i < 8; i++) s->es[i] = ((volatile uint32_t *) 0x1178c)[i];
    if (crx_ptr_ok(s->am) && crx_ptr_ok(s->am + CRX_AMN * 4))
        for (int i = 0; i < CRX_AMN; i++) s->amw[i] = ((volatile uint32_t *) s->am)[i];
    if (crx_ptr_ok(s->cg) && crx_ptr_ok(s->cg + CRX_CGN * 4))
    {
        for (int i = 0; i < CRX_CGN; i++) s->cgw[i] = ((volatile uint32_t *) s->cg)[i];
        s->nid = ((volatile uint8_t *) s->cg)[0x1a];
        s->blk = s->cgw[0x28 / 4];
        int n = MIN((int) s->nid + 1, CRX_NID);
        if (crx_ptr_ok(s->blk) && crx_ptr_ok(s->blk + n * 0x288))
            for (int k = 0; k < n; k++)
                for (int i = 0; i < CRX_IDN; i++)
                    s->idw[k][i] = ((volatile uint32_t *) (s->blk + k * 0x288))[i];
    }
}
static void crx_dump_words(FILE *f, const char *name, uint32_t base, uint32_t *w, int n)
{
    my_fprintf(f, "  %s @%08x:", name, base);
    for (int i = 0; i < n; i++)
    {
        if (i % 8 == 0) my_fprintf(f, "\n   +%03x:", i * 4);
        my_fprintf(f, " %08x", w[i]);
    }
    my_fprintf(f, "\n");
}
static void crxst_task()
{
    msleep(2000);
    struct crx_snap *snaps = malloc(CRX_NSNAP * sizeof(struct crx_snap));
    struct crx_snap *cur = malloc(sizeof(struct crx_snap));
    uint32_t *ds0 = malloc(CRX_DSN * 4), *ds1 = malloc(CRX_DSN * 4);
    if (!snaps || !cur || !ds0 || !ds1) { NotifyBox(5000, "crxst: malloc failed"); goto out; }
    uint32_t t0 = get_ms_clock();
    uint32_t ds = *(volatile uint32_t *) 0xb424;
    memset(ds0, 0, CRX_DSN * 4);
    if (crx_ptr_ok(ds) && crx_ptr_ok(ds + CRX_DSN * 4))
        for (int i = 0; i < CRX_DSN; i++) ds0[i] = ((volatile uint32_t *) ds)[i];
    int n = 0, polls = 0, cg_seen = 0;
    NotifyBox(3000, "crxst: take ONE RAW photo now (30 s)");
    while (get_ms_clock() - t0 < 30000)
    {
        crx_take(cur, t0);
        polls++;
        if (cur->cg) cg_seen++;
        if (n < CRX_NSNAP && (n == 0 ||
            memcmp(&cur->am, &snaps[n - 1].am, sizeof(*cur) - offsetof(struct crx_snap, am))))
            memcpy(&snaps[n++], cur, sizeof(*cur));
        msleep(5);
    }
    ds = *(volatile uint32_t *) 0xb424;
    memset(ds1, 0, CRX_DSN * 4);
    if (crx_ptr_ok(ds) && crx_ptr_ok(ds + CRX_DSN * 4))
        for (int i = 0; i < CRX_DSN; i++) ds1[i] = ((volatile uint32_t *) ds)[i];

    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/CRXST.TXT");
    if (f)
    {
        my_fprintf(f, "=== movie=%d polls=%d cg_nonzero=%d snaps=%d Ds=%x\n", is_movie_mode(), polls, cg_seen, n, ds);
        crx_dump_words(f, "Ds before", ds, ds0, CRX_DSN);
        crx_dump_words(f, "Ds after ", ds, ds1, CRX_DSN);
        for (int k = 0; k < n; k++)
        {
            struct crx_snap *s = &snaps[k];
            my_fprintf(f, "--- snap %d t=%d ms am=%x cg=%x nid=%d blk=%x es: %x %x %x %x %x %x %x %x\n",
                       k, s->t_ms, s->am, s->cg, s->nid, s->blk,
                       s->es[0], s->es[1], s->es[2], s->es[3], s->es[4], s->es[5], s->es[6], s->es[7]);
            crx_dump_words(f, "AlgsMgr", s->am, s->amw, CRX_AMN);
            if (s->cg)
            {
                crx_dump_words(f, "CRawGlob", s->cg, s->cgw, CRX_CGN);
                for (int i = 0; i < MIN((int) s->nid + 1, CRX_NID); i++)
                    crx_dump_words(f, "IdBlock", s->blk + i * 0x288, s->idw[i], CRX_IDN);
            }
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "crxst: done (%d snaps). ML/LOGS/CRXST.TXT", n);
out:
    if (snaps) free(snaps);
    if (cur) free(cur);
    if (ds0) free(ds0);
    if (ds1) free(ds1);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(crxst_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("crxst_task", 0x1e, 0x2000, crxst_task, 0);
}

/* ONE-SHOT CALL TEST: Canon's CRX (CR3 RAW) encoder on ONE LiveView RAW frame.
 * ROM (sign 1): ALGSMGR_StartEncode FUN_e0795850(entry[8], n=1, cbr, ctx), called by
 * ImageCodecEncState (FUN_e0829428 -> FUN_e083e60c). entry = {type, width, height, 0, 0,
 * bitdepth 14, raw address, pitch in pixels}; input = 14-bit packed, line bytes =
 * 14 * pitch / 8 (algsmgrEncInit FUN_e024dc3e); tiles = (width >> 12) + 1.
 * Camera (sign 2, CRXST run 1): pAlgsMgrData *(0x5a20) live (+0x14 0x3f, +0x18 0x13 = the
 * boot ALGSMGR_Initialize args); Canon's entry fields Ds+0x1b8/0x1bc = 6288x4056 = CR3 size.
 * type 2 -> Id 2 (FUN_e024db5e): Canon never uses it (block 2 = stale memory in CRXST), so
 * algsmgrEncInit runs once with OUR size and Canon's photo Ids (0/1/4/5/10-13) stay untouched.
 * Completion: cbr(ctx, handle, 0x24) from the algsmgr task (FUN_e024bdda; handle & 1 = error).
 * Output: GetRingList FUN_e0795ca8(handle, &n, &addrs, &sizes) (checks the signature, returns 7
 * if wrong); release: ALGSMGR_FreeEncBuffer FUN_e0795d2e(handle, 0, 0) (as ImageCodec does).
 * Output buffers come from Canon's resource manager (algsmgr asks it itself) -> photo mode,
 * mlv_lite idle (photo LV memory is only taken on REC). On timeout the input buffer is kept
 * (never freed) and a late cbr frees Canon's buffer itself. Files: ML/LOGS/CRXENC.TXT,
 * CRXIN.RAW (our exact 14-bit input), CRXOUT.BIN (all output chunks, table in the TXT). */
/* run 2 (raw_tests/crxenc2): type 2 = Id 2 = CtMode 3 = 3 DWT levels (C-RAW style, block +0x40
 * 0x0403000e) and the encoder reads a plain LSB-first 14-bit stream, not Canon's LV pixblock
 * packing -> output = scrambled image. Run 3: type 1 = Id 1 = CtMode 0 (am+0xa8 bit1 = 0) =
 * levels 0 = Canon's lossless CR3 mode (CR3 small-RAW track, 1624x1080). Id 1 is set up only
 * once (algsmgrEncInit runs when the flag am+0x108+Id*4 == 0, FUN_e024dc3e) -> clear it before
 * our call (our size) and again after (Canon re-inits Id 1 with its own size at the next photo).
 * Input is repacked on the CPU (pixblock -> LSB-first) for this one frame. */
#define CRXE_TYPE   1
#define CRXE_ID     1
#define CRXE_H      832          /* multiple of 8; photo LV raw has 838 rows */
static uint32_t (*ALGSMGR_StartEncode)(uint32_t *entry, int n, void (*cbr)(void *, uint32_t, uint32_t), void *ctx)
    = (void *) 0xe0795851;
static uint32_t (*ALGSMGR_GetRingList)(uint32_t handle, int *n, uint32_t **addrs, uint32_t **sizes)
    = (void *) 0xe0795ca9;
static uint32_t (*ALGSMGR_FreeEncBuffer)(uint32_t handle, int a, int b) = (void *) 0xe0795d2f;
static struct semaphore *crxe_sem = 0;
static volatile uint32_t crxe_handle = 0, crxe_flag = 0, crxe_done = 0, crxe_late = 0, crxe_t_cbr = 0;
static volatile int crxe_used = 0;   /* one call per boot: Id 2 stays initialized for our size */

/* Canon LV raw_pixblock (8 px / 14 B, MSB-first in 16-bit words) -> plain LSB-first 14-bit stream */
static void crxe_repack(const uint8_t *src, uint8_t *dst, int nblocks)
{
    for (int i = 0; i < nblocks; i++)
    {
        const struct raw_pixblock *p = (const struct raw_pixblock *) (src + i * 14);
        uint32_t px[8] = {
            p->a, (p->b_hi << 12) | p->b_lo, (p->c_hi << 10) | p->c_lo, (p->d_hi << 8) | p->d_lo,
            (p->e_hi << 6) | p->e_lo, (p->f_hi << 4) | p->f_lo, (p->g_hi << 2) | p->g_lo, p->h };
        uint32_t acc = 0; int nb = 0;
        for (int k = 0; k < 8; k++)
        {
            acc |= px[k] << nb; nb += 14;
            while (nb >= 8) { *dst++ = acc & 0xff; acc >>= 8; nb -= 8; }
        }
    }
}

static void crxe_cbr(void *ctx, uint32_t handle, uint32_t flag)
{
    crxe_handle = handle;
    crxe_flag = flag;
    crxe_t_cbr = get_us_clock();
    crxe_done = 1;
    if (crxe_late)
    {
        if (handle && !(handle & 1))
            ALGSMGR_FreeEncBuffer(handle, 0, 0);   /* we already gave up: just give it back */
        return;
    }
    give_semaphore(crxe_sem);
}

static void crxenc_task()
{
    void *in_raw = 0, *enc_raw = 0;
    int keep_input = 0;
    volatile uint32_t *idflag = 0;
    msleep(3000);
    FILE *f = FIO_CreateFile("ML/LOGS/CRXENC.TXT");
    if (!f) { rawdump_running = 0; return; }
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    my_fprintf(f, "=== CRX one-shot type=%d movie=%d lv=%d am=%x used=%d\n", CRXE_TYPE, is_movie_mode(), lv, am, crxe_used);
    if (crxe_used || is_movie_mode() || !lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f)
    {
        my_fprintf(f, "gate failed: need photo mode LiveView, ALGSMGR (am+0x14 = %x), first run after boot\n",
                   crx_ptr_ok(am) ? ((volatile uint32_t *) am)[5] : 0);
        NotifyBox(5000, "crxenc: gate failed (photo M + LV, once per boot)");
        goto end;
    }
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);

    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    my_fprintf(f, "raw ok=%d buf=%x %dx%d pitch=%d bpp=%d black=%d white=%d\n", ok, raw_info.buffer,
               raw_info.width, raw_info.height, raw_info.pitch, raw_info.bits_per_pixel,
               raw_info.black_level, raw_info.white_level);
    int W = raw_info.width, pitch = raw_info.pitch;
    if (!ok || raw_info.bits_per_pixel != 14 || W < 1024 || W > 4000 || (W & 15) || pitch != W * 14 / 8
        || raw_info.height < CRXE_H)
    {
        my_fprintf(f, "gate failed: raw geometry\n");
        raw_lv_release();
        NotifyBox(5000, "crxenc: raw geometry gate failed");
        goto end;
    }
    int in_size = pitch * CRXE_H;
    in_raw = fio_malloc(in_size + 128);
    enc_raw = fio_malloc(in_size + 128);
    if (!in_raw || !enc_raw) { my_fprintf(f, "fio_malloc in failed\n"); raw_lv_release(); goto end; }
    void *in = (void *)(((uintptr_t) in_raw + 63) & ~63);
    memcpy(CACHEABLE(in), UNCACHEABLE(raw_info.buffer), in_size);
    dcache_clean((uint32_t) CACHEABLE(in), in_size);
    raw_lv_release();

    FILE *fi = FIO_CreateFile("ML/LOGS/CRXIN.RAW");
    int wi = fi ? FIO_WriteFile(fi, CACHEABLE(in), in_size) : -1;
    if (fi) FIO_CloseFile(fi);
    my_fprintf(f, "input %x (%dx%d, %d B) saved=%d\n", in, W, CRXE_H, in_size, wi);

    void *enc = (void *)(((uintptr_t) enc_raw + 63) & ~63);
    uint32_t tr0 = get_us_clock();
    crxe_repack(CACHEABLE(in), CACHEABLE(enc), in_size / 14);
    dcache_clean((uint32_t) CACHEABLE(enc), in_size);
    my_fprintf(f, "repack pixblock -> LSB14: %d us, enc buffer %x\n", (int)(get_us_clock() - tr0), enc);

    /* per-Id "EncInit done" flags (ROM: am+0x108+Id*4); all must be 0/1 */
    my_fprintf(f, "am+0xa8=%x initflags:", ((volatile uint32_t *) am)[0xa8 / 4]);
    int flags_ok = 1;
    for (int i = 0; i < 14; i++)
    {
        uint32_t v = ((volatile uint32_t *) (am + 0x108))[i];
        my_fprintf(f, " %d:%x", i, v);
        if (v > 1) flags_ok = 0;
    }
    my_fprintf(f, "\n");
    if (!flags_ok || (((volatile uint32_t *) am)[0xa8 / 4] & 2))
    {
        my_fprintf(f, "gate failed: init flags not 0/1 or am+0xa8 bit1 set (CtMode would be 3)\n");
        NotifyBox(5000, "crxenc: flag gate failed");
        goto end;
    }
    idflag = (volatile uint32_t *) (am + 0x108 + CRXE_ID * 4);
    *idflag = 0;   /* our size for Id 1 */

    uint32_t entry[8] = { CRXE_TYPE, W, CRXE_H, 0, 0, 14, (uint32_t) UNCACHEABLE(enc), W };
    crxe_done = 0; crxe_handle = 0; crxe_late = 0; crxe_used = 1;
    uint32_t t0 = get_us_clock();
    uint32_t r = ALGSMGR_StartEncode(entry, 1, crxe_cbr, (void *) 0x43525845);
    my_fprintf(f, "StartEncode(%d,%d,%d,0,0,14,%x,%d) = %x\n", entry[0], entry[1], entry[2],
               entry[6], entry[7], r);
    if (r & 1) { NotifyBox(5000, "crxenc: StartEncode error %x", r); goto end; }
    keep_input = 1;   /* from here on the encoder may read our input */

    int to = take_semaphore(crxe_sem, 3000);
    *idflag = 0;   /* Canon re-initializes Id 1 with its own size at the next photo */
    my_fprintf(f, "wait=%d done=%d handle=%x flag=%x time=%d us\n", to, crxe_done, crxe_handle,
               crxe_flag, crxe_done ? (int)(crxe_t_cbr - t0) : -1);
    if (!crxe_done)
    {
        crxe_late = 1;   /* a late cbr will free Canon's buffer; our input stays allocated */
        my_fprintf(f, "TIMEOUT: no result in 3 s (input buffer kept on purpose)\n");
        NotifyBox(8000, "crxenc: TIMEOUT. No photos, power off.");
        goto end;
    }
    keep_input = 0;   /* encode finished: the input is no longer read */
    uint32_t h = crxe_handle;
    if (!h || (h & 1)) { my_fprintf(f, "encoder returned error %x\n", h); NotifyBox(5000, "crxenc: error %x", h); goto end; }

    int n = 0; uint32_t *addrs = 0, *sizes = 0;
    uint32_t g = ALGSMGR_GetRingList(h, &n, &addrs, &sizes);
    my_fprintf(f, "GetRingList=%x n=%d addrs=%x sizes=%x\n", g, n, addrs, sizes);
    int total = 0;
    /* run 1: n = 81 (> old limit 64) -> nothing saved. Log the whole table now. */
    if (g == 0 && n > 0 && n <= 1024 && crx_ptr_ok((uint32_t) addrs) && crx_ptr_ok((uint32_t) sizes))
    {
        FILE *fo = FIO_CreateFile("ML/LOGS/CRXOUT.BIN");
        void *tmp_raw = fio_malloc(256 * 1024 + 64);
        void *tmp = tmp_raw ? (void *)(((uintptr_t) tmp_raw + 63) & ~63) : 0;
        for (int i = 0; i < n; i++)
        {
            uint32_t a = addrs[i], s = sizes[i];
            int good = a && s && s < 64 * 1024 * 1024 && crx_ptr_ok(a & ~3) && crx_ptr_ok((a + s + 3) & ~3);
            my_fprintf(f, " chunk %d: addr=%x size=%d (%s) first:", i, a, s, good ? "ok" : "skip");
            if (good)
                for (int k = 0; k < 16 && k < (int) s; k++)
                    my_fprintf(f, " %02x", ((volatile uint8_t *) UNCACHEABLE(a))[k]);
            my_fprintf(f, "\n");
            if (good) my_fprintf(f, "  -> file offset %d\n", total);
            if (good && fo && tmp)
            {
                for (uint32_t off = 0; off < s; off += 256 * 1024)
                {
                    uint32_t len = MIN(s - off, 256 * 1024);
                    memcpy(tmp, UNCACHEABLE(a + off), len);
                    FIO_WriteFile(fo, tmp, len);
                }
                total += s;
            }
        }
        if (fo) FIO_CloseFile(fo);
        if (tmp_raw) fio_free(tmp_raw);
    }
    my_fprintf(f, "output total %d B = %d.%02d %% of input\n", total,
               (int)((int64_t) total * 100 / in_size), (int)((int64_t) total * 10000 / in_size % 100));

    /* encoder state for Id 2 (header parameters) */
    uint32_t cg = *(volatile uint32_t *) 0x111b8;
    if (crx_ptr_ok(cg))
    {
        uint32_t blk = ((volatile uint32_t *) cg)[0x28 / 4];
        if (crx_ptr_ok(blk) && crx_ptr_ok(blk + 3 * 0x288))
        {
            static uint32_t w[CRX_IDN];
            for (int i = 0; i < CRX_IDN; i++) w[i] = ((volatile uint32_t *) (blk + CRXE_ID * 0x288))[i];
            crx_dump_words(f, "IdBlock", blk + CRXE_ID * 0x288, w, CRX_IDN);
        }
    }
    uint32_t fr = ALGSMGR_FreeEncBuffer(h, 0, 0);
    my_fprintf(f, "FreeEncBuffer=%x\n", fr);
    NotifyBox(8000, "crxenc: done, %d B out. ML/LOGS/CRXENC.TXT", total);
end:
    if (idflag) my_fprintf(f, "Id %d init flag now %x\n", CRXE_ID, *idflag);
    if (in_raw && !keep_input) fio_free(in_raw);
    if (enc_raw && !keep_input) fio_free(enc_raw);
    FIO_CloseFile(f);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(crxenc_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("crxenc_task", 0x1e, 0x2000, crxenc_task, 0);
}

/* CALL test: hardware path for lossless video. Run 3 (raw_tests/crxenc3) was BIT-EXACT but
 * needed a CPU repack (pixblock -> LSB-first 14-bit, 212 ms/frame). Idea: let the EDMAC
 * unpack the LV raw to plain 16-bit samples (u16 little-endian = an LSB-first 16-bit stream)
 * and give the encoder bit depth 16. ROM signs: Canon's EHD MultipleAddRaw FUN_e084d0e0 writes
 * 16-bit with pack cfg {1, 0, 2, 1, 0} (cfg0 = 1 -> 16-bit), line = 16 * w / 8, transfer mode 1;
 * our mem2mem ch61 -> ch24 + Pack/Unpack units are proven (pack tests #2..#5, 10/12-bit video);
 * CRaw AHD EncInit FUN_e02aacd0 only asserts BitDepth >= 8 and tile >= 0x2c.
 * Steps: (1) 64-line EDMAC trial for 2 variants (mode 1 / mode 0), CPU check vs pixblock decode;
 * (2) whole frame with the first exact variant, full check; (3) encode with entry[5] = 16
 * (type 1 = Id 1 lossless, flag cleared before/after as in run 3). Files: ML/LOGS/CRX16.TXT,
 * CRX16IN.RAW (pixblock frame), CRX16.RAW (EDMAC 16-bit frame), CRX16OUT.BIN (encoder output). */
static inline uint32_t crx16_px(const uint8_t *blk, int k)
{
    const struct raw_pixblock *p = (const struct raw_pixblock *) blk;
    switch (k)
    {
        case 0: return p->a;
        case 1: return (p->b_hi << 12) | p->b_lo;
        case 2: return (p->c_hi << 10) | p->c_lo;
        case 3: return (p->d_hi << 8) | p->d_lo;
        case 4: return (p->e_hi << 6) | p->e_lo;
        case 5: return (p->f_hi << 4) | p->f_lo;
        case 6: return (p->g_hi << 2) | p->g_lo;
        default: return p->h;
    }
}
/* number of pixels (first n) where the u16 output differs from the pixblock source */
static int crx16_check(const uint8_t *src, const uint16_t *d16, int n, int *first)
{
    int bad = 0; *first = -1;
    for (int i = 0; i < n; i++)
        if (d16[i] != crx16_px(src + (i / 8) * 14, i % 8)) { if (*first < 0) *first = i; bad++; }
    return bad;
}

static void crx16_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    static uint32_t rd14[4] = { 0, 2, 1, 0 };             /* Canon read unpack 14-bit (reg 0x21) */
    static uint32_t wr16[5] = { 1, 0, 2, 1, 0 };          /* MultipleAddRaw 16-bit write */
    void *in_raw = 0, *o_raw = 0;
    int keep = 0, locked = 0;
    void *lock = 0;
    volatile uint32_t *idflag = 0;
    uint32_t o_rd24 = 0, o_wr24 = 0, o_rdD0 = 0, o_wrD0 = 0;
    msleep(3000);
    FILE *f = FIO_CreateFile("ML/LOGS/CRX16.TXT");
    if (!f) { rawdump_running = 0; return; }
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    my_fprintf(f, "=== CRX16 movie=%d lv=%d am=%x used=%d\n", is_movie_mode(), lv, am, crxe_used);
    if (crxe_used || is_movie_mode() || !lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f)
    {
        my_fprintf(f, "gate failed: photo mode LiveView, ALGSMGR, once per boot\n");
        NotifyBox(5000, "crx16: gate failed (photo M + LV, once per boot)");
        goto end;
    }
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);

    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    int W = raw_info.width, pitch = raw_info.pitch;
    my_fprintf(f, "raw ok=%d buf=%x %dx%d pitch=%d bpp=%d\n", ok, raw_info.buffer, W, raw_info.height, pitch, raw_info.bits_per_pixel);
    if (!ok || raw_info.bits_per_pixel != 14 || W < 1024 || W > 4000 || (W & 15) || pitch != W * 14 / 8 || raw_info.height < CRXE_H)
    {
        my_fprintf(f, "gate failed: raw geometry\n"); raw_lv_release(); goto end;
    }
    int in_size = pitch * CRXE_H, o_size = W * 2 * CRXE_H;
    in_raw = fio_malloc(in_size + 128);
    o_raw = fio_malloc(o_size + M2M_GUARD + 128);
    if (!in_raw || !o_raw) { my_fprintf(f, "fio_malloc failed\n"); raw_lv_release(); goto end; }
    uint8_t *in = (void *)(((uintptr_t) in_raw + 63) & ~63);
    uint8_t *o16 = (void *)(((uintptr_t) o_raw + 63) & ~63);
    memcpy(CACHEABLE(in), UNCACHEABLE(raw_info.buffer), in_size);
    dcache_clean((uint32_t) CACHEABLE(in), in_size);
    raw_lv_release();
    FILE *fi = FIO_CreateFile("ML/LOGS/CRX16IN.RAW");
    if (fi) { FIO_WriteFile(fi, CACHEABLE(in), in_size); FIO_CloseFile(fi); }

    lock = rom_CreateResLockEntry(res_ids, 2);
    unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
    my_fprintf(f, "lock %x -> %x\n", lock, r);
    if (r & 1) { if (lock) rom_DeleteResLockEntry(lock); lock = 0; goto end; }
    locked = 1;
    rom_PwrMngEnableSubChip(subchips);
    o_rd24 = MEM(PACK_UNIT_RD61 + 0x24); o_wr24 = MEM(PACK_UNIT_WR24 + 0x24);
    o_rdD0 = MEM(M2M_RD_BASE + 0xD0);    o_wrD0 = MEM(M2M_WR_BASE + 0xD0);
    my_fprintf(f, "orig pack: rd61 %x/%x wr24 %x/%x\n", o_rd24, o_rdD0, o_wr24, o_wrD0);

    /* (1) 64-line trials. Run 1 (raw_tests/crx16_1): rd {0,2,1,0} + wr {1,0,2,1,0} not exact
     * (mode 1: px0<<4, 0, ..; mode 0: 0x8040 0x420 0x220 0x2000 for 2052 2052 2048 2048).
     * The 10/12-bit configs that ARE exact (pack test #5, core edmac-d8.c) use rd {0,2,0,0},
     * wr {0,1,d,0,0}, mode 0 -> try the 16-bit flag (cfg0 = 1) on top of those. */
    static uint32_t rdA[4] = { 0, 2, 0, 0 };
    static uint32_t w1[5] = { 1, 1, 2, 0, 0 }, w2[5] = { 1, 0, 2, 0, 0 }, w3[5] = { 1, 0, 2, 1, 0 }, w4[5] = { 1, 1, 2, 1, 0 };
    struct { uint32_t *rd, *wr; int mode; } tv[] = {
        { rdA, w1, 0 }, { rdA, w2, 0 }, { rdA, w3, 0 }, { rdA, w4, 0 },
        { rdA, w1, 1 }, { rdA, w3, 1 }, { rd14, w1, 0 }, { rd14, w2, 0 },
    };
    int mode_ok = -1, v_ok = -1, shift_ok = -1;
    for (int k = 0; k < (int) COUNT(tv); k++)
    {
        memset(CACHEABLE(o16), 0xAA, W * 2 * 64 + M2M_GUARD);
        dcache_clean((uint32_t) CACHEABLE(o16), W * 2 * 64 + M2M_GUARD);
        int ms = m2m_copy_pack(in, o16, pitch, W * 2, 64, tv[k].rd, tv[k].wr, tv[k].mode);
        uint16_t *d = UNCACHEABLE(o16);
        const uint8_t *sp = UNCACHEABLE(in);
        int bad[3] = { 0, 0, 0 };                       /* d == px, px << 2, px << 4 */
        for (int q = 0; q < W * 64; q++)
        {
            uint32_t v = crx16_px(sp + (q / 8) * 14, q % 8);
            if (d[q] != v) bad[0]++;
            if (d[q] != (v << 2)) bad[1]++;
            if (d[q] != (v << 4)) bad[2]++;
        }
        int guard = 0;
        for (int q = W * 2 * 64; q < W * 2 * 64 + 256; q++) if (((volatile uint8_t *) d)[q] != 0xAA) guard++;
        my_fprintf(f, "trial %d mode %d rd %x wr %x: ms=%d bad px/<<2/<<4 = %d/%d/%d of %d past_end=%d | src %d %d %d %d dst %x %x %x %x %x %x\n",
                   k, tv[k].mode, MEM(PACK_UNIT_RD61 + 0x24), MEM(PACK_UNIT_WR24 + 0x24), ms, bad[0], bad[1], bad[2], W * 64, guard,
                   crx16_px(sp, 0), crx16_px(sp, 1), crx16_px(sp, 2), crx16_px(sp, 3), d[0], d[1], d[2], d[3], d[4], d[5]);
        char fn[32];
        snprintf(fn, sizeof(fn), "ML/LOGS/CRX16T%d.BIN", k);
        FILE *ft = FIO_CreateFile(fn);
        if (ft) { FIO_WriteFile(ft, d, W * 2 * 64); FIO_CloseFile(ft); }
        if (ms >= 0 && guard == 0 && v_ok < 0)
            for (int sh = 0; sh < 3; sh++)
                if (bad[sh] == 0) { v_ok = k; mode_ok = tv[k].mode; shift_ok = sh * 2; break; }
    }
    if (v_ok < 0) { my_fprintf(f, "no exact 16-bit EDMAC variant -> stop (encoder not called)\n"); NotifyBox(5000, "crx16: EDMAC 16-bit not exact (2)"); goto end; }
    my_fprintf(f, "using trial %d (mode %d, samples = px << %d)\n", v_ok, mode_ok, shift_ok);
    rd14[0] = tv[v_ok].rd[0]; rd14[1] = tv[v_ok].rd[1]; rd14[2] = tv[v_ok].rd[2]; rd14[3] = tv[v_ok].rd[3];
    for (int q = 0; q < 5; q++) wr16[q] = tv[v_ok].wr[q];

    /* (2) whole frame */
    uint32_t t0 = get_us_clock();
    int ms = m2m_copy_pack(in, o16, pitch, W * 2, CRXE_H, rd14, wr16, mode_ok);
    uint32_t t1 = get_us_clock();
    int first = -1, bad = 0;
    {
        uint16_t *d = UNCACHEABLE(o16); const uint8_t *sp = UNCACHEABLE(in);
        for (int q = 0; q < W * CRXE_H; q++)
            if (d[q] != (crx16_px(sp + (q / 8) * 14, q % 8) << shift_ok)) { if (first < 0) first = q; bad++; }
    }
    my_fprintf(f, "frame mode %d: ms=%d (%d us) bad=%d/%d first=%d\n", mode_ok, ms, (int)(t1 - t0), bad, W * CRXE_H, first);
    FILE *fo16 = FIO_CreateFile("ML/LOGS/CRX16.RAW");
    if (fo16) { FIO_WriteFile(fo16, UNCACHEABLE(o16), o_size); FIO_CloseFile(fo16); }
    if (ms < 0 || bad) { NotifyBox(5000, "crx16: frame copy not exact"); goto end; }

    /* put the pack units back before the encoder runs */
    MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
    MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
    rom_PwrMngDisableSubChip(subchips);
    rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock); locked = 0; lock = 0;

    /* (3) encode the 16-bit frame */
    int flags_ok = 1;
    for (int i = 0; i < 14; i++) if (((volatile uint32_t *) (am + 0x108))[i] > 1) flags_ok = 0;
    if (!flags_ok || (((volatile uint32_t *) am)[0xa8 / 4] & 2)) { my_fprintf(f, "gate failed: init flags\n"); goto end; }
    idflag = (volatile uint32_t *) (am + 0x108 + CRXE_ID * 4);
    *idflag = 0;
    uint32_t entry[8] = { CRXE_TYPE, W, CRXE_H, 0, 0, 16, (uint32_t) UNCACHEABLE(o16), W };
    crxe_done = 0; crxe_handle = 0; crxe_late = 0; crxe_used = 1;
    t0 = get_us_clock();
    r = ALGSMGR_StartEncode(entry, 1, crxe_cbr, (void *) 0x43525836);
    my_fprintf(f, "StartEncode(%d,%d,%d,0,0,16,%x,%d) = %x\n", entry[0], entry[1], entry[2], entry[6], entry[7], r);
    if (r & 1) goto end;
    keep = 1;
    int to = take_semaphore(crxe_sem, 3000);
    *idflag = 0;
    my_fprintf(f, "wait=%d done=%d handle=%x flag=%x time=%d us\n", to, crxe_done, crxe_handle, crxe_flag,
               crxe_done ? (int)(crxe_t_cbr - t0) : -1);
    if (!crxe_done) { crxe_late = 1; my_fprintf(f, "TIMEOUT (buffers kept)\n"); NotifyBox(8000, "crx16: TIMEOUT. No photos, power off."); goto end; }
    keep = 0;
    uint32_t h = crxe_handle;
    if (!h || (h & 1)) { my_fprintf(f, "encoder error %x\n", h); goto end; }
    int n = 0; uint32_t *addrs = 0, *sizes = 0;
    uint32_t g = ALGSMGR_GetRingList(h, &n, &addrs, &sizes);
    my_fprintf(f, "GetRingList=%x n=%d\n", g, n);
    int total = 0;
    if (g == 0 && n > 0 && n <= 1024 && crx_ptr_ok((uint32_t) addrs) && crx_ptr_ok((uint32_t) sizes))
    {
        FILE *fo = FIO_CreateFile("ML/LOGS/CRX16OUT.BIN");
        void *tmp_raw = fio_malloc(256 * 1024 + 64);
        void *tmp = tmp_raw ? (void *)(((uintptr_t) tmp_raw + 63) & ~63) : 0;
        for (int i = 0; i < n; i++)
        {
            uint32_t a = addrs[i], s = sizes[i];
            int good = a && s && s < 64 * 1024 * 1024 && crx_ptr_ok(a & ~3) && crx_ptr_ok((a + s + 3) & ~3);
            if (!good) continue;
            my_fprintf(f, " chunk %d: addr=%x size=%d at %d first:", i, a, s, total);
            for (int k = 0; k < 16 && k < (int) s; k++) my_fprintf(f, " %02x", ((volatile uint8_t *) UNCACHEABLE(a))[k]);
            my_fprintf(f, "\n");
            if (fo && tmp)
                for (uint32_t off = 0; off < s; off += 256 * 1024)
                {
                    uint32_t len = MIN(s - off, 256 * 1024);
                    memcpy(tmp, UNCACHEABLE(a + off), len);
                    FIO_WriteFile(fo, tmp, len);
                }
            total += s;
        }
        if (fo) FIO_CloseFile(fo);
        if (tmp_raw) fio_free(tmp_raw);
    }
    my_fprintf(f, "output total %d B = %d %% of 14-bit size\n", total, (int)((int64_t) total * 100 / in_size));
    uint32_t cg = *(volatile uint32_t *) 0x111b8;
    if (crx_ptr_ok(cg))
    {
        uint32_t blk = ((volatile uint32_t *) cg)[0x28 / 4];
        if (crx_ptr_ok(blk))
        {
            static uint32_t w[CRX_IDN];
            for (int i = 0; i < CRX_IDN; i++) w[i] = ((volatile uint32_t *) (blk + CRXE_ID * 0x288))[i];
            crx_dump_words(f, "IdBlock", blk + CRXE_ID * 0x288, w, CRX_IDN);
        }
    }
    my_fprintf(f, "FreeEncBuffer=%x\n", ALGSMGR_FreeEncBuffer(h, 0, 0));
    NotifyBox(8000, "crx16: done, %d B. ML/LOGS/CRX16.TXT", total);
end:
    if (locked)
    {
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        rom_PwrMngDisableSubChip(subchips);
        rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock);
    }
    if (idflag) my_fprintf(f, "Id %d init flag now %x\n", CRXE_ID, *idflag);
    if (in_raw && !keep) fio_free(in_raw);
    if (o_raw && !keep) fio_free(o_raw);
    FIO_CloseFile(f);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(crx16_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("crx16_task", 0x1e, 0x4000, crx16_task, 0);
}

/* CALL test before mlv_lite integration: (a) does ALGSMGR keep its output buffers after an
 * encode (am+0x394 count, alloc table am+0x154..) and do they overlap the shoot memory that
 * mlv_lite takes on REC (shoot_malloc_suite(0))? (b) sustained speed: 48 frames of
 * LV raw -> EDMAC 14->16 (proven exact, crx16_2) -> encode (Id 1, 16-bit) -> EDMAC copy of
 * the output chunks into the shoot-memory suite (like an MLV slot) -> FreeEncBuffer, while the
 * suite is held. If ALGSMGR needs Canon memory while we hold the suite, its task waits until
 * the suite is freed: our 500 ms timeout stops the burst, the input buffer is kept, the late
 * cbr frees Canon's buffer. Files: ML/LOGS/CRXMEM.TXT, CRXMEM16.RAW (last 16-bit input),
 * CRXMEMOUT.BIN (last frame as copied into the suite: chunks at 64-byte aligned offsets). */
#define CRXM_N 48
static int crxm_copy_chunk(void *src, void *dst, int size)
{
    /* EDMAC plain copy (pack regs must be Canon's defaults), rows of 4096 B + CPU tail */
    int rows = size / 4096, tail = size % 4096, ms = 0;
    if (rows) ms = m2m_copy_pack(src, dst, 4096, 4096, rows, 0, 0, 0);
    if (tail) memcpy(UNCACHEABLE((uint8_t *) dst + rows * 4096), UNCACHEABLE((uint8_t *) src + rows * 4096), tail);
    return ms;
}

static void crxmem_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    static uint32_t rdA[4] = { 0, 2, 0, 0 }, w16[5] = { 1, 1, 2, 0, 0 };
    static int t_e16[CRXM_N], t_enc[CRXM_N], t_cp[CRXM_N], t_all[CRXM_N], o_sz[CRXM_N], cp_bad[CRXM_N];
    static uint32_t amw[0xC0];
    void *e_raw = 0; int keep = 0, locked = 0; void *lock = 0;
    struct memSuite *suite = 0;
    volatile uint32_t *idflag = 0;
    uint32_t o_rd24 = 0, o_wr24 = 0, o_rdD0 = 0, o_wrD0 = 0;
    int done_n = 0;
    msleep(3000);
    FILE *f = FIO_CreateFile("ML/LOGS/CRXMEM.TXT");
    if (!f) { rawdump_running = 0; return; }
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    my_fprintf(f, "=== CRXMEM movie=%d lv=%d am=%x used=%d\n", is_movie_mode(), lv, am, crxe_used);
    if (crxe_used || is_movie_mode() || !lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f)
    { my_fprintf(f, "gate failed\n"); NotifyBox(5000, "crxmem: gate failed (photo M + LV, once per boot)"); goto end; }
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);
    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    int W = raw_info.width, pitch = raw_info.pitch;
    my_fprintf(f, "raw ok=%d buf=%x %dx%d pitch=%d\n", ok, raw_info.buffer, W, raw_info.height, pitch);
    if (!ok || raw_info.bits_per_pixel != 14 || W < 1024 || W > 4000 || (W & 15) || pitch != W * 14 / 8 || raw_info.height < CRXE_H)
    { my_fprintf(f, "gate failed: raw geometry\n"); goto end_raw; }
    int e_size = W * 2 * CRXE_H;
    e_raw = fio_malloc(e_size + 128);
    if (!e_raw) { my_fprintf(f, "fio_malloc failed\n"); goto end_raw; }
    uint8_t *e16 = (void *)(((uintptr_t) e_raw + 63) & ~63);
    my_fprintf(f, "enc input buffer %x\n", e16);

    for (int i = 0; i < 0xC0; i++) amw[i] = ((volatile uint32_t *) (am + 0x100))[i];
    crx_dump_words(f, "am+0x100 before", am + 0x100, amw, 0xC0);

    lock = rom_CreateResLockEntry(res_ids, 2);
    unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
    if (r & 1) { my_fprintf(f, "lock failed %x\n", r); if (lock) rom_DeleteResLockEntry(lock); lock = 0; goto end_raw; }
    locked = 1;
    rom_PwrMngEnableSubChip(subchips);
    o_rd24 = MEM(PACK_UNIT_RD61 + 0x24); o_wr24 = MEM(PACK_UNIT_WR24 + 0x24);
    o_rdD0 = MEM(M2M_RD_BASE + 0xD0);    o_wrD0 = MEM(M2M_WR_BASE + 0xD0);

    idflag = (volatile uint32_t *) (am + 0x108 + CRXE_ID * 4);
    for (int i = 0; i < 14; i++) if (((volatile uint32_t *) (am + 0x108))[i] > 1) { my_fprintf(f, "gate: init flags\n"); goto end_raw; }
    if (((volatile uint32_t *) am)[0xa8 / 4] & 2) { my_fprintf(f, "gate: am+0xa8\n"); goto end_raw; }
    *idflag = 0;
    crxe_used = 1;

    /* frame loop; frame 0 runs before the shoot memory is taken */
    for (int k = 0; k < CRXM_N; k++)
    {
        if (k == 1)
        {
            for (int i = 0; i < 0xC0; i++) amw[i] = ((volatile uint32_t *) (am + 0x100))[i];
            crx_dump_words(f, "am+0x100 after encode 0", am + 0x100, amw, 0xC0);
            suite = shoot_malloc_suite(0);
            my_fprintf(f, "shoot_malloc_suite(0) = %x size %d\n", suite, suite ? suite->size : 0);
            if (!suite) break;
            int nc = 0;
            for (struct memChunk *c = GetFirstChunkFromSuite(suite); c && nc < 40; c = GetNextMemoryChunk(suite, c), nc++)
                my_fprintf(f, "  shoot chunk %d: %x size %d\n", nc, GetMemoryAddressOfMemoryChunk(c), GetSizeOfMemoryChunk(c));
            for (int i = 0; i < 0xC0; i++) amw[i] = ((volatile uint32_t *) (am + 0x100))[i];
            crx_dump_words(f, "am+0x100 with shoot memory held", am + 0x100, amw, 0xC0);
        }
        uint32_t t0 = get_us_clock();
        /* LV raw -> 16-bit samples (pixel << 2) */
        int ms16 = m2m_copy_pack(raw_info.buffer, e16, pitch, W * 2, CRXE_H, rdA, w16, 0);
        uint32_t t1 = get_us_clock();
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        if (ms16 < 0) { my_fprintf(f, "frame %d: EDMAC 16 timeout\n", k); break; }
        uint32_t entry[8] = { CRXE_TYPE, W, CRXE_H, 0, 0, 16, (uint32_t) UNCACHEABLE(e16), W };
        crxe_done = 0; crxe_handle = 0; crxe_late = 0;
        while (take_semaphore(crxe_sem, 1) == 0) ;   /* drop a stale give */
        r = ALGSMGR_StartEncode(entry, 1, crxe_cbr, (void *) 0x43524d4d);
        if (r & 1) { my_fprintf(f, "frame %d: StartEncode %x\n", k, r); break; }
        int to = take_semaphore(crxe_sem, 500);
        uint32_t t2 = get_us_clock();
        if (!crxe_done) { crxe_late = 1; keep = 1; my_fprintf(f, "frame %d: TIMEOUT wait=%d (ALGSMGR blocked? input kept)\n", k, to); break; }
        uint32_t h = crxe_handle;
        if (!h || (h & 1)) { my_fprintf(f, "frame %d: encoder error %x\n", k, h); break; }
        int n = 0; uint32_t *addrs = 0, *sizes = 0;
        uint32_t g = ALGSMGR_GetRingList(h, &n, &addrs, &sizes);
        int total = 0, bad = 0;
        uint8_t *dst = 0;
        if (suite && GetSizeOfMemoryChunk(GetFirstChunkFromSuite(suite)) >= 4 * 1024 * 1024)
            dst = GetMemoryAddressOfMemoryChunk(GetFirstChunkFromSuite(suite));
        if (g == 0 && n > 0 && n <= 64 && crx_ptr_ok((uint32_t) addrs) && crx_ptr_ok((uint32_t) sizes))
        {
            for (int i = 0; i < n; i++)
            {
                uint32_t a = addrs[i], s = sizes[i];
                if (!a || !s || s > 16 * 1024 * 1024 || !crx_ptr_ok(a & ~3)) continue;
                if (k == 0) my_fprintf(f, "  frame 0 chunk %d: %x size %d\n", i, a, s);
                if (dst)
                {
                    crxm_copy_chunk((void *) a, dst + total, s);
                    volatile uint8_t *ps = UNCACHEABLE(a), *pd = UNCACHEABLE(dst + total);
                    for (int q = 0; q < 16 && q < (int) s; q++) { if (ps[q] != pd[q]) bad++; if (ps[s - 1 - q] != pd[s - 1 - q]) bad++; }
                }
                total += (s + 63) & ~63;
            }
        }
        uint32_t t3 = get_us_clock();
        if (k == CRXM_N - 1 && dst)
        {
            FILE *fo = FIO_CreateFile("ML/LOGS/CRXMEMOUT.BIN");
            if (fo) { FIO_WriteFile(fo, UNCACHEABLE(dst), total); FIO_CloseFile(fo); }
            FILE *fi = FIO_CreateFile("ML/LOGS/CRXMEM16.RAW");
            if (fi) { FIO_WriteFile(fi, UNCACHEABLE(e16), e_size); FIO_CloseFile(fi); }
            for (int i = 0; i < n; i++) my_fprintf(f, "  last frame chunk %d size %d\n", i, sizes[i]);
        }
        ALGSMGR_FreeEncBuffer(h, 0, 0);
        t_e16[k] = t1 - t0; t_enc[k] = t2 - t1; t_cp[k] = t3 - t2; t_all[k] = get_us_clock() - t0; o_sz[k] = total; cp_bad[k] = bad;
        done_n = k + 1;
    }
    for (int k = 0; k < done_n; k++)
        my_fprintf(f, "frame %2d: edmac16 %5d us, encode %5d us, copy %5d us, total %5d us, out %d B, copy_bad %d\n",
                   k, t_e16[k], t_enc[k], t_cp[k], t_all[k], o_sz[k], cp_bad[k]);
    my_fprintf(f, "frames done %d of %d\n", done_n, CRXM_N);
end_raw:
    if (suite) { shoot_free_suite(suite); suite = 0; my_fprintf(f, "shoot memory freed\n"); }
    if (am && crx_ptr_ok(am))
    {
        for (int i = 0; i < 0xC0; i++) amw[i] = ((volatile uint32_t *) (am + 0x100))[i];
        crx_dump_words(f, "am+0x100 end", am + 0x100, amw, 0xC0);
    }
    raw_lv_release();
end:
    if (locked)
    {
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        rom_PwrMngDisableSubChip(subchips);
        rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock);
    }
    if (idflag) { *idflag = 0; my_fprintf(f, "Id %d init flag reset\n", CRXE_ID); }
    if (e_raw && !keep) fio_free(e_raw);
    NotifyBox(8000, "crxmem: %d/%d frames. ML/LOGS/CRXMEM.TXT", done_n, CRXM_N);
    FIO_CloseFile(f);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(crxmem_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("crxmem_task", 0x1e, 0x4000, crxmem_task, 0);
}

/* CALL test: can the encoder drop the cost of the EDMAC << 2? (M05-1148: 66 %, card margin 2-5 %)
 * ROM sign (FUN_e02aa8e6, CRX plane header): byte 8 = plane << 4 | (block[0x260+p*4] & 1) << 3 |
 * (block[0x214+p*4] & 3) << 1 = LibRaw's compHdrRoundedBits; FUN_e02aae50 (EncSet, every frame)
 * copies the same block[0x214+p*4] into the encoder register (FUN_e031cc90: bits 16..23, 2 per
 * plane). block = *(*(0x111b8)+0x28) + Id*0x288, filled by algsmgrEncInit with 0 for all planes.
 * LibRaw: mask = 1 << (field-1), value = mask*2*code + (code>>31) + prediction.
 * Field 1: step 2, error <= 1 -> pixel = (v+1) >> 2 is exact for data = pixel << 2.
 * Field 2: step 4, error up to 2 -> ties possible, must be measured.
 * Encodes the SAME 16-bit frame with field 0, 1, 2 (RAM poke between encodes, restored to 0, Id
 * flag cleared so EncInit rewrites the block). Files: ML/LOGS/CRXR.TXT, CRXR16.RAW (input),
 * CRXR0.BIN, CRXR1.BIN, CRXR2.BIN (encoder output, no padding between planes). */
static void crxr_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    static uint32_t rdA[4] = { 0, 2, 0, 0 }, w16[5] = { 1, 1, 2, 0, 0 };
    void *e_raw = 0, *lock = 0; int keep = 0;
    volatile uint32_t *idflag = 0, *rbits = 0;
    msleep(3000);
    FILE *f = FIO_CreateFile("ML/LOGS/CRXR.TXT");
    if (!f) { rawdump_running = 0; return; }
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    my_fprintf(f, "=== CRXR movie=%d lv=%d am=%x used=%d iso=%d\n", is_movie_mode(), lv, am, crxe_used, lens_info.iso);
    if (crxe_used || is_movie_mode() || !lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f)
    { my_fprintf(f, "gate failed\n"); NotifyBox(5000, "crxr: gate failed (photo M + LV, once per boot)"); goto end; }
    for (int i = 0; i < 14; i++) if (((volatile uint32_t *) (am + 0x108))[i] > 1) { my_fprintf(f, "gate: init flags\n"); goto end; }
    if (((volatile uint32_t *) am)[0xa8 / 4] & 2) { my_fprintf(f, "gate: am+0xa8\n"); goto end; }
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);
    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    int W = raw_info.width, pitch = raw_info.pitch;
    my_fprintf(f, "raw ok=%d buf=%x %dx%d pitch=%d\n", ok, raw_info.buffer, W, raw_info.height, pitch);
    if (!ok || raw_info.bits_per_pixel != 14 || W < 1024 || W > 4000 || (W & 15) || pitch != W * 14 / 8 || raw_info.height < CRXE_H)
    { my_fprintf(f, "gate failed: raw geometry\n"); raw_lv_release(); goto end; }
    int e_size = W * 2 * CRXE_H;
    e_raw = fio_malloc(e_size + 128);
    if (!e_raw) { my_fprintf(f, "fio_malloc failed\n"); raw_lv_release(); goto end; }
    uint8_t *e16 = (void *)(((uintptr_t) e_raw + 63) & ~63);

    /* one 16-bit frame (pixel << 2), proven exact in crx16_2 */
    lock = rom_CreateResLockEntry(res_ids, 2);
    unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
    if (r & 1) { my_fprintf(f, "lock failed %x\n", r); if (lock) rom_DeleteResLockEntry(lock); raw_lv_release(); goto end; }
    rom_PwrMngEnableSubChip(subchips);
    uint32_t o_rd24 = MEM(PACK_UNIT_RD61 + 0x24), o_wr24 = MEM(PACK_UNIT_WR24 + 0x24);
    uint32_t o_rdD0 = MEM(M2M_RD_BASE + 0xD0),    o_wrD0 = MEM(M2M_WR_BASE + 0xD0);
    int ms = m2m_copy_pack(raw_info.buffer, e16, pitch, W * 2, CRXE_H, rdA, w16, 0);
    MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
    MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
    rom_PwrMngDisableSubChip(subchips);
    rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock);
    raw_lv_release();
    my_fprintf(f, "EDMAC 16-bit frame ms=%d\n", ms);
    if (ms < 0) goto end;
    FILE *fi = FIO_CreateFile("ML/LOGS/CRXR16.RAW");
    if (fi) { FIO_WriteFile(fi, UNCACHEABLE(e16), e_size); FIO_CloseFile(fi); }

    idflag = (volatile uint32_t *) (am + 0x108 + CRXE_ID * 4);
    *idflag = 0;
    crxe_used = 1;
    for (int field = 0; field <= 3; field++)   /* 3 added 2026-11-07: step 8 on pixel<<2 = about 13-bit */
    {
        if (field)
        {
            /* Id block must hold our size (written by EncInit in the field 0 encode) */
            uint32_t cg = *(volatile uint32_t *) 0x111b8;
            uint32_t blk = crx_ptr_ok(cg) ? ((volatile uint32_t *) cg)[0x28 / 4] : 0;
            uint32_t idb = crx_ptr_ok(blk) ? blk + CRXE_ID * 0x288 : 0;
            if (!idb || ((volatile uint16_t *) (idb + 0x200))[0] != W || ((volatile uint16_t *) (idb + 0x200))[1] != CRXE_H)
            { my_fprintf(f, "gate: Id block %x not ours, stop\n", idb); break; }
            rbits = (volatile uint32_t *) (idb + 0x214);
            my_fprintf(f, "field %d: block %x +0x200=%x, +0x214 before %x %x %x %x\n", field, idb,
                       ((volatile uint32_t *) (idb + 0x200))[0], rbits[0], rbits[1], rbits[2], rbits[3]);
            if (rbits[0] > 3 || rbits[1] > 3 || rbits[2] > 3 || rbits[3] > 3) { my_fprintf(f, "gate: +0x214 not small, stop\n"); rbits = 0; break; }
            for (int p = 0; p < 4; p++) rbits[p] = field;
        }
        uint32_t entry[8] = { CRXE_TYPE, W, CRXE_H, 0, 0, 16, (uint32_t) UNCACHEABLE(e16), W };
        crxe_done = 0; crxe_handle = 0; crxe_late = 0;
        while (take_semaphore_now(crxe_sem) == 0) ;
        uint32_t t0 = get_us_clock();
        r = ALGSMGR_StartEncode(entry, 1, crxe_cbr, (void *) 0x43525852);
        if (r & 1) { my_fprintf(f, "field %d: StartEncode %x\n", field, r); break; }
        take_semaphore(crxe_sem, 1000);
        if (!crxe_done) { crxe_late = 1; keep = 1; my_fprintf(f, "field %d: TIMEOUT (input kept)\n", field); NotifyBox(8000, "crxr: TIMEOUT. No photos, power off."); break; }
        uint32_t h = crxe_handle;
        my_fprintf(f, "field %d: handle=%x flag=%x time=%d us\n", field, h, crxe_flag, (int)(crxe_t_cbr - t0));
        if (!h || (h & 1)) { my_fprintf(f, "encoder error %x\n", h); break; }
        int n = 0; uint32_t *addrs = 0, *sizes = 0;
        uint32_t g = ALGSMGR_GetRingList(h, &n, &addrs, &sizes);
        int total = 0;
        if (g == 0 && n > 0 && n <= 64 && crx_ptr_ok((uint32_t) addrs) && crx_ptr_ok((uint32_t) sizes))
        {
            char fn[32];
            snprintf(fn, sizeof(fn), "ML/LOGS/CRXR%d.BIN", field);
            FILE *fo = FIO_CreateFile(fn);
            void *tmp_raw = fio_malloc(256 * 1024 + 64);
            void *tmp = tmp_raw ? (void *)(((uintptr_t) tmp_raw + 63) & ~63) : 0;
            for (int i = 0; i < n; i++)
            {
                uint32_t a = addrs[i], s = sizes[i];
                if (!a || !s || s > 16 * 1024 * 1024 || !crx_ptr_ok(a & ~3)) continue;
                my_fprintf(f, "  chunk %d: %x size %d\n", i, a, s);
                if (fo && tmp)
                    for (uint32_t off = 0; off < s; off += 256 * 1024)
                    {
                        uint32_t len = MIN(s - off, 256 * 1024);
                        memcpy(tmp, UNCACHEABLE(a + off), len);
                        FIO_WriteFile(fo, tmp, len);
                    }
                total += s;
            }
            if (fo) FIO_CloseFile(fo);
            if (tmp_raw) fio_free(tmp_raw);
        }
        my_fprintf(f, "field %d: output %d B = %d %% of 14-bit size\n", field, total, (int)((int64_t) total * 100 / (pitch * CRXE_H)));
        ALGSMGR_FreeEncBuffer(h, 0, 0);
    }
    if (rbits) { for (int p = 0; p < 4; p++) rbits[p] = 0; my_fprintf(f, "rounded fields restored to 0\n"); }
    NotifyBox(8000, "crxr: done. ML/LOGS/CRXR.TXT");
end:
    if (idflag) { *idflag = 0; my_fprintf(f, "Id %d init flag reset\n", CRXE_ID); }
    if (e_raw && !keep) fio_free(e_raw);
    FIO_CloseFile(f);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(crxr_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("crxr_task", 0x1e, 0x4000, crxr_task, 0);
}

/* READ-ONLY: Canon's VD table. Chain found in ROM: FrameData -> SetTgParam -> VD struct
 * 0xa3594 (VD, AC at +0xe) -> ImageControllerLvState (FUN_e082c1c2: m_VSize = VD, then
 * LV_RoutineB sends the sensor list) -> CMOS 0x50 / 0x29. VD is recomputed every frame by
 * FUN_e00da408: VD = u16 table[*(0x123c8+0x5c) + mode*0x1a + rate*2] (x factor byte
 * *(0x123c8), at least AC+2), mode = *(0x123c8+0xc), rate = u16 *(0x123c8+2).
 * FPSRAM2: rate 4 @ movie 24p, 2 @ 30p, 0 @ 60p / photo; mode 5 movie, 1 photo. */
static void vdtab_task()
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/VDTAB.TXT");
    if (f)
    {
        volatile uint8_t *seq = (volatile uint8_t *) 0x123c8;
        uint32_t tab = *(volatile uint32_t *) (0x123c8 + 0x5c);
        my_fprintf(f, "=== movie=%d fps_x1000=%d timerB=%x factor=%d rate=%d mode=%d tab=%x VD=%x AC=%d\n",
                   is_movie_mode(), lv_vsync_fps_x1000(), *(volatile uint32_t *) 0xD04061A4,
                   seq[0], *(volatile uint16_t *) (0x123c8 + 2), *(volatile uint32_t *) (0x123c8 + 0xc),
                   tab, ((volatile uint16_t *) 0xa3594)[0], ((volatile uint16_t *) 0xa3594)[7]);
        my_fprintf(f, "  seq:");
        for (int i = 0; i < 40; i++) my_fprintf(f, " %08x", ((volatile uint32_t *) 0x123c8)[i]);
        my_fprintf(f, "\n");
        if (tab > 0x1000 && tab < 0x60000000 && !(tab & 1))
        {
            for (int m = 0; m < 12; m++)
            {
                my_fprintf(f, "  mode %2d:", m);
                for (int r = 0; r < 13; r++)
                    my_fprintf(f, " %4x", ((volatile uint16_t *) (tab + m * 0x1a))[r]);
                my_fprintf(f, "\n");
            }
        }
        FIO_CloseFile(f);
    }
    NotifyBox(5000, "vdtab: done. ML/LOGS/VDTAB.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(vdtab_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("vdtab_task", 0x1e, 0x2000, vdtab_task, 0);
}

/* WRITE test: Canon's own frame length for photo LV. VDTAB.TXT (raw_tests/vdtab1): photo
 * row (mode 1) = 26f, -, 4df, 5d7 (60/50/30/25p) and no 24p; movie row (mode 5) has
 * 24p = 0x617 at column 4 (= our timer B value, sign 2). Sign 1: FUN_e00da408 reads this RAM
 * table each frame (VD struct 0xa3594 -> m_VSize -> LV_RoutineB -> CMOS 0x50 / 0x29).
 * Patch ONLY the photo 60p entry 0x26f -> 0x617 (rate index unchanged, so every other
 * per-rate table stays valid), measure, restore. mlv_lite FPS should be OFF: does Canon
 * reach 24 fps and program the sensor by itself? */
static void vdpatch_task()
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/VDPATCH.TXT");
    if (!f) { rawdump_running = 0; return; }
    uint32_t tab = *(volatile uint32_t *) (0x123c8 + 0x5c);
    uint32_t mode = *(volatile uint32_t *) (0x123c8 + 0xc);
    uint32_t rate = *(volatile uint16_t *) (0x123c8 + 2);
    volatile uint16_t *e = (volatile uint16_t *) (tab + 1 * 0x1a + 0 * 2);
    uint16_t orig = (tab > 0x1000 && tab < 0x60000000) ? *e : 0;
    my_fprintf(f, "=== movie=%d lv=%d mode=%d rate=%d tab=%x entry=%x fps=%d timerB=%x\n",
               is_movie_mode(), lv, mode, rate, tab, orig, lv_vsync_fps_x1000(),
               *(volatile uint32_t *) 0xD04061A4);
    if (!lv || is_movie_mode() || mode != 1 || rate != 0 || orig != 0x26f)
    {
        my_fprintf(f, "abort: need photo LV (mode 1, rate 0, entry 26f)\n");
        FIO_CloseFile(f);
        NotifyBox(5000, "vdpatch: abort (photo mode M, FPS OFF)");
        rawdump_running = 0;
        return;
    }
    raw_lv_request();
    msleep(500);
    raw_update_params();

    for (int p = 0; p < 6; p++)
    {
        if (p == 1) *e = 0x617;
        if (p == 5) *e = orig;
        msleep(p == 0 ? 500 : 1500);
        uint32_t P = *(volatile uint32_t *) (0xeedc + 0x18);
        volatile uint32_t *l = (volatile uint32_t *) (P + 4 * 0x6c + 0x2f14);
        uint32_t r50 = 0, r29 = 0;
        if (P > 0x1000 && P < 0x60000000)
            for (int i = 0; i < 27 && l[i] != 0xffffffff; i++)
            {
                if ((l[i] >> 22) == 0x50) r50 = l[i] & 0x3fffff;
                if ((l[i] >> 22) == 0x29) r29 = l[i] & 0x3fffff;
            }
        int top = 0, bottom = 0;
        for (int k = 0; k < 3; k++)
        {
            top += tgw_meter(100, 400);
            bottom += tgw_meter(raw_info.active_area.y2 - raw_info.active_area.y1 - 90,
                                raw_info.active_area.y2 - raw_info.active_area.y1 - 10);
            msleep(60);
        }
        my_fprintf(f, "phase %d entry=%x fps=%d timerB=%x VD=%x AC=%d m_VSize=%x m_Shutter=%x cmos50=%x cmos29=%x top=%d bottom=%d\n",
                   p, *e, lv_vsync_fps_x1000(), *(volatile uint32_t *) 0xD04061A4,
                   ((volatile uint16_t *) 0xa3594)[0], ((volatile uint16_t *) 0xa3594)[7],
                   *(volatile uint32_t *) 0xef34, *(volatile uint32_t *) 0xef44, r50, r29,
                   top / 3, bottom / 3);
    }
    raw_lv_release();
    FIO_CloseFile(f);
    NotifyBox(5000, "vdpatch: done. ML/LOGS/VDPATCH.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(vdpatch_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("vdpatch_task", 0x1e, 0x2000, vdpatch_task, 0);
}

static MENU_SELECT_FUNC(tgsnap_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("tgsnap_task", 0x1e, 0x2000, tgsnap_task, 0);
}

static void fps24_log(FILE *f, const char *what)
{
    my_fprintf(f, "%s: A=%x B=%x confirm=%x vsync_fps_x1000=%d setparam=%d vsync=%d"
               " | VSize=%x max=%x VD=%x max=%x AC=%d\n", what,
               *(volatile uint32_t *) M50_FPS_TIMER_A, *(volatile uint32_t *) M50_FPS_TIMER_B,
               *(volatile uint32_t *) M50_FPS_CONFIRM, lv_vsync_fps_x1000(),
               fps24_setparam_count, fps24_vsync_count,
               *(volatile uint32_t *) M50_RAM_VSIZE, *(volatile uint32_t *) M50_RAM_VSIZE_MAX,
               *(volatile uint32_t *) M50_RAM_VD, *(volatile uint32_t *) M50_RAM_VD_MAX,
               ((volatile uint16_t *) M50_RAM_VD)[7]);
    my_fprintf(f, "    esync rate=%x grp=%x\n", *(volatile uint32_t *) M50_RAM_ESYNC_RATE,
               *(volatile uint32_t *) (0x1770c + 0x20));
}

static void fps24_task(int with_vd)
{
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/FPS24.TXT");
    if (!f) { rawdump_running = 0; return; }

    uint32_t a = *(volatile uint32_t *) M50_FPS_TIMER_A;
    uint32_t b = *(volatile uint32_t *) M50_FPS_TIMER_B;
    my_fprintf(f, "=== movie=%d lv=%d\n", is_movie_mode(), lv);
    fps24_log(f, "before");

    /* only the exact state measured in photo mode LV (mlv_lite FPS override OFF);
     * Canon flickers B and VD by 1 (26e/26f, 26f/270), seen in FPSREG2 / FPSRAM */
    uint32_t vsize = *(volatile uint32_t *) M50_RAM_VSIZE;
    uint32_t vd = *(volatile uint32_t *) M50_RAM_VD;
    if (!lv || is_movie_mode() || a != 0x2d1 || (b != 0x26e && b != 0x26f) ||
        (vsize != 0x26f && vsize != 0x270) || (vd != 0x26f && vd != 0x270))
    {
        my_fprintf(f, "abort: not photo LV (A=%x B=%x VSize=%x VD=%x)\n", a, b, vsize, vd);
        FIO_CloseFile(f);
        NotifyBox(5000, "fps24: abort (photo mode M, LiveView)");
        rawdump_running = 0;
        return;
    }

    fps24_ram_orig[0] = *(volatile uint32_t *) M50_RAM_VSIZE;
    fps24_ram_orig[1] = *(volatile uint32_t *) M50_RAM_VSIZE_MAX;
    fps24_ram_orig[2] = *(volatile uint32_t *) M50_RAM_VD;
    fps24_ram_orig[3] = *(volatile uint32_t *) M50_RAM_VD_MAX;
    fps24_ram_orig[4] = *(volatile uint32_t *) M50_RAM_ESYNC_RATE;
    if (with_vd == 2 && fps24_ram_orig[4] != 0)
    {
        my_fprintf(f, "abort: ESync rate %x, expected 0 in photo LV\n", fps24_ram_orig[4]);
        FIO_CloseFile(f);
        NotifyBox(5000, "fps24: abort (ESync rate not 0)");
        rawdump_running = 0;
        return;
    }

    my_fprintf(f, with_vd == 2 ? "run 4: timer B + VSize/VD + ESync rate 2 every frame\n" :
                  with_vd ? "run 3: timer B + Canon VSize/VD every frame\n" : "run 2: rewrite every frame\n");
    fps24_setparam_count = fps24_vsync_count = 0;
    fps24_vd = with_vd;
    fps24_write();
    fps24_active = 1;
    for (int i = 0; i < (with_vd ? 10 : 6); i++)
    {
        msleep(500);
        fps24_log(f, "24p");
    }

    fps24_active = 0;
    fps24_vd = 0;
    msleep(100);
    *(volatile uint32_t *) M50_FPS_TIMER_B = b;
    *(volatile uint32_t *) M50_FPS_CONFIRM = 1;
    if (with_vd)
    {
        *(volatile uint32_t *) M50_RAM_VSIZE = fps24_ram_orig[0];
        *(volatile uint32_t *) M50_RAM_VSIZE_MAX = fps24_ram_orig[1];
        *(volatile uint32_t *) M50_RAM_VD = fps24_ram_orig[2];
        *(volatile uint32_t *) M50_RAM_VD_MAX = fps24_ram_orig[3];
    }
    if (with_vd == 2)
    {
        *(volatile uint32_t *) M50_RAM_ESYNC_RATE = fps24_ram_orig[4];
    }
    msleep(1500);
    fps24_log(f, "restored");
    FIO_CloseFile(f);

    NotifyBox(5000, "fps24: done (%d.%03d fps). ML/LOGS/FPS24.TXT",
              lv_vsync_fps_x1000() / 1000, lv_vsync_fps_x1000() % 1000);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(fps24_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("fps24_task", 0x1e, 0x2000, fps24_task, 0);
}

static MENU_SELECT_FUNC(fps24fr_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("fps24fr_task", 0x1e, 0x2000, fps24_task, (void *) 2);
}

static MENU_SELECT_FUNC(fps24vd_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("fps24vd_task", 0x1e, 0x2000, fps24_task, (void *) 1);
}

/* READ-ONLY: dumps Canon's Evf state object (EVF_STATE pointer at 0x6cec, used by
 * Conductor/EvfCap/EvfState.c in ROM) and samples current_state, so the vsync
 * hook can be configured from facts. Nothing is written or hooked. */
#define M50_EVF_STATE_PTR 0x6cec

static void evfinfo_task()
{
    char line[200];
    msleep(3000);
    FILE *f = FIO_CreateFile("ML/LOGS/EVFSTATE.TXT");
    if (!f) { rawdump_running = 0; return; }

    uint32_t so = MEM(M50_EVF_STATE_PTR);
    my_fprintf(f, "EVF_STATE ptr @%x = %x\n", M50_EVF_STATE_PTR, so);
    if (so && so < 0x40000000)
    {
        const char *type = (const char *) MEM(so + 0x00);
        const char *name = (const char *) MEM(so + 0x04);
        my_fprintf(f, "type ptr %x \"%s\"\n", type, ((uint32_t)type >> 28) == 0xE ? type : "?");
        my_fprintf(f, "name ptr %x \"%s\"\n", name, ((uint32_t)name >> 28) == 0xE ? name : "?");
        my_fprintf(f, "auto_seq %x  StateTransition %x  matrix %x  max_inputs %d  max_states %d  current %d\n",
                   MEM(so + 0x08), MEM(so + 0x0C), MEM(so + 0x10), MEM(so + 0x14), MEM(so + 0x18), MEM(so + 0x1C));

        /* how often each state is seen over 2 s (1 ms sampling) */
        static int seen[16];
        int changes = 0, last = MEM(so + 0x1C);
        for (int i = 0; i < 2000; i++)
        {
            int st = MEM(so + 0x1C);
            if (st >= 0 && st < 16) seen[st]++;
            if (st != last) { changes++; last = st; }
            msleep(1);
        }
        snprintf(line, sizeof(line), "2 s: changes=%d  seen:", changes);
        my_fprintf(f, "%s", line);
        for (int i = 0; i < 16; i++) if (seen[i]) my_fprintf(f, " s%d=%d", i, seen[i]);
        my_fprintf(f, "\n");
    }
    FIO_CloseFile(f);
    NotifyBox(5000, "evfinfo: done. ML/LOGS/EVFSTATE.TXT");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(evfinfo_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("evfinfo_task", 0x1e, 0x2000, evfinfo_task, 0);
}

static MENU_SELECT_FUNC(rawburst_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("rawburst_task", 0x1f, 0x4000, rawburst_task, 0);   /* lower priority than LV/GUI */
}

static MENU_SELECT_FUNC(rawdump_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("rawdump_task", 0x1e, 0x2000, rawdump_task, 0);
}

/* Card write test (2026-11-07, raw_tests/cardtest_pc1 showed no slow phases on the PC):
 * is the camera's own SD write path sometimes slow, without any recording? 4 files x 2 GiB,
 * 8 MiB per FIO_WriteFile, paced to 40 MiB/s like mlv_lite lossless, every write timed.
 * Photo LV idle. Files CWT0.BIN..CWT3.BIN in the card root are rewritten on every run.
 * Log: ML/LOGS/CWTEST.TXT (written only at the end, so the log does not disturb the test). */
#define CWT_BLOCK    (8 * 1024 * 1024)
#define CWT_PER_FILE 256
#define CWT_FILES    4
#define CWT_TOTAL    (CWT_FILES * CWT_PER_FILE)
static uint16_t cwt_ms[CWT_TOTAL];
static int cwt_t[CWT_TOTAL];

static void cwt_task()
{
    msleep(3000);
    int lv0 = lv, movie0 = is_movie_mode();
    void *buf = fio_malloc(CWT_BLOCK + 64);
    int done = 0, failed_at = -1, failed_ret = 0;
    int create_ms[CWT_FILES] = {0}, close_ms[CWT_FILES] = {0};
    if (buf)
    {
        for (int i = 0; i < CWT_BLOCK / 4; i++) ((uint32_t *) buf)[i] = i * 2654435761u;   /* not compressible */
        int t0 = get_ms_clock();
        int k = 0;
        for (int fi = 0; fi < CWT_FILES && failed_at < 0; fi++)
        {
            char name[16];
            snprintf(name, sizeof(name), "CWT%d.BIN", fi);
            int tc = get_ms_clock();
            FILE *f = FIO_CreateFile(name);
            create_ms[fi] = get_ms_clock() - tc;
            if (!f) { failed_at = k; failed_ret = -1; break; }
            for (int i = 0; i < CWT_PER_FILE; i++, k++)
            {
                int due = t0 + k * 200;            /* 8 MiB every 200 ms = 40 MiB/s */
                int now = get_ms_clock();
                if (now < due) msleep(due - now);
                int a = get_ms_clock();
                int w = FIO_WriteFile(f, buf, CWT_BLOCK);
                int ms = get_ms_clock() - a;
                cwt_t[k] = a - t0;
                cwt_ms[k] = ms > 65535 ? 65535 : ms;
                if (w != CWT_BLOCK) { failed_at = k; failed_ret = w; k++; break; }
                if (k % 20 == 0)
                    NotifyBox(2000, "card write test: %d of %d MB, last %d ms", (k + 1) * 8, CWT_TOTAL * 8, ms);
            }
            tc = get_ms_clock();
            FIO_CloseFile(f);
            close_ms[fi] = get_ms_clock() - tc;
            done = k;
        }
    }

    FILE *log = FIO_CreateFile("ML/LOGS/CWTEST.TXT");
    if (log)
    {
        my_fprintf(log, "=== CWTEST lv=%d movie=%d buf=%x writes=%d failed_at=%d ret=%d\n", lv0, movie0, buf, done, failed_at, failed_ret);
        for (int fi = 0; fi < CWT_FILES; fi++) my_fprintf(log, "file %d: create %d ms, close %d ms\n", fi, create_ms[fi], close_ms[fi]);
        int over300 = 0, over500 = 0, over1000 = 0, phases = 0, maxms = 0, run = 0, total_ms = 0;
        int hist[8] = {0};   /* <100, <150, <200, <300, <500, <1000, <2000, more */
        for (int i = 0; i < done; i++)
        {
            int m = cwt_ms[i];
            total_ms += m;
            if (m > maxms) maxms = m;
            over300 += m > 300; over500 += m > 500; over1000 += m > 1000;
            hist[m < 100 ? 0 : m < 150 ? 1 : m < 200 ? 2 : m < 300 ? 3 : m < 500 ? 4 : m < 1000 ? 5 : m < 2000 ? 6 : 7]++;
            if (m > 300) { run++; if (run == 3) phases++; } else run = 0;
        }
        my_fprintf(log, "writes over 300 ms %d, over 500 ms %d, over 1000 ms %d, max %d ms, slow phases (3+ over 300 ms in a row) %d\n",
                   over300, over500, over1000, maxms, phases);
        my_fprintf(log, "pure write speed %d KiB/s\n", total_ms ? (int)((long long) done * 8 * 1024 * 1000 / total_ms) : 0);
        my_fprintf(log, "hist <100 %d, <150 %d, <200 %d, <300 %d, <500 %d, <1000 %d, <2000 %d, more %d\n",
                   hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
        for (int i = 0; i < done; i++) my_fprintf(log, "%d %d %d\n", i, cwt_t[i], cwt_ms[i]);
        FIO_CloseFile(log);
    }
    if (buf) free(buf);
    NotifyBox(10000, buf ? "card write test: done, %d writes. ML/LOGS/CWTEST.TXT" : "card write test: no memory (%d)", done);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(cwt_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("cwt_task", 0x1e, 0x2000, cwt_task, 0);
}

/* Card write + recording load (2026-11-07). cardtest_cam1: the card writes 55 MB/s in photo LV
 * idle with no slow phases, but during lossless recording only 42-44 MB/s with multi-second
 * phases at about 15 MB/s. Same 40 MiB/s write pattern as cwt_task, 2 GiB per phase, while a
 * second task (priority 0x0F like mlv_lite's compress_task) adds the recording work every 42 ms:
 *   phase 0: nothing (baseline)
 *   phase 1: EDMAC resource lock + sub-chips 4/7 on (as lossless_rec_start), no copies
 *   phase 2: + LV raw -> 16-bit EDMAC copy (crx16_2 config, CRXE_H lines)
 *   phase 3: + CRX encode (Id 1, 16-bit) + FreeEncBuffer
 *   phase 4: + EDMAC copy of the output chunks into a buffer (like the MLV slot copy)
 * Nothing is recorded. Files CWL0..CWL4.BIN in the card root are rewritten on every run.
 * Log: ML/LOGS/CWLOAD.TXT at the end. Photo M, LV, not recording. */
#define CWL_PHASES 5
#define CWL_TOTAL  (CWL_PHASES * CWT_PER_FILE)
static uint16_t cwl_ms[CWL_TOTAL];
static int cwl_t[CWL_TOTAL];
static volatile int cwl_phase = -2;          /* 0..4 running, -1 = stop */
static volatile int cwl_load_running = 0, cwl_load_err = 0, cwl_load_err_phase = -1;
static volatile int cwl_frames[CWL_PHASES], cwl_us[CWL_PHASES], cwl_us_max[CWL_PHASES];
static uint8_t *cwl_e16 = 0, *cwl_dst = 0;
#define CWL_DST_SIZE (4 * 1024 * 1024)

static void cwl_load_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    static uint32_t rdA[4] = { 0, 2, 0, 0 }, w16[5] = { 1, 1, 2, 0, 0 };
    void *lock = 0; int locked = 0;
    uint32_t o_rd24 = 0, o_wr24 = 0, o_rdD0 = 0, o_wrD0 = 0;
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    volatile uint32_t *idflag = crx_ptr_ok(am) ? (volatile uint32_t *) (am + 0x108 + CRXE_ID * 4) : 0;
    int W = raw_info.width, pitch = raw_info.pitch;
    int next = get_ms_clock();
    while (cwl_phase != -1)
    {
        int ph = cwl_phase;
        if (ph >= 1 && !locked && !cwl_load_err)
        {
            lock = rom_CreateResLockEntry(res_ids, 2);
            unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
            if (r & 1) { if (lock) rom_DeleteResLockEntry(lock); lock = 0; cwl_load_err = 1; cwl_load_err_phase = ph; }
            else
            {
                locked = 1;
                rom_PwrMngEnableSubChip(subchips);
                o_rd24 = MEM(PACK_UNIT_RD61 + 0x24); o_wr24 = MEM(PACK_UNIT_WR24 + 0x24);
                o_rdD0 = MEM(M2M_RD_BASE + 0xD0);    o_wrD0 = MEM(M2M_WR_BASE + 0xD0);
                if (idflag) *idflag = 0;          /* Id 1 set up again with our size at the first encode */
            }
        }
        if (ph >= 2 && ph < CWL_PHASES && locked && !cwl_load_err)
        {
            uint32_t t0 = get_us_clock();
            int ms16 = m2m_copy_pack(raw_info.buffer, cwl_e16, pitch, W * 2, CRXE_H, rdA, w16, 0);
            MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
            MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
            if (ms16 < 0) { cwl_load_err = 2; cwl_load_err_phase = ph; }
            else if (ph >= 3)
            {
                uint32_t entry[8] = { CRXE_TYPE, W, CRXE_H, 0, 0, 16, (uint32_t) UNCACHEABLE(cwl_e16), W };
                crxe_done = 0; crxe_handle = 0; crxe_late = 0;
                while (take_semaphore(crxe_sem, 1) == 0) ;   /* drop a stale give */
                uint32_t r = ALGSMGR_StartEncode(entry, 1, crxe_cbr, (void *) 0x43574c44);
                if (r & 1) { cwl_load_err = 3; cwl_load_err_phase = ph; }
                else
                {
                    take_semaphore(crxe_sem, 500);
                    if (!crxe_done) { crxe_late = 1; cwl_load_err = 4; cwl_load_err_phase = ph; }
                    else
                    {
                        uint32_t h = crxe_handle;
                        if (!h || (h & 1)) { cwl_load_err = 5; cwl_load_err_phase = ph; }
                        else
                        {
                            if (ph >= 4)
                            {
                                int n = 0, total = 0; uint32_t *addrs = 0, *sizes = 0;
                                if (ALGSMGR_GetRingList(h, &n, &addrs, &sizes) == 0 && n > 0 && n <= 64 &&
                                    crx_ptr_ok((uint32_t) addrs) && crx_ptr_ok((uint32_t) sizes))
                                {
                                    for (int i = 0; i < n; i++)
                                    {
                                        uint32_t a = addrs[i], s = sizes[i];
                                        if (!a || !s || !crx_ptr_ok(a & ~3)) continue;
                                        if (total + (int)((s + 63) & ~63) > CWL_DST_SIZE) break;
                                        crxm_copy_chunk((void *) a, cwl_dst + total, s);
                                        total += (s + 63) & ~63;
                                    }
                                }
                            }
                            ALGSMGR_FreeEncBuffer(h, 0, 0);
                        }
                    }
                }
            }
            int us = get_us_clock() - t0;
            cwl_frames[ph]++;
            cwl_us[ph] += us;
            if (us > cwl_us_max[ph]) cwl_us_max[ph] = us;
        }
        next += 42;
        int now = get_ms_clock();
        if (next > now) msleep(next - now); else next = now;
    }
    if (locked)
    {
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        rom_PwrMngDisableSubChip(subchips);
        rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock);
    }
    if (idflag) *idflag = 0;                  /* Canon re-initializes Id 1 for its own photos */
    cwl_load_running = 0;
}

static void cwl_task()
{
    msleep(3000);
    FILE *log = 0;
    void *buf = 0, *e_raw = 0, *d_raw = 0;
    int done = 0, failed_at = -1, failed_ret = 0, gate = 0, raw_on = 0;
    int create_ms[CWL_PHASES] = {0}, close_ms[CWL_PHASES] = {0};
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    for (int p = 0; p < CWL_PHASES; p++) { cwl_frames[p] = 0; cwl_us[p] = 0; cwl_us_max[p] = 0; }
    cwl_load_err = 0; cwl_load_err_phase = -1;

    if (is_movie_mode() || !lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f) { gate = 1; goto report; }
    raw_lv_request(); raw_on = 1;
    msleep(500);
    if (!raw_update_params() || raw_info.bits_per_pixel != 14 || raw_info.width < 1024 || raw_info.width > 4000 ||
        (raw_info.width & 15) || raw_info.pitch != raw_info.width * 14 / 8 || raw_info.height < CRXE_H) { gate = 2; goto report; }
    buf = fio_malloc(CWT_BLOCK + 64);
    e_raw = fio_malloc(raw_info.width * 2 * CRXE_H + 128);
    d_raw = fio_malloc(CWL_DST_SIZE + 128);
    if (!buf || !e_raw || !d_raw) { gate = 3; goto report; }
    cwl_e16 = (void *)(((uintptr_t) e_raw + 63) & ~63);
    cwl_dst = (void *)(((uintptr_t) d_raw + 63) & ~63);
    for (int i = 0; i < CWT_BLOCK / 4; i++) ((uint32_t *) buf)[i] = i * 2654435761u;
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);
    crxe_used = 1;

    cwl_phase = 0;
    cwl_load_running = 1;
    task_create("cwl_load", 0x0F, 0x2000, cwl_load_task, 0);

    int t0 = get_ms_clock();
    int k = 0;
    for (int p = 0; p < CWL_PHASES && failed_at < 0; p++)
    {
        cwl_phase = p;
        char name[16];
        snprintf(name, sizeof(name), "CWL%d.BIN", p);
        int tc = get_ms_clock();
        FILE *f = FIO_CreateFile(name);
        create_ms[p] = get_ms_clock() - tc;
        if (!f) { failed_at = k; failed_ret = -1; break; }
        for (int i = 0; i < CWT_PER_FILE; i++, k++)
        {
            int due = t0 + k * 200;
            int now = get_ms_clock();
            if (now < due) msleep(due - now);
            int a = get_ms_clock();
            int w = FIO_WriteFile(f, buf, CWT_BLOCK);
            int ms = get_ms_clock() - a;
            cwl_t[k] = a - t0;
            cwl_ms[k] = ms > 65535 ? 65535 : ms;
            if (w != CWT_BLOCK) { failed_at = k; failed_ret = w; k++; break; }
            if (k % 20 == 0)
                NotifyBox(2000, "write+load test: step %d of 5, %d of %d MB, last %d ms", p + 1, (k + 1) * 8, CWL_TOTAL * 8, ms);
        }
        tc = get_ms_clock();
        FIO_CloseFile(f);
        close_ms[p] = get_ms_clock() - tc;
        done = k;
    }
    cwl_phase = -1;
    for (int i = 0; i < 100 && cwl_load_running; i++) msleep(50);

report:
    if (raw_on) raw_lv_release();
    log = FIO_CreateFile("ML/LOGS/CWLOAD.TXT");
    if (log)
    {
        my_fprintf(log, "=== CWLOAD lv=%d movie=%d gate=%d W=%d H=%d writes=%d failed_at=%d ret=%d load_err=%d at phase %d load_running=%d\n",
                   lv, is_movie_mode(), gate, raw_info.width, raw_info.height, done, failed_at, failed_ret,
                   cwl_load_err, cwl_load_err_phase, cwl_load_running);
        for (int p = 0; p < CWL_PHASES; p++)
        {
            int n = 0, over300 = 0, over500 = 0, over1000 = 0, maxms = 0, run = 0, slow = 0, sum = 0;
            for (int i = p * CWT_PER_FILE; i < (p + 1) * CWT_PER_FILE && i < done; i++)
            {
                int m = cwl_ms[i];
                n++; sum += m;
                if (m > maxms) maxms = m;
                over300 += m > 300; over500 += m > 500; over1000 += m > 1000;
                if (m > 300) { run++; if (run == 3) slow++; } else run = 0;
            }
            my_fprintf(log, "phase %d: writes %d, avg %d ms, speed %d KiB/s, over 300 %d, over 500 %d, over 1000 %d, max %d ms, slow phases %d; "
                            "load frames %d, avg %d us, max %d us; create %d ms, close %d ms\n",
                       p, n, n ? sum / n : 0, sum ? (int)((long long) n * 8 * 1024 * 1000 / sum) : 0, over300, over500, over1000, maxms, slow,
                       cwl_frames[p], cwl_frames[p] ? cwl_us[p] / cwl_frames[p] : 0, cwl_us_max[p], create_ms[p], close_ms[p]);
        }
        for (int i = 0; i < done; i++) my_fprintf(log, "%d %d %d %d\n", i, i / CWT_PER_FILE, cwl_t[i], cwl_ms[i]);
        FIO_CloseFile(log);
    }
    if (!cwl_load_running)
    {
        if (e_raw) fio_free(e_raw);
        if (d_raw) fio_free(d_raw);
    }
    if (buf) fio_free(buf);
    NotifyBox(10000, gate ? "write+load test: gate %d, see ML/LOGS/CWLOAD.TXT" : "write+load test: done. ML/LOGS/CWLOAD.TXT", gate);
    cwl_phase = -2;
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(cwl_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("cwl_task", 0x19, 0x2000, cwl_task, 0);
}

/* Card write + ALL shoot memory held (2026-11-07). cardtest_load1 (full per-frame load, 16 MB of
 * fio memory) and cardtest_pc2 (recording-like sizes on the PC) showed no slow phases. mlv_lite
 * holds ALL shoot memory during REC (shoot_malloc_suite(0)), so test that too:
 *   step A: no load, all shoot memory held, 8 MiB writes from the fio buffer
 *   step B: full recording load (cwl_load_task phase 4), all shoot memory held, same writes
 *   step C: full load, all shoot memory held, recording-like writes: groups of 1-6 frames of
 *           1.35-1.85 MB (512-byte multiples) FROM the held shoot memory, rotating addresses
 * 2 GiB per step at 37 MiB/s. Files CWM0..CWM2.BIN in the card root, rewritten on every run.
 * Log: ML/LOGS/CWMEM.TXT at the end. Photo M, LV, not recording. */
#define CWM_STEPS 3
#define CWM_STEP_BYTES (2048LL * 1024 * 1024)
#define CWM_MAXW 1500
static uint16_t cwm_ms[CWM_STEPS * CWM_MAXW];
static int cwm_t[CWM_STEPS * CWM_MAXW], cwm_sz[CWM_STEPS * CWM_MAXW];
static uint32_t cwm_rng = 1;
static uint32_t cwm_rand() { cwm_rng = cwm_rng * 1103515245u + 12345u; return cwm_rng >> 8; }

static void cwm_task()
{
    msleep(3000);
    FILE *log = 0;
    void *buf = 0, *e_raw = 0, *d_raw = 0;
    struct memSuite *suite = 0;
    int gate = 0, raw_on = 0, failed_at = -1, failed_ret = 0, load_started = 0;
    int nw[CWM_STEPS] = {0}, create_ms[CWM_STEPS] = {0}, close_ms[CWM_STEPS] = {0}, frames0[CWM_STEPS + 1] = {0};
    uint8_t *big = 0; int big_size = 0, suite_size = 0, n_chunks = 0;
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    for (int p = 0; p < CWL_PHASES; p++) { cwl_frames[p] = 0; cwl_us[p] = 0; cwl_us_max[p] = 0; }
    cwl_load_err = 0; cwl_load_err_phase = -1;

    if (is_movie_mode() || !lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f) { gate = 1; goto report; }
    raw_lv_request(); raw_on = 1;
    msleep(500);
    if (!raw_update_params() || raw_info.bits_per_pixel != 14 || raw_info.width < 1024 || raw_info.width > 4000 ||
        (raw_info.width & 15) || raw_info.pitch != raw_info.width * 14 / 8 || raw_info.height < CRXE_H) { gate = 2; goto report; }
    buf = fio_malloc(CWT_BLOCK + 64);
    e_raw = fio_malloc(raw_info.width * 2 * CRXE_H + 128);
    d_raw = fio_malloc(CWL_DST_SIZE + 128);
    if (!buf || !e_raw || !d_raw) { gate = 3; goto report; }
    cwl_e16 = (void *)(((uintptr_t) e_raw + 63) & ~63);
    cwl_dst = (void *)(((uintptr_t) d_raw + 63) & ~63);
    for (int i = 0; i < CWT_BLOCK / 4; i++) ((uint32_t *) buf)[i] = i * 2654435761u;
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);
    crxe_used = 1;

    /* everything that is left, like mlv_lite on REC */
    suite = shoot_malloc_suite(0);
    if (!suite) { gate = 4; goto report; }
    suite_size = suite->size;
    for (struct memChunk *c = GetFirstChunkFromSuite(suite); c; c = GetNextMemoryChunk(suite, c))
    {
        n_chunks++;
        if ((int) GetSizeOfMemoryChunk(c) > big_size) { big_size = GetSizeOfMemoryChunk(c); big = GetMemoryAddressOfMemoryChunk(c); }
    }
    if (big_size < 16 * 1024 * 1024) { gate = 5; goto report; }
    big = (void *)(((uintptr_t) big + 63) & ~63); big_size -= 64;
    for (int i = 0; i < big_size / 4; i += 1024) ((volatile uint32_t *) UNCACHEABLE(big))[i] = i * 2654435761u;

    cwl_phase = 0;                 /* load task idle until step B */
    cwl_load_running = 1;
    load_started = 1;
    task_create("cwl_load", 0x0F, 0x2000, cwl_load_task, 0);

    int t0 = get_ms_clock();
    long long total = 0;
    int k = 0, off = 0;
    for (int p = 0; p < CWM_STEPS && failed_at < 0; p++)
    {
        frames0[p] = cwl_frames[4];
        cwl_phase = p == 0 ? 0 : 4;
        char name[16];
        snprintf(name, sizeof(name), "CWM%d.BIN", p);
        int tc = get_ms_clock();
        FILE *f = FIO_CreateFile(name);
        create_ms[p] = get_ms_clock() - tc;
        if (!f) { failed_at = k; failed_ret = -1; break; }
        long long step_end = total + CWM_STEP_BYTES;
        while (total < step_end && nw[p] < CWM_MAXW)
        {
            const void *src; int size;
            if (p < 2) { src = buf; size = CWT_BLOCK; }
            else
            {
                int frames = 1 + cwm_rand() % 8; if (frames > 6) frames = 6;
                size = 0;
                for (int q = 0; q < frames; q++) size += ((int)(1415577 + cwm_rand() % 524288) + 511) & ~511;
                if (off + size > big_size) off = 0;
                src = big + off;
                off = (off + size + 63) & ~63;
            }
            long long due = t0 + total * 1000 / (37LL * 1024 * 1024);
            int now = get_ms_clock();
            if (now < due) msleep((int)(due - now));
            int a = get_ms_clock();
            int w = FIO_WriteFile(f, src, size);
            int ms = get_ms_clock() - a;
            cwm_t[k] = a - t0; cwm_ms[k] = ms > 65535 ? 65535 : ms; cwm_sz[k] = size;
            k++; nw[p]++;
            if (w != size) { failed_at = k - 1; failed_ret = w; break; }
            total += size;
            if (k % 20 == 0)
                NotifyBox(2000, "memory test: step %d of 3, %d MB, last %d ms", p + 1, (int)(total >> 20), ms);
        }
        tc = get_ms_clock();
        FIO_CloseFile(f);
        close_ms[p] = get_ms_clock() - tc;
    }
    frames0[CWM_STEPS] = cwl_frames[4];
    cwl_phase = -1;
    for (int i = 0; i < 100 && cwl_load_running; i++) msleep(50);

report:
    if (suite) { shoot_free_suite(suite); suite = 0; }
    if (raw_on) raw_lv_release();
    log = FIO_CreateFile("ML/LOGS/CWMEM.TXT");
    if (log)
    {
        my_fprintf(log, "=== CWMEM gate=%d W=%d suite %d bytes in %d chunks, biggest %d at %x, failed_at=%d ret=%d load_err=%d (phase %d) load_running=%d\n",
                   gate, raw_info.width, suite_size, n_chunks, big_size, big, failed_at, failed_ret, cwl_load_err, cwl_load_err_phase, cwl_load_running);
        int base = 0;
        for (int p = 0; p < CWM_STEPS; p++)
        {
            int over300 = 0, over500 = 0, over1000 = 0, maxms = 0, run = 0, slow = 0;
            long long bytes = 0, msum = 0;
            for (int i = base; i < base + nw[p]; i++)
            {
                int m = cwm_ms[i];
                bytes += cwm_sz[i]; msum += m;
                if (m > maxms) maxms = m;
                over300 += m > 300; over500 += m > 500; over1000 += m > 1000;
                /* slow = a write below 20 MB/s (bytes/ms < 20000) */
                int is_slow = cwm_sz[i] > 4000000 && (long long) cwm_sz[i] < 20000LL * m;
                if (is_slow) { run++; if (run == 2) slow++; } else run = 0;
            }
            my_fprintf(log, "step %c: writes %d, %d MB, pure speed %d KiB/s, over 300 %d, over 500 %d, over 1000 %d, max %d ms, slow phases %d; "
                            "load frames %d; create %d ms, close %d ms\n",
                       'A' + p, nw[p], (int)(bytes >> 20), msum ? (int)(bytes * 1000 / 1024 / msum) : 0, over300, over500, over1000, maxms, slow,
                       frames0[p + 1] - frames0[p], create_ms[p], close_ms[p]);
            base += nw[p];
        }
        my_fprintf(log, "load: frames %d, avg %d us, max %d us\n", cwl_frames[4], cwl_frames[4] ? cwl_us[4] / cwl_frames[4] : 0, cwl_us_max[4]);
        base = 0;
        for (int p = 0; p < CWM_STEPS; p++)
        {
            for (int i = base; i < base + nw[p]; i++) my_fprintf(log, "%d %d %d %d %d\n", i, p, cwm_t[i], cwm_ms[i], cwm_sz[i]);
            base += nw[p];
        }
        FIO_CloseFile(log);
    }
    if (!load_started || !cwl_load_running)
    {
        if (e_raw) fio_free(e_raw);
        if (d_raw) fio_free(d_raw);
    }
    if (buf) fio_free(buf);
    NotifyBox(10000, gate ? "memory test: gate %d, see ML/LOGS/CWMEM.TXT" : "memory test: done. ML/LOGS/CWMEM.TXT", gate);
    cwl_phase = -2;
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(cwm_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("cwm_task", 0x19, 0x2000, cwm_task, 0);
}

/* Card write + console log (2026-11-07). No earlier write test printed anything while writing,
 * but during REC mlv_lite prints (CRX t lines, slow writes, GUI events), and console_task
 * (CONFIG_CONSOLE_LOG_FILE) then appends ML/LOGS/CONSOLE.LOG every 200 ms with
 * FIO_CreateFileOrAppend + write + close: a second file written between the MLV writes.
 *   step A: 8 MiB writes at 40 MiB/s, no printf
 *   step B: same + one printf after every write (log append about every 200 ms)
 *   step C: same + four printf lines after every write
 * 2 GiB per step. Files CWC0..CWC2.BIN in the card root, rewritten on every run.
 * Log: ML/LOGS/CWCON.TXT at the end. Photo M, LV, not recording. */
#define CWC_STEPS 3
static uint16_t cwc_ms[CWC_STEPS * CWT_PER_FILE];
static int cwc_t[CWC_STEPS * CWT_PER_FILE];

static void cwc_task()
{
    msleep(3000);
    void *buf = fio_malloc(CWT_BLOCK + 64);
    int done = 0, failed_at = -1, failed_ret = 0;
    int create_ms[CWC_STEPS] = {0}, close_ms[CWC_STEPS] = {0};
    if (buf)
    {
        for (int i = 0; i < CWT_BLOCK / 4; i++) ((uint32_t *) buf)[i] = i * 2654435761u;
        int t0 = get_ms_clock();
        int k = 0;
        for (int p = 0; p < CWC_STEPS && failed_at < 0; p++)
        {
            char name[16];
            snprintf(name, sizeof(name), "CWC%d.BIN", p);
            int tc = get_ms_clock();
            FILE *f = FIO_CreateFile(name);
            create_ms[p] = get_ms_clock() - tc;
            if (!f) { failed_at = k; failed_ret = -1; break; }
            for (int i = 0; i < CWT_PER_FILE; i++, k++)
            {
                int due = t0 + k * 200;
                int now = get_ms_clock();
                if (now < due) msleep(due - now);
                int a = get_ms_clock();
                int w = FIO_WriteFile(f, buf, CWT_BLOCK);
                int ms = get_ms_clock() - a;
                cwc_t[k] = a - t0;
                cwc_ms[k] = ms > 65535 ? 65535 : ms;
                if (w != CWT_BLOCK) { failed_at = k; failed_ret = w; k++; break; }
                if (p >= 1) printf("cwc step %d write %d: %d ms\n", p, k, ms);
                if (p >= 2)
                {
                    printf("cwc t%d f%d 1500000B (55%%) 2100/3300/429392408 us\n", a, k);
                    printf("cwc rec event 0x79\n");
                    printf("cwc rec event 0x80\n");
                }
                if (k % 20 == 0)
                    NotifyBox(2000, "log test: step %d of 3, %d of %d MB, last %d ms", p + 1, (k + 1) * 8, CWC_STEPS * CWT_PER_FILE * 8, ms);
            }
            tc = get_ms_clock();
            FIO_CloseFile(f);
            close_ms[p] = get_ms_clock() - tc;
            done = k;
        }
    }

    FILE *log = FIO_CreateFile("ML/LOGS/CWCON.TXT");
    if (log)
    {
        my_fprintf(log, "=== CWCON lv=%d movie=%d buf=%x writes=%d failed_at=%d ret=%d\n", lv, is_movie_mode(), buf, done, failed_at, failed_ret);
        for (int p = 0; p < CWC_STEPS; p++)
        {
            int n = 0, over300 = 0, over500 = 0, over1000 = 0, maxms = 0, run = 0, slow = 0, sum = 0;
            for (int i = p * CWT_PER_FILE; i < (p + 1) * CWT_PER_FILE && i < done; i++)
            {
                int m = cwc_ms[i];
                n++; sum += m;
                if (m > maxms) maxms = m;
                over300 += m > 300; over500 += m > 500; over1000 += m > 1000;
                if (m > 300) { run++; if (run == 3) slow++; } else run = 0;
            }
            my_fprintf(log, "step %d: writes %d, avg %d ms, speed %d KiB/s, over 300 %d, over 500 %d, over 1000 %d, max %d ms, slow phases %d; create %d ms, close %d ms\n",
                       p, n, n ? sum / n : 0, sum ? (int)((long long) n * 8 * 1024 * 1000 / sum) : 0, over300, over500, over1000, maxms, slow, create_ms[p], close_ms[p]);
        }
        for (int i = 0; i < done; i++) my_fprintf(log, "%d %d %d %d\n", i, i / CWT_PER_FILE, cwc_t[i], cwc_ms[i]);
        FIO_CloseFile(log);
    }
    if (buf) fio_free(buf);
    NotifyBox(10000, buf ? "log test: done. ML/LOGS/CWCON.TXT" : "log test: no memory");
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(cwc_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("cwc_task", 0x19, 0x2000, cwc_task, 0);
}

/* True 12/10-bit lossless, step 1 (2026-11-07). Our EDMAC write pack {1,1,2,0,0} gives 16-bit
 * samples = pixel << 2 (crx16_2); mlv_lite's 12/10-bit uncompressed path uses {0,1,sel,0,0} with
 * sel 1 = 12-bit, 0 = 10-bit. So try {1,1,sel,0,0} and {1,0,sel,0,0}: 16-bit containers holding
 * a 12/10-bit value. 64-line trials checked against: p>>s, (p>>s)<<s, (p>>s)<<(s+2), p<<2,
 * rounded p>>s, rounded p>>s << (s+2) (s = 14 - depth). If one is exact on the whole frame,
 * encode it with field 0 (and field 3 when the value is left aligned, then still exact).
 * Files: ML/LOGS/CRXD.TXT, CRXD12.RAW / CRXD10.RAW (16-bit input), CRXD12_0.BIN, CRXD12_3.BIN,
 * CRXD10_0.BIN, CRXD10_3.BIN. Photo M, LiveView, not recording, fresh boot (once per boot). */
static int crxd_encode(FILE *f, uint8_t *e16, int W, int field, const char *fname, int *keep)
{
    volatile uint32_t *rbits = 0;
    uint32_t cg = *(volatile uint32_t *) 0x111b8;
    uint32_t blk = crx_ptr_ok(cg) ? ((volatile uint32_t *) cg)[0x28 / 4] : 0;
    uint32_t idb = crx_ptr_ok(blk) ? blk + CRXE_ID * 0x288 : 0;
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    int inited = crx_ptr_ok(am) && ((volatile uint32_t *) (am + 0x108))[CRXE_ID] == 1;
    if (inited)
    {
        if (!idb || ((volatile uint16_t *) (idb + 0x200))[0] != W || ((volatile uint16_t *) (idb + 0x200))[1] != CRXE_H)
        { my_fprintf(f, "%s: Id block %x not ours, skip\n", fname, idb); return -1; }
        rbits = (volatile uint32_t *) (idb + 0x214);
        if (rbits[0] > 3 || rbits[1] > 3 || rbits[2] > 3 || rbits[3] > 3) { my_fprintf(f, "%s: +0x214 not small, skip\n", fname); return -1; }
        for (int p = 0; p < 4; p++) rbits[p] = field;
    }
    else if (field)
    {
        my_fprintf(f, "%s: encoder not set up yet, field %d skipped\n", fname, field);
        return -1;
    }
    uint32_t entry[8] = { CRXE_TYPE, W, CRXE_H, 0, 0, 16, (uint32_t) UNCACHEABLE(e16), W };
    crxe_done = 0; crxe_handle = 0; crxe_late = 0;
    while (take_semaphore_now(crxe_sem) == 0) ;
    uint32_t t0 = get_us_clock();
    uint32_t r = ALGSMGR_StartEncode(entry, 1, crxe_cbr, (void *) 0x43525844);
    if (r & 1) { my_fprintf(f, "%s: StartEncode %x\n", fname, r); return -1; }
    take_semaphore(crxe_sem, 1000);
    if (!crxe_done) { crxe_late = 1; *keep = 1; my_fprintf(f, "%s: TIMEOUT (input kept)\n", fname); return -2; }
    uint32_t h = crxe_handle;
    if (!h || (h & 1)) { my_fprintf(f, "%s: encoder error %x\n", fname, h); return -1; }
    int n = 0, total = 0; uint32_t *addrs = 0, *sizes = 0;
    uint32_t g = ALGSMGR_GetRingList(h, &n, &addrs, &sizes);
    if (g == 0 && n > 0 && n <= 64 && crx_ptr_ok((uint32_t) addrs) && crx_ptr_ok((uint32_t) sizes))
    {
        FILE *fo = FIO_CreateFile(fname);
        void *tmp_raw = fio_malloc(256 * 1024 + 64);
        void *tmp = tmp_raw ? (void *)(((uintptr_t) tmp_raw + 63) & ~63) : 0;
        for (int i = 0; i < n; i++)
        {
            uint32_t a = addrs[i], s = sizes[i];
            if (!a || !s || s > 16 * 1024 * 1024 || !crx_ptr_ok(a & ~3)) continue;
            if (fo && tmp)
                for (uint32_t off = 0; off < s; off += 256 * 1024)
                {
                    uint32_t len = MIN(s - off, 256 * 1024);
                    memcpy(tmp, UNCACHEABLE(a + off), len);
                    FIO_WriteFile(fo, tmp, len);
                }
            total += s;
        }
        if (fo) FIO_CloseFile(fo);
        if (tmp_raw) fio_free(tmp_raw);
    }
    ALGSMGR_FreeEncBuffer(h, 0, 0);
    if (rbits) for (int p = 0; p < 4; p++) rbits[p] = 0;
    my_fprintf(f, "%s: field %d, %d us, %d B = %d %% of 14-bit\n", fname, field, (int)(crxe_t_cbr - t0), total,
               (int)((int64_t) total * 100 / (W * 14 / 8 * CRXE_H)));
    return total;
}

static void crxd_task()
{
    /* crxd1: the check read the LIVE LV buffer, which Canon overwrites every frame, so nothing could
     * match (values looked like p >> 2 of another frame). Now: freeze one frame in our own buffer,
     * convert that copy, check against that copy. Trials: write pack {1,1,sel,0,0} and {1,0,sel,0,0}
     * for sel 0..3, each checked as 12-bit (s = 2) and 10-bit (s = 4) candidates. */
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    static uint32_t rdA[4] = { 0, 2, 0, 0 };
    static uint32_t wtry[8][5];
    void *e_raw = 0, *in_raw = 0; int keep = 0;
    volatile uint32_t *idflag = 0;
    msleep(3000);
    FILE *f = FIO_CreateFile("ML/LOGS/CRXD.TXT");
    if (!f) { rawdump_running = 0; return; }
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    my_fprintf(f, "=== CRXD v2 movie=%d lv=%d am=%x used=%d\n", is_movie_mode(), lv, am, crxe_used);
    if (crxe_used || is_movie_mode() || !lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f)
    { my_fprintf(f, "gate failed\n"); NotifyBox(5000, "crxd: gate failed (photo M + LV, once per boot)"); goto end; }
    if (((volatile uint32_t *) am)[0xa8 / 4] & 2) { my_fprintf(f, "gate: am+0xa8\n"); goto end; }
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);
    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    int W = raw_info.width, pitch = raw_info.pitch;
    my_fprintf(f, "raw ok=%d buf=%x %dx%d pitch=%d\n", ok, raw_info.buffer, W, raw_info.height, pitch);
    if (!ok || raw_info.bits_per_pixel != 14 || W < 1024 || W > 4000 || (W & 15) || pitch != W * 14 / 8 || raw_info.height < CRXE_H)
    { my_fprintf(f, "gate failed: raw geometry\n"); raw_lv_release(); goto end; }
    int e_size = W * 2 * CRXE_H, in_size = pitch * CRXE_H;
    e_raw = fio_malloc(e_size + 128);
    in_raw = fio_malloc(in_size + 128);
    if (!e_raw || !in_raw) { my_fprintf(f, "fio_malloc failed\n"); raw_lv_release(); goto end; }
    uint8_t *e16 = (void *)(((uintptr_t) e_raw + 63) & ~63);
    uint8_t *in = (void *)(((uintptr_t) in_raw + 63) & ~63);
    uint16_t *d = (uint16_t *) UNCACHEABLE(e16);
    const uint8_t *src = (const uint8_t *) UNCACHEABLE(in);
    memcpy(UNCACHEABLE(in), UNCACHEABLE(raw_info.buffer), in_size);   /* one frozen frame */
    raw_lv_release();

    idflag = (volatile uint32_t *) (am + 0x108 + CRXE_ID * 4);
    *idflag = 0;
    crxe_used = 1;

    for (int t = 0; t < 8; t++)
    {
        wtry[t][0] = 1; wtry[t][1] = (t & 1) ? 0 : 1; wtry[t][2] = t >> 1; wtry[t][3] = 0; wtry[t][4] = 0;
    }
    int found_t[2] = { -1, -1 }, found_c[2] = { -1, -1 };   /* [0] = 12-bit, [1] = 10-bit */

    void *lock = rom_CreateResLockEntry(res_ids, 2);
    unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
    if (r & 1) { my_fprintf(f, "lock failed %x\n", r); if (lock) rom_DeleteResLockEntry(lock); goto end; }
    rom_PwrMngEnableSubChip(subchips);
    uint32_t o_rd24 = MEM(PACK_UNIT_RD61 + 0x24), o_wr24 = MEM(PACK_UNIT_WR24 + 0x24);
    uint32_t o_rdD0 = MEM(M2M_RD_BASE + 0xD0),    o_wrD0 = MEM(M2M_WR_BASE + 0xD0);

    for (int t = 0; t < 8; t++)
    {
        memset(d, 0, W * 2 * 64);
        int ms = m2m_copy_pack(in, e16, pitch, W * 2, 64, rdA, wtry[t], 0);
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        int bad[2][6];
        for (int k = 0; k < 2; k++) for (int c = 0; c < 6; c++) bad[k][c] = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < W; x++)
            {
                uint32_t p = crx16_px(src + y * pitch + (x / 8) * 14, x % 8);
                uint32_t v = d[y * W + x];
                for (int k = 0; k < 2; k++)
                {
                    int s = k ? 4 : 2, maxv = (1 << (14 - s)) - 1;
                    uint32_t rnd = (p + (1u << (s - 1))) >> s; if (rnd > (uint32_t) maxv) rnd = maxv;
                    if (v != (p >> s)) bad[k][0]++;
                    if (v != ((p >> s) << s)) bad[k][1]++;
                    if (v != ((p >> s) << (s + 2))) bad[k][2]++;
                    if (v != (p << 2)) bad[k][3]++;
                    if (v != rnd) bad[k][4]++;
                    if (v != (rnd << (s + 2))) bad[k][5]++;
                }
            }
        my_fprintf(f, "trial %d wr {%d,%d,%d,0,0} ms=%d | 12-bit bad %d %d %d %d %d %d | 10-bit bad %d %d %d %d %d %d | d %x %x %x %x px %x %x %x %x\n",
                   t, wtry[t][0], wtry[t][1], wtry[t][2], ms,
                   bad[0][0], bad[0][1], bad[0][2], bad[0][3], bad[0][4], bad[0][5],
                   bad[1][0], bad[1][1], bad[1][2], bad[1][3], bad[1][4], bad[1][5],
                   d[0], d[1], d[2], d[3], crx16_px(src, 0), crx16_px(src, 1), crx16_px(src, 2), crx16_px(src, 3));
        int order[4] = { 0, 4, 2, 5 };
        for (int k = 0; k < 2 && ms >= 0; k++)
            if (found_t[k] < 0)
                for (int c = 0; c < 4; c++)
                    if (bad[k][order[c]] == 0) { found_t[k] = t; found_c[k] = order[c]; break; }
    }

    /* full frame for each depth that has an exact candidate */
    static int full_bad[2];
    for (int k = 0; k < 2; k++)
    {
        full_bad[k] = -1;
        if (found_t[k] < 0) { my_fprintf(f, "%d-bit: no exact candidate\n", k ? 10 : 12); continue; }
        int s = k ? 4 : 2, maxv = (1 << (14 - s)) - 1;
        int ms = m2m_copy_pack(in, e16, pitch, W * 2, CRXE_H, rdA, wtry[found_t[k]], 0);
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        full_bad[k] = (ms < 0) ? W * CRXE_H : 0;
        for (int y = 0; y < CRXE_H && ms >= 0; y++)
            for (int x = 0; x < W; x++)
            {
                uint32_t p = crx16_px(src + y * pitch + (x / 8) * 14, x % 8);
                uint32_t rnd = (p + (1u << (s - 1))) >> s; if (rnd > (uint32_t) maxv) rnd = maxv;
                int c = found_c[k];
                uint32_t want = c == 0 ? (p >> s) : c == 4 ? rnd : c == 2 ? ((p >> s) << (s + 2)) : (rnd << (s + 2));
                if (d[y * W + x] != want) full_bad[k]++;
            }
        my_fprintf(f, "%d-bit: trial %d, candidate %d, full frame ms=%d bad=%d of %d\n", k ? 10 : 12, found_t[k], found_c[k], ms, full_bad[k], W * CRXE_H);
        if (full_bad[k] == 0)
        {
            char fn[32];
            snprintf(fn, sizeof(fn), "ML/LOGS/CRXD%d.RAW", k ? 10 : 12);
            FILE *fi = FIO_CreateFile(fn);
            if (fi) { FIO_WriteFile(fi, UNCACHEABLE(e16), e_size); FIO_CloseFile(fi); }
            /* encode needs the EDMAC lock released? crxr encoded after unlock; keep the same order */
            rom_PwrMngDisableSubChip(subchips);
            rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock); lock = 0;
            snprintf(fn, sizeof(fn), "ML/LOGS/CRXD%d_0.BIN", k ? 10 : 12);
            if (crxd_encode(f, e16, W, 0, fn, &keep) == -2) goto end_enc;
            if (found_c[k] == 2 || found_c[k] == 5)
            {
                snprintf(fn, sizeof(fn), "ML/LOGS/CRXD%d_3.BIN", k ? 10 : 12);
                if (crxd_encode(f, e16, W, 3, fn, &keep) == -2) goto end_enc;
            }
            lock = rom_CreateResLockEntry(res_ids, 2);
            r = lock ? rom_LockEngineResources(lock) : 1;
            if (r & 1) { my_fprintf(f, "relock failed %x\n", r); if (lock) rom_DeleteResLockEntry(lock); lock = 0; goto end_enc; }
            rom_PwrMngEnableSubChip(subchips);
        }
    }
    if (lock)
    {
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        rom_PwrMngDisableSubChip(subchips);
        rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock); lock = 0;
    }
end_enc:
    NotifyBox(8000, "crxd: done. ML/LOGS/CRXD.TXT");
end:
    if (idflag) { *idflag = 0; my_fprintf(f, "Id %d init flag reset\n", CRXE_ID); }
    if (e_raw && !keep) fio_free(e_raw);
    if (in_raw) fio_free(in_raw);
    FIO_CloseFile(f);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(crxd_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("crxd_task", 0x1e, 0x4000, crxd_task, 0);
}

/* CRX encoder in MOVIE LV (2026-11-07). mlv_lite's encoder timed out in movie mode (mode1210_1);
 * there mlv_lite holds all shoot memory from the start, and ALGSMGR asks Canon's resource task
 * for its output buffers at the first encode (waits with no timeout, DEV_PLAN "ROM for the one-shot
 * test"). So this runs with mlv_lite's RAW video OFF (memory free) and measures, in any LV mode:
 * windows full active / 3072x1296 / 2560x1080 / 1920x1080 (centered): EDMAC 14->16 crop time,
 * first encode (with EncInit) and second encode times, output size, ALGSMGR buffer count.
 * Saves the 2560x1080 window (16-bit input + CRX output) for a PC decode check.
 * Files: ML/LOGS/CRXV.TXT (appended per run), CRXV25.RAW, CRXV25.BIN. Re-runnable. */
static int m2m_crop16(const uint8_t *src, int pitch, void *dst, int w, int h)
{
    static uint32_t chans[2] = { 0x3D, 0x18 };
    static struct edmac_info_d8 sinfo, dinfo;
    static uint32_t size_arg[3];
    static void *addr_arg[2];
    static uint32_t rdA[4] = { 0, 2, 0, 0 }, w16[5] = { 1, 1, 2, 0, 0 };
    memset(&sinfo, 0, sizeof(sinfo)); memset(&dinfo, 0, sizeof(dinfo));
    sinfo.xb = w * 14 / 8; sinfo.off1b = pitch - sinfo.xb; sinfo.yb = h - 1;
    dinfo.xb = w * 2; dinfo.yb = h - 1;
    size_arg[0] = (uint32_t) &sinfo; size_arg[1] = (uint32_t) &dinfo; size_arg[2] = 0;
    addr_arg[0] = UNCACHEABLE(src); addr_arg[1] = UNCACHEABLE(dst);
    if (!m2m_sem) m2m_sem = create_named_semaphore("m2m_done", SEM_CREATE_LOCKED);
    while (take_semaphore_now(m2m_sem) == 0) ;
    m2m_done = 0;
    rom_m2m5_Setup(chans);
    rom_m2m5_RegisterCBR(m2m_copy_cbr, 0);
    rom_m2m5_SetAddress(addr_arg);
    rom_m2m5_SetSize(size_arg);
    rom_EdmacSetUnpack(0x3D, rdA);
    rom_EdmacSetPack(0x18, w16);
    uint32_t t0 = get_us_clock();
    rom_m2m5_Start();
    take_semaphore(m2m_sem, 1000);
    int us = get_us_clock() - t0;
    rom_m2m5_ResetCBR();
    rom_m2m5_ClearEdmac();
    return m2m_done ? us : -1;
}

/* one encode of the 16-bit buffer; returns output bytes, -2 on timeout; *us = encode time */
static int crxv_encode(FILE *f, uint8_t *e16, int w, int h, int *us, const char *save)
{
    uint32_t entry[8] = { CRXE_TYPE, w, h, 0, 0, 16, (uint32_t) UNCACHEABLE(e16), w };
    crxe_done = 0; crxe_handle = 0; crxe_late = 0;
    while (take_semaphore_now(crxe_sem) == 0) ;
    uint32_t t0 = get_us_clock();
    uint32_t r = ALGSMGR_StartEncode(entry, 1, crxe_cbr, (void *) 0x43525856);
    if (r & 1) { my_fprintf(f, "  StartEncode %x\n", r); return -1; }
    take_semaphore(crxe_sem, 2000);
    if (!crxe_done) { crxe_late = 1; my_fprintf(f, "  TIMEOUT after 2 s (encoder blocked?)\n"); return -2; }
    *us = (int)(crxe_t_cbr - t0);
    uint32_t hd = crxe_handle;
    if (!hd || (hd & 1)) { my_fprintf(f, "  encoder error %x\n", hd); return -1; }
    int n = 0, total = 0; uint32_t *addrs = 0, *sizes = 0;
    uint32_t g = ALGSMGR_GetRingList(hd, &n, &addrs, &sizes);
    FILE *fo = save ? FIO_CreateFile(save) : 0;
    void *tmp_raw = fo ? fio_malloc(256 * 1024 + 64) : 0;
    void *tmp = tmp_raw ? (void *)(((uintptr_t) tmp_raw + 63) & ~63) : 0;
    if (g == 0 && n > 0 && n <= 64 && crx_ptr_ok((uint32_t) addrs) && crx_ptr_ok((uint32_t) sizes))
        for (int i = 0; i < n; i++)
        {
            uint32_t a = addrs[i], s = sizes[i];
            if (!a || !s || s > 32 * 1024 * 1024 || !crx_ptr_ok(a & ~3)) continue;
            if (fo && tmp)
                for (uint32_t off = 0; off < s; off += 256 * 1024)
                {
                    uint32_t len = MIN(s - off, 256 * 1024);
                    memcpy(tmp, UNCACHEABLE(a + off), len);
                    FIO_WriteFile(fo, tmp, len);
                }
            total += s;
        }
    if (fo) FIO_CloseFile(fo);
    if (tmp_raw) fio_free(tmp_raw);
    ALGSMGR_FreeEncBuffer(hd, 0, 0);
    return total;
}

static void crxv_task()
{
    static uint32_t res_ids[2] = { 0x5001F, 0x50023 };
    static const uint32_t subchips[2] = { 4, 7 };
    void *e_raw = 0; int keep = 0;
    msleep(3000);
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/CRXV.TXT");
    if (!f) { rawdump_running = 0; return; }
    uint32_t am = *(volatile uint32_t *) 0x5a20;
    my_fprintf(f, "=== CRXV movie=%d video_res=%d fps_x100=%d lv=%d am=%x late=%d\n", is_movie_mode(), video_mode_resolution,
               video_mode_fps_x100, lv, am, crxe_late);
    if (!lv || !crx_ptr_ok(am) || ((volatile uint32_t *) am)[5] != 0x3f || crxe_late)
    { my_fprintf(f, "gate failed\n"); NotifyBox(5000, "crxv: gate failed (LiveView, no earlier timeout)"); goto end; }
    my_fprintf(f, "ALGSMGR buffers before: count %d/%d rings %x %x size %x\n", *(volatile uint32_t *) (am + 0x394),
               *(volatile uint32_t *) (am + 0x398), *(volatile uint32_t *) (am + 0x154), *(volatile uint32_t *) (am + 0x1e4),
               *(volatile uint32_t *) (am + 0x274));
    if (!crxe_sem) crxe_sem = create_named_semaphore("crxe_sem", 0);
    raw_lv_request();
    msleep(500);
    int ok = raw_update_params();
    int W = raw_info.width, H = raw_info.height, pitch = raw_info.pitch;
    int ax = raw_info.active_area.x1, ay = raw_info.active_area.y1;
    int aw = raw_info.active_area.x2 - ax, ah = raw_info.active_area.y2 - ay;
    my_fprintf(f, "raw ok=%d buf=%x %dx%d pitch=%d bpp=%d active %d,%d %dx%d\n", ok, raw_info.buffer, W, H, pitch, raw_info.bits_per_pixel, ax, ay, aw, ah);
    if (!ok || raw_info.bits_per_pixel != 14 || pitch != W * 14 / 8 || aw < 1024 || ah < 512)
    { my_fprintf(f, "gate failed: raw geometry\n"); raw_lv_release(); goto end; }

    int win_w[4] = { aw & ~15, 3072, 2560, 1920 };
    int win_h[4] = { ah & ~7, 1296, 1080, 1080 };
    int max_bytes = (aw & ~15) * 2 * (ah & ~7);
    e_raw = fio_malloc(max_bytes + 128);
    if (!e_raw) { my_fprintf(f, "fio_malloc(%d) failed, trying 3072x1296 size\n", max_bytes); max_bytes = 3072 * 2 * 1296; e_raw = fio_malloc(max_bytes + 128); }
    if (!e_raw) { my_fprintf(f, "fio_malloc failed\n"); raw_lv_release(); goto end; }
    uint8_t *e16 = (void *)(((uintptr_t) e_raw + 63) & ~63);
    volatile uint32_t *idflag = (volatile uint32_t *) (am + 0x108 + CRXE_ID * 4);

    for (int k = 0; k < 4; k++)
    {
        int w = win_w[k], h = win_h[k];
        if (w > (aw & ~15) || h > (ah & ~7) || w * 2 * h > max_bytes) { my_fprintf(f, "window %dx%d: skipped (too big)\n", w, h); continue; }
        int x0 = (ax + ((aw - w) / 2)) & ~7, y0 = (ay + ((ah - h) / 2)) & ~1;
        const uint8_t *src = (const uint8_t *) raw_info.buffer + y0 * pitch + x0 * 14 / 8;

        void *lock = rom_CreateResLockEntry(res_ids, 2);
        unsigned int r = lock ? rom_LockEngineResources(lock) : 1;
        if (r & 1) { my_fprintf(f, "lock failed %x\n", r); if (lock) rom_DeleteResLockEntry(lock); break; }
        rom_PwrMngEnableSubChip(subchips);
        uint32_t o_rd24 = MEM(PACK_UNIT_RD61 + 0x24), o_wr24 = MEM(PACK_UNIT_WR24 + 0x24);
        uint32_t o_rdD0 = MEM(M2M_RD_BASE + 0xD0),    o_wrD0 = MEM(M2M_WR_BASE + 0xD0);
        int us16 = m2m_crop16(src, pitch, e16, w, h);
        MEM(PACK_UNIT_RD61 + 0x24) = o_rd24; MEM(PACK_UNIT_WR24 + 0x24) = o_wr24;
        MEM(M2M_RD_BASE + 0xD0) = o_rdD0;    MEM(M2M_WR_BASE + 0xD0) = o_wrD0;
        rom_PwrMngDisableSubChip(subchips);
        rom_UnLockEngineResources(lock); rom_DeleteResLockEntry(lock);
        if (us16 < 0) { my_fprintf(f, "window %dx%d at %d,%d: EDMAC timeout\n", w, h, x0, y0); continue; }

        if (k == 2)
        {
            FILE *fi = FIO_CreateFile("ML/LOGS/CRXV25.RAW");
            if (fi) { FIO_WriteFile(fi, UNCACHEABLE(e16), w * 2 * h); FIO_CloseFile(fi); }
        }
        *idflag = 0;                          /* EncInit with this size */
        int us1 = 0, us2 = 0;
        int out1 = crxv_encode(f, e16, w, h, &us1, k == 2 ? "ML/LOGS/CRXV25.BIN" : 0);
        if (out1 == -2) { keep = 1; break; }
        int out2 = out1 >= 0 ? crxv_encode(f, e16, w, h, &us2, 0) : -1;
        if (out2 == -2) { keep = 1; break; }
        int frame14 = w * h * 14 / 8;
        my_fprintf(f, "window %dx%d at %d,%d: EDMAC16 %d us, encode1 %d us, encode2 %d us, out %d B = %d %% of 14-bit, ALGSMGR count %d\n",
                   w, h, x0, y0, us16, us1, us2, out1, out1 > 0 ? (int)((int64_t) out1 * 100 / frame14) : 0, *(volatile uint32_t *) (am + 0x394));
    }
    *idflag = 0;
    raw_lv_release();
    my_fprintf(f, "ALGSMGR buffers after: count %d/%d rings %x %x\n", *(volatile uint32_t *) (am + 0x394),
               *(volatile uint32_t *) (am + 0x398), *(volatile uint32_t *) (am + 0x154), *(volatile uint32_t *) (am + 0x1e4));
    NotifyBox(8000, keep ? "crxv: TIMEOUT, see CRXV.TXT. Power off." : "crxv: done. ML/LOGS/CRXV.TXT");
end:
    if (e_raw && !keep) fio_free(e_raw);
    FIO_CloseFile(f);
    rawdump_running = 0;
}

static MENU_SELECT_FUNC(crxv_start)
{
    if (rawdump_running) return;
    rawdump_running = 1;
    task_create("crxv_task", 0x1e, 0x4000, crxv_task, 0);
}

static struct menu_entry rawdump_menu[] =
{
    {
        .name   = "Sensor mode info (read)",
        .select = smi_start,
        .help   = "Read only. Run in photo M, movie FHD 23.976 and photo x5. Close menu in 3 s.",
        .help2  = "Logs Canon seq mode, SensorMode, sensor lists, raw size. ML/LOGS/SMODE.TXT",
    },
    {
        .name   = "SD card info (read)",
        .select = sdi_start,
        .help   = "Read only. SD driver state for the card speed research. ML/LOGS/SDINFO.TXT",
    },
    {
        .name   = "OG3 ARM rows only",
        .select = og3arm_r_start,
        .help   = "Movie FHD 23.98. Sensor window full height (accum 68=e7f5, 93=2a5), capture stays 1164.",
        .help2  = "Then movie size 4K, then FHD 23.98. Reboot or DISARM undoes it.",
    },
    {
        .name   = "OG3 ARM in 4K rows",
        .select = og3arm_4kr_start,
        .help   = "Movie 4K 23.98. Sensor full window for the next FHD (capture stays 1164).",
        .help2  = "Then movie size FHD 23.98 only. Reboot undoes it.",
    },
    {
        .name   = "OG3 ARM in 4K lists",
        .select = og3arm_4kl_start,
        .help   = "Movie 4K 23.98. Sensor full window + Canon TG 1372 + front-end 1370 lists for the next FHD.",
        .help2  = "Then movie size FHD 23.98 only, then 'OG3 LIVE height 1370'. Reboot undoes it.",
    },
    {
        .name   = "OG3 ARM in 4K lists+ROM",
        .select = og3arm_4kx_start,
        .help   = "Movie 4K 23.98. Like 'lists' + Canon ROM TG list (lines 1372) for the next FHD.",
        .help2  = "Then movie size FHD 23.98 only, then 'OG3 LIVE height 1370'. Reboot undoes it.",
    },
    {
        .name   = "DISO test ON (ISO 1600 lines)",
        .select = diso_start,
        .help   = "Dual ISO test: half the lines at ISO 1600. Movie M, ISO 100. Reboot undoes it.",
        .help2  = "Then ISO 200 -> 100 in Canon, then Sensor mode info (DNG). ML/LOGS/OG3.TXT",
    },
    {
        .name   = "DISO OFF",
        .select = disooff_start,
        .help   = "Undo the Dual ISO test (or just reboot).",
    },
    {
        .name   = "OG3 LIVE TG probe",
        .select = og3livetg_start,
        .help   = "After 'OG3 ARM rows only' in FHD 23.98. Full 1370 live + TG b8 test, measures frame length.",
        .help2  = "No Canon size change. ML/LOGS/OG3.TXT + OG3T.DNG",
    },
    {
        .name   = "OG3 LIVE full 1370",
        .select = og3livefe_start,
        .help   = "After 'OG3 ARM rows only' in FHD 23.98. Front-end + capture 1370 rows live, snap, back.",
        .help2  = "No Canon size change. ML/LOGS/OG3.TXT + OG3F.DNG",
    },
    {
        .name   = "OG3 LIVE height 1370",
        .select = og3live_start,
        .help   = "After 'OG3 ARM rows only' in FHD 23.98. Capture 1370 rows live, snap, back to 1164.",
        .help2  = "No Canon size change. ML/LOGS/OG3.TXT + OG3L.DNG",
    },
    {
        .name   = "TG info (read)",
        .select = tgi_start,
        .help   = "Read only. DIGIC timing registers + Canon's per-mode TG lists in RAM.",
        .help2  = "Run in movie FHD 23.98 and in movie 4K 23.98. ML/LOGS/TGINFO.TXT",
    },
    {
        .name   = "OG3 DISARM",
        .select = og3disarm_start,
        .help   = "Turns the OG3 changes OFF. Then movie size 4K and FHD again.",
        .help2  = "ML/LOGS/OG3.TXT",
    },
    {
        .name   = "Movie full height AUTO (WRITE test)",
        .select = rpa_start,
        .help   = "Movie FHD 23.98, plain lit wall out of focus. RAM 2c=256, LV restart, snaps.",
        .help2  = "Don't touch the camera ~30 s. Canon value back at the end. ML/LOGS/RPA.TXT + 3 DNG",
    },
    {
        .name   = "Movie full height RAM patch",
        .select = rp_start,
        .help   = "Movie FHD 23.98 only. Changes Canon's RAM sensor list (2c=256). Sends nothing.",
        .help2  = "Then Playback and back to LV. Reboot undoes it. ML/LOGS/RPATCH.TXT",
    },
    {
        .name   = "CRX encode in movie LV (CALL test)",
        .select = crxv_start,
        .help   = "mlv_lite RAW video OFF. Any LV mode (movie FHD, 4K). Close menu in 3 s.",
        .help2  = "Times EDMAC + encode for full / 3K / 2.5K / 1080 windows. ML/LOGS/CRXV.TXT",
    },
    {
        .name   = "CRX 12/10-bit encode (CALL test)",
        .select = crxd_start,
        .help   = "Photo M, LiveView, not recording, fresh boot. Close menu in 3 s. Same frame",
        .help2  = "as 12-bit and 10-bit, then encoded. ML/LOGS/CRXD.TXT, CRXD12/10.RAW, CRXD*_0/3.BIN",
    },
    {
        .name   = "Card write + console log (test)",
        .select = cwc_start,
        .help   = "Photo M, LiveView, not recording. Close menu in 3 s. About 2.5 min, 3 steps:",
        .help2  = "6 GB written, steps 2 and 3 also print to the console log. ML/LOGS/CWCON.TXT",
    },
    {
        .name   = "Card write + all memory (test)",
        .select = cwm_start,
        .help   = "Photo M, LiveView, not recording. Close menu in 3 s. About 3 min, 3 steps:",
        .help2  = "6 GB written while all shoot memory is held like REC. ML/LOGS/CWMEM.TXT",
    },
    {
        .name   = "Card write + recording load (test)",
        .select = cwl_start,
        .help   = "Photo M, LiveView, not recording. Close menu in 3 s. About 4.5 min, 5 steps:",
        .help2  = "10 GB written at 40 MB/s while recording work is added step by step. ML/LOGS/CWLOAD.TXT",
    },
    {
        .name   = "Card write 8 GB (test)",
        .select = cwt_start,
        .help   = "Photo M, LiveView, not recording. Close menu in 3 s. About 3.5 min: 8 GB",
        .help2  = "written at 40 MB/s, every write timed. Files CWT0-3.BIN. ML/LOGS/CWTEST.TXT",
    },
    {
        .name   = "CRX rounded bits 0-3 (CALL test)",
        .select = crxr_start,
        .help   = "Photo M, LiveView, not recording, fresh boot. Close menu in 3 s. Same frame",
        .help2  = "encoded 4 ways (field 0-3). ML/LOGS/CRXR.TXT, CRXR16.RAW, CRXR0-3.BIN",
    },
    {
        .name   = "CRX memory + 48-frame burst (CALL test)",
        .select = crxmem_start,
        .help   = "Photo M, LiveView, not recording, fresh boot. Close menu in 3 s. Encoder",
        .help2  = "memory vs shoot memory + 48 compressed frames timing. ML/LOGS/CRXMEM.TXT",
    },
    {
        .name   = "CRX 16-bit EDMAC encode (CALL test)",
        .select = crx16_start,
        .help   = "Photo M, LiveView, not recording, fresh boot. Close menu in 3 s. EDMAC",
        .help2  = "14->16-bit + Canon CR3 encoder at 16-bit. ML/LOGS/CRX16.TXT + .RAW/.BIN",
    },
    {
        .name   = "CRX encode 1 frame (CALL test)",
        .select = crxenc_start,
        .help   = "Photo M, LiveView, not recording. Close menu in 3 s. Canon's CR3 encoder",
        .help2  = "compresses 1 LV RAW frame -> ML/LOGS/CRXENC.TXT, CRXIN.RAW, CRXOUT.BIN",
    },
    {
        .name   = "CRX encoder state (read)",
        .select = crxst_start,
        .help   = "Photo M, image quality RAW. Close menu, then take ONE photo within 30 s.",
        .help2  = "READ-ONLY: Canon's CR3/CRX encoder RAM state. ML/LOGS/CRXST.TXT",
    },
    {
        .name   = "EDMAC pack 12/10-bit (test)",
        .select = packtest_start,
        .help   = "Close menu within 10 s (movie mode, LV on). Copies a 1920x64 LV RAW",
        .help2  = "block on ch61/24 with Pack 14->12/10, checks vs CPU. PACKTEST.TXT",
    },
    {
        .name   = "Evf state info (read)",
        .select = evfinfo_start,
        .help   = "Close menu within 3 s (LiveView on). READ-ONLY: dumps Canon's",
        .help2  = "Evf state object at 0x6cec. ML/LOGS/EVFSTATE.TXT.",
    },
    {
        .name   = "VD table 24p (WRITE test)",
        .select = vdpatch_start,
        .help   = "Photo M, Photo mode LV FPS OFF. Close menu in 3 s. ~8 s: Canon's photo",
        .help2  = "frame-length entry 26f -> 617, measure fps/RAW, restore. ML/LOGS/VDPATCH.TXT",
    },
    {
        .name   = "VD table (read)",
        .select = vdtab_start,
        .help   = "Close menu within 3 s. READ-ONLY: Canon's frame-length table (mode x rate).",
        .help2  = "Photo M once is enough. ML/LOGS/VDTAB.TXT",
    },
    {
        .name   = "CMOS DMA buffer (read)",
        .select = cmosdma_start,
        .help   = "Close menu within 3 s. READ-ONLY: Canon's per-frame sensor DMA buffer.",
        .help2  = "Movie 24p + photo M. ML/LOGS/CMOSDMA.TXT",
    },
    {
        .name   = "CMOS 24p sensor (WRITE test)",
        .select = cmos24_start,
        .help   = "Photo M + Photo mode LV FPS 23.976. Close menu in 3 s. ~9 s: sends sensor",
        .help2  = "VSize 1560 + shutter 180/90/270 deg, meters RAW, restores. ML/LOGS/CMOS24.TXT",
    },
    {
        .name   = "CMOS lists (read)",
        .select = cmosl_start,
        .help   = "Close menu within 3 s. READ-ONLY: Canon's sensor CMOS register lists",
        .help2  = "(VSize 0x50, shutter 0x29). Movie 24p + photo M. ML/LOGS/CMOSL.TXT",
    },
    {
        .name   = "TG 15F0 exposure (WRITE test)",
        .select = tgw_start,
        .help   = "Photo M, ISO 800, 23.976. Close menu in 3 s. ~6 s: writes 0xD04115F0/F4,",
        .help2  = "measures RAW brightness per value, then Canon's values again. ML/LOGS/TGW.TXT",
    },
    {
        .name   = "Sensor regs snapshot (read)",
        .select = tgsnap_start,
        .help   = "Close menu within 3 s. READ-ONLY: 655 TG/SSG registers from Canon's tables.",
        .help2  = "Photo M f/4 + f/16 (FPS OFF), movie 24p, photo 23.976. ML/LOGS/TGSNAP.TXT",
    },
    {
        .name   = "FrameData timing (read)",
        .select = fdlog_start,
        .help   = "Close menu within 3 s. READ-ONLY: logs Canon FrameData (ISO/Acc) at both",
        .help2  = "LV hooks for ~48 frames. Movie 24p + photo M ISO 100/800. ML/LOGS/FRAMEDAT.TXT",
    },
    {
        .name   = "FPS 24p + VD + BaseFR (WRITE test)",
        .select = fps24fr_start,
        .help   = "Photo M, mlv_lite FPS OFF, ISO 100. Close menu in 3 s. 5 s at 24p, restored.",
        .help2  = "WRITES timer B + VSize/VD + ESync rate (RAM 0x17730 = 2). ML/LOGS/FPS24.TXT.",
    },
    {
        .name   = "FPS 24p + VD fix (WRITE test)",
        .select = fps24vd_start,
        .help   = "Photo M, mlv_lite FPS OFF, low ISO. Close menu in 3 s. 5 s at 24p, restored.",
        .help2  = "WRITES timer B + Canon VSize/VD (RAM, movie 24p values). ML/LOGS/FPS24.TXT.",
    },
    {
        .name   = "Sensor VD/AC RAM read (test)",
        .select = fpsram_start,
        .help   = "Close menu within 3 s. Movie 24/30/60p, photo M at low + high ISO.",
        .help2  = "READ-ONLY (RAM 0xef24, 0xa3594, 0x123c8). Appends ML/LOGS/FPSRAM2.TXT.",
    },
    {
        .name   = "FPS 24p photo LV (WRITE test)",
        .select = fps24_start,
        .help   = "Photo mode M only. Close menu within 3 s. ~2 s at 24p, then restored.",
        .help2  = "WRITES timer B 0xD04061A4 + confirm. ML/LOGS/FPS24.TXT.",
    },
    {
        .name   = "FPS timer read (test)",
        .select = fpsreg_start,
        .help   = "Close menu within 3 s. Run once in movie 24p, 30p, 60p and photo M.",
        .help2  = "READ-ONLY (0xD0406180..E0, 0xD0406000..38). Appends to ML/LOGS/FPSREG2.TXT.",
    },
    {
        .name   = "Mode info (read)",
        .select = modeinfo_start,
        .help   = "Close menu within 3 s, then turn the dial M -> movie -> M",
        .help2  = "within 20 s. READ-ONLY. ML/LOGS/MODEINFO.TXT.",
    },
    {
        .name   = "LV RAW dump (test)",
        .select = rawdump_start,
        .help   = "Close the menu within 10 s. Saves one LiveView RAW frame",
        .help2  = "with Canon's lv_raw_dump. File name = raw buffer address.",
    },
    {
        .name   = "LV RAW info (test)",
        .select = rawinfo_start,
        .help   = "Close the menu within 10 s. Reads Canon's LV RAW struct",
        .help2  = "10x (read-only) into ML/LOGS/RAWINFO.TXT.",
    },
    {
        .name   = "ML RAW -> DNG (test)",
        .select = rawsnap_start,
        .help   = "Close the menu within 10 s. Uses ML's raw.c to read the frame",
        .help2  = "and saves ML/LOGS/RAWSNAP.DNG + RAWSNAP.TXT.",
    },
    {
        .name   = "EDMAC port 3 read (test)",
        .select = edmacinfo_start,
        .help   = "Close the menu within 10 s. READ-ONLY: reads LV RAW EDMAC",
        .help2  = "port 3 (0xD0420200) registers 20x into ML/LOGS/EDMAC3.TXT.",
    },
    {
        .name   = "EDMAC free channel scan",
        .select = edmacscan_start,
        .help   = "Close menu within 10 s, then 30 s scan: start+stop a video rec",
        .help2  = "inside it. READ-ONLY. Result: ML/LOGS/EDMACSCN.TXT (FREE/used).",
    },
    {
        .name   = "M2M ch24/61 check (read)",
        .select = m2mcheck_start,
        .help   = "Close menu within 10 s. Locks Canon m2m resources, wakes chip 4,",
        .help2  = "READS ch24/61 regs 20x, then releases. ML/LOGS/M2MCHK.TXT.",
    },
    {
        .name   = "M2M DMA copy (test)",
        .select = m2mcopy_start,
        .help   = "Close menu within 10 s. First EDMAC copy RAM->RAM on ch61/24",
        .help2  = "(Canon's own recipe). 64KB then 4MB x5. ML/LOGS/M2MCOPY.TXT.",
    },
    {
        .name   = "DMA RAW burst 12f (test)",
        .select = dmaburst_start,
        .help   = "Close menu within 10 s. 12 LV RAW frames copied by EDMAC at 24p,",
        .help2  = "saved as DNG in ML/RAWVID2. Log: ML/LOGS/DMABURST.TXT.",
    },
    {
        .name   = "Core DMA burst 12f (test)",
        .select = coreburst_start,
        .help   = "Same as DMA RAW burst, through the core edmac API (edmac-d8.c),",
        .help2  = "plus a crop copy checked vs CPU. Log ML/LOGS/DMABURST.TXT.",
    },
    {
        .name   = "RAW video burst 8f (test)",
        .select = rawburst_start,
        .help   = "Close the menu within 10 s. Records 8 frames to RAM,",
        .help2  = "then saves a DNG sequence to ML/RAWVID + ML/LOGS/BURST.TXT.",
    },
};

static unsigned int rawdump_init()
{
    menu_add("Debug", rawdump_menu, COUNT(rawdump_menu));
    return 0;
}

static unsigned int rawdump_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(rawdump_init)
    MODULE_DEINIT(rawdump_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_VSYNC_SETPARAM, fps24_setparam_cbr, 0)
    MODULE_CBR(CBR_VSYNC, fps24_vsync_cbr, 0)
MODULE_CBRS_END()
