/* This is the master header for the Scheduler.  It includes some other files
 * and defines the principal constants.
 */
#define _SYSTEM		1	/* tell headers that this is the kernel */

#define BURST_ESTIMATION_ALPHA 50   /* Alpha parameter for exponential averaging (0-100) */
#define DEFAULT_BURST_ESTIMATE 100  /* Default burst time estimate for new processes */
#define MIN_BURST_ESTIMATE 10       /* Minimum burst estimate to prevent starvation */
#define MAX_BURST_ESTIMATE 1000     /* Maximum burst estimate */

/* Se estas constantes não existirem, adicione também: */
#ifndef USER_Q
#define USER_Q 12                   /* First user queue */
#endif

#ifndef MIN_USER_Q  
#define MIN_USER_Q 14               /* Last user queue */
#endif

/* The following are so basic, all the *.c files get them automatically. */
#include <minix/config.h>	/* MUST be first */
#include <sys/types.h>
#include <minix/const.h>

#include <minix/syslib.h>
#include <minix/sysutil.h>

#include <errno.h>

#include "proto.h"

extern struct machine machine;		/* machine info */
