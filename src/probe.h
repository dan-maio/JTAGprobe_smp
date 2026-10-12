#ifndef PROBE_H_
#define PROBE_H_

#include <cmsis-plus/rtos/os-c-api.h>
#include "board_pico_config.h"
#include "DAP_config.h"
#include "DAP.h"

#if false
#define probe_info(format,args...) \
do { \
	os_sched_lock(); \
	printf(format, ## args); \
	os_sched_unlock(); \
} while (0)
#else
#define probe_info(format,...) ((void)0)
#endif


#if false
#define probe_debug(format,args...) \
do { \
	os_sched_lock(); \
	printf(format, ## args); \
	os_sched_unlock(); \
} while (0)
#else
#define probe_debug(format,...) ((void)0)
#endif

#if false
#define probe_dump(format,args...)\
do { \
	os_sched_lock(); \
	printf(format, ## args); \
	os_sched_unlock(); \
} while (0)
#else
#define probe_dump(format,...) ((void)0)
#endif


// Add the configuration to binary information
void bi_decl_config();

#define PROTO_DAP_V1 1
#define PROTO_DAP_V2 2

// Interface config
#ifndef PROBE_DEBUG_PROTOCOL
#define PROBE_DEBUG_PROTOCOL PROTO_DAP_V2
#endif

#endif