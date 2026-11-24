/* Lossless raw compression */

#ifndef _lossless_h_
#define _lossless_h_

/* to be called before anything else; 1 = success */
int lossless_init();

/* returns output size if successful, negative on error */
int lossless_compress_raw(struct raw_info * raw_info, struct memSuite * output_memsuite);

/* compress a rectangle cropped from a source buffer */
/* similar to edmac_copy_rectangle */
int lossless_compress_raw_rectangle(
    struct memSuite * dst_suite, void * src,
    int src_width, int src_x, int src_y,
    int width, int height
);

int lossless_decompress_raw(
    struct memSuite * src, void * dst,
    int width, int height,
    int output_bpp
);


/* M50 CRX backend: call on recording start / stop (no-ops elsewhere) */
void lossless_rec_start();
void lossless_rec_stop();
extern int lossless_m50_last_us[4];
int lossless_m50_prealloc();   /* 1 = encoder input buffer available */
int lossless_ready();          /* 0 only on M50 without that buffer */
extern int lossless_m50_round_field;   /* M50: 1 = exact, 3 = 13-bit compact */
extern int lossless_m50_mode;          /* M50: 0 = 14-bit, 1 = 12-bit lossless, 2 = 10-bit */
int lossless_m50_extra_regions(int width, int height, uint32_t *addr, int *size);   /* up to 2 regions */

#endif
