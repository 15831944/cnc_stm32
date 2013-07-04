/*
 * spiffs_config.h
 *
 *  Created on: Jul 3, 2013
 *      Author: petera
 */

#ifndef SPIFFS_CONFIG_H_
#define SPIFFS_CONFIG_H_

#include "system.h"
#include "miniutils.h"
#include "os.h"

extern os_mutex fs_mutex;

// compile time switches

#define SPIFFS_DBG(...) DBG(D_FS, D_DEBUG, __VA_ARGS__)
#define SPIFFS_GC_DBG(...) DBG(D_FS, D_DEBUG, __VA_ARGS__)
#define SPIFFS_CACHE_DBG(...) DBG(D_FS, D_DEBUG, __VA_ARGS__)

// define maximum number of gc runs to perform to reach desired free pages
#define SPIFFS_GC_MAX_RUNS              3
// enable/disable statistics on gc
#define SPIFFS_GC_STATS                 1

// enables/disable memory read caching of nucleus file system operations
// if enabled, memory area must be provided for cache in SPIFFS_init
#define SPIFFS_CACHE                    1
#if SPIFFS_CACHE
// enables memory write caching for file descriptors in hydrogen
#define SPIFFS_CACHE_WR                 1
// enable/disable statistics on caching
#define SPIFFS_CACHE_STATS              0
#endif

// checks header of each accessed page to validate state
#define SPIFFS_PAGE_CHECK               1

// Garbage collecting examines all pages in a block which and sums up
// to a block score. Deleted pages normally gives positive score and
// used pages normally gives a negative score (as these must be moved).
// The larger the score, the more likely it is that the block will
// picked for garbage collection.

// garbage collecting heuristics - weight used for deleted pages
#define SPIFFS_GC_HEUR_W_DELET          (10)
// garbage collecting heuristics - weight used for used pages
#define SPIFFS_GC_HEUR_W_USED           (-1)

// object name length
#define SPIFFS_OBJ_NAME_LEN (32 - sizeof(spiffs_obj_type))

// size of buffer on stack used when copying data
#define SPIFFS_COPY_BUFFER_STACK        (64)

#define SPIFFS_LOCK(fs)   do {OS_mutex_lock(&fs_mutex);  } while(0);
#define SPIFFS_UNLOCK(fs) do {OS_mutex_unlock(&fs_mutex);} while(0);

// types and constants

// spiffs file descriptor index type
typedef s16_t spiffs_file;
// spiffs file descriptor attributes
typedef u32_t spiffs_attr;
// spiffs file descriptor mode
typedef u32_t spiffs_mode;

// block index type
typedef u16_t spiffs_block_ix; // (address-phys_addr) / block_size
// page index type
typedef u16_t spiffs_page_ix;  // (address-phys_addr) / page_size
// object id type - most significant bit is reserved for index flag
typedef u16_t spiffs_obj_id;
// object span index type
typedef u16_t spiffs_span_ix;
// object type
typedef u8_t spiffs_obj_type;

// enable if only one spiffs instance with constant configuration will exist
// on the system
#define SPIFFS_SINGLETON

#ifdef SPIFFS_SINGLETON
// instead of giving parameters in config struct, singleton build must
// give parameters in defines below
#define SPIFFS_CFG_PHYS_SZ(ignore)        (1024*1024*1)
#define SPIFFS_CFG_PHYS_ERASE_SZ(ignore)  (65536)
#define SPIFFS_CFG_PHYS_ADDR(ignore)      (0)
#define SPIFFS_CFG_LOG_PAGE_SZ(ignore)    (256)
#define SPIFFS_CFG_LOG_BLOCK_SZ(ignore)   (65536)
#endif




#endif /* SPIFFS_CONFIG_H_ */
