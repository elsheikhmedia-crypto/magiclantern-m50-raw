/**
 * Camera internals for EOS M50 1.1.0
 */

/** This camera has a DIGIC VIII chip */
#define CONFIG_DIGIC_VIII

// has inter-core RPC (so far this has always been dependent on SGI, 0xc)
#define CONFIG_RPC

// Cam has MMU (by itself, does nothing, see CONFIG_MMU_REMAP)
#define CONFIG_MMU

/** Digic 8 does not have bitmap font in ROM, try to load it from card **/
#define CONFIG_NO_BFNT

/** disable SRM for now
 * in current state SRM_AllocateMemoryResourceFor1stJob makes camera crash
 * even if just one buffer is requrested.
 */
#define CONFIG_MEMORY_SRM_NOT_WORKING

/* has LV */
#define CONFIG_LIVEVIEW

/* EDMAC mem2mem copy via Canon's MemoryToMemoryEsub5 path, see edmac-d8.c */
#define CONFIG_EDMAC_MEMCPY_M50

/* movie mode shows up as PROP_SHOOTING_MODE = 0x14 (and lv_movie_select = 2);
 * PROP_SHOOTING_MODE_2 stays at the movie exposure mode. Logged on camera, MODEINFO.TXT */
#define CONFIG_MOVIE_MODE_IN_SHOOTING_MODE_CUSTOM

/* fps_get_current_x1000() from PROP_VIDEO_MODE (fps x100), FPS registers not found yet */
#define CONFIG_FPS_FROM_VIDEO_MODE

/* copy every console line to ML/LOGS/CONSOLE.LOG (RAM buffer, flushed by console_task) */
#define CONFIG_CONSOLE_LOG_FILE

/* Evf state object hooks for vsync (mlv_lite CBR_VSYNC). Checked on camera:
 * *(0x6cec) = StateObject "EvfState", 16 inputs x 8 states, matrix 0xE09DDE74 in ROM;
 * in state 5, input 5 = evfReadOutDoneInterrupt, input 4 = evfSetParamInterrupt.
 * The old EvfCap crash: the hook was installed before EvfState existed, so the spy
 * is now installed late, once the object exists (CONFIG_EVF_STATE_LATE_HOOK). */
#define CONFIG_STATE_OBJECT_HOOKS
#define CONFIG_EVF_STATE_SYNC
#define CONFIG_EVF_STATE_LATE_HOOK

#define CONFIG_MALLOC_STRUCT_V2

#define CONFIG_TASK_STRUCT_V2_SMP
#define CONFIG_TASK_ATTR_STRUCT_V5
