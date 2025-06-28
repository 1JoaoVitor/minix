/* This file contains the SPN (Shortest Process Next) scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 *   socorro não aguento mais
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include <minix/syslib.h>    /* Para sys_times */

#include <sys/times.h>       /* Para clock_t */

static unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */
#define BURST_ESTIMATION_ALPHA 50 /* Alpha parameter for exponential averaging (0-100) */
#define DEFAULT_BURST_ESTIMATE 100 /* Default burst time estimate for new processes */
#define MIN_BURST_ESTIMATE 10 /* Minimum burst estimate to prevent starvation */
#define MAX_BURST_ESTIMATE 1000 /* Maximum burst estimate */

static int schedule_process(struct schedproc * rmp, unsigned flags);

#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4

#define SCHEDULE_CHANGE_ALL	(	\
		SCHEDULE_CHANGE_PRIO	|	\
		SCHEDULE_CHANGE_QUANTUM	|	\
		SCHEDULE_CHANGE_CPU		\
		)

#define schedule_process_local(p)	\
	schedule_process(p, SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)
#define schedule_process_migrate(p)	\
	schedule_process(p, SCHEDULE_CHANGE_CPU)

#define CPU_DEAD	-1

#define cpu_is_available(c)	(cpu_proc[c] >= 0)

#define DEFAULT_USER_TIME_SLICE 200

/* processes created by RS are system processes */
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)

static unsigned cpu_proc[CONFIG_MAX_CPUS];

/* SPN-specific functions */
static void update_burst_estimate(struct schedproc *rmp, clock_t actual_burst);
static int calculate_spn_priority(struct schedproc *rmp);
static void sort_ready_processes(void);

static clock_t get_system_time(void)
{
    clock_t uptime;
    if (sys_times(NONE, &uptime, NULL, NULL, NULL) != OK) {
        return 0;
    }
    return uptime;
}

static void pick_cpu(struct schedproc * proc)
{
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;
	
	if (machine.processors_count == 1) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* schedule system processes only on the boot cpu */
	if (is_system_proc(proc)) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* if no other cpu available, try BSP */
	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		/* skip dead cpus */
		if (!cpu_is_available(c))
			continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) {
			cpu_load = cpu_proc[c];
			cpu = c;
		}
	}
	proc->cpu = cpu;
	cpu_proc[cpu]++;
#else
	proc->cpu = 0;
#endif
}

/*===========================================================================*
 *				update_burst_estimate			     *
 *===========================================================================*/
static void update_burst_estimate(struct schedproc *rmp, clock_t actual_burst)
{
	/* Exponential averaging: new_estimate = alpha * actual + (1-alpha) * old_estimate */
	/* Using integer arithmetic to avoid floating point */
	int alpha = BURST_ESTIMATION_ALPHA;
	int new_estimate;
	
	if (actual_burst <= 0) {
		actual_burst = 1; /* Avoid zero or negative values */
	}
	
	new_estimate = (alpha * actual_burst + (100 - alpha) * rmp->burst_estimate) / 100;
	
	/* Ensure estimate stays within reasonable bounds */
	if (new_estimate < MIN_BURST_ESTIMATE) {
		new_estimate = MIN_BURST_ESTIMATE;
	} else if (new_estimate > MAX_BURST_ESTIMATE) {
		new_estimate = MAX_BURST_ESTIMATE;
	}
	
	rmp->burst_estimate = new_estimate;
}

/*===========================================================================*
 *				calculate_spn_priority			     *
 *===========================================================================*/
static int calculate_spn_priority(struct schedproc *rmp)
{
	/* For SPN, lower burst estimate = higher priority (lower priority number) */
	/* Map burst estimate to priority queue */
	int priority;
	
	/* System processes get highest priority */
	if (is_system_proc(rmp)) {
		return rmp->max_priority; /* Keep system process priority */
	}
	
	/* Map burst estimate to priority levels */
	if (rmp->burst_estimate <= 20) {
		priority = USER_Q; /* Highest user priority */
	} else if (rmp->burst_estimate <= 50) {
		priority = USER_Q + 1;
	} else if (rmp->burst_estimate <= 100) {
		priority = USER_Q + 2;
	} else if (rmp->burst_estimate <= 200) {
		priority = USER_Q + 3;
	} else {
		priority = MIN_USER_Q; /* Lowest user priority */
	}
	
	/* Ensure we don't exceed queue limits */
	if (priority > MIN_USER_Q) {
		priority = MIN_USER_Q;
	}
	
	return priority;
}

/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/
int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;
	clock_t actual_burst;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];

	/* Calculate actual burst time (time since last scheduling) */
	actual_burst = rmp->time_slice; /* Process used full quantum */
	
	/* Update burst estimate based on actual usage */
	update_burst_estimate(rmp, actual_burst);
	
	/* Recalculate priority based on new burst estimate */
	if (!is_system_proc(rmp)) {
		rmp->priority = calculate_spn_priority(rmp);
	}

	/* Record start time for next burst measurement */
	rmp->last_start_time = get_system_time();

	if ((rv = schedule_process_local(rmp)) != OK) {
		return rv;
	}
	return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int proc_nr_n;
	clock_t current_time, actual_burst;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->m_lsys_sched_scheduling_stop.endpoint,
		    &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%d\n", m_ptr->m_lsys_sched_scheduling_stop.endpoint);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	
	/* Update burst estimate one final time */
	current_time = get_system_time();
	if (rmp->last_start_time > 0) {
		actual_burst = current_time - rmp->last_start_time;
		if (actual_burst > 0) {
			update_burst_estimate(rmp, actual_burst);
		}
	}
	
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0; /*&= ~IN_USE;*/

	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
int do_start_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->m_lsys_sched_scheduling_start.endpoint,
			&proc_nr_n)) != OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint     = m_ptr->m_lsys_sched_scheduling_start.endpoint;
	rmp->parent       = m_ptr->m_lsys_sched_scheduling_start.parent;
	rmp->max_priority = m_ptr->m_lsys_sched_scheduling_start.maxprio;
	
	/* Initialize SPN-specific fields */
	rmp->burst_estimate = DEFAULT_BURST_ESTIMATE;
	rmp->last_start_time = get_system_time();
	
	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Inherit current priority and time slice from parent. Since there
	 * is currently only one scheduler scheduling the whole system, this
	 * value is local and we assert that the parent endpoint is valid */
	if (rmp->endpoint == rmp->parent) {
		/* We have a special case here for init, which is the first
		   process scheduled, and the parent of itself. */
		rmp->priority   = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;

		/*
		 * Since kernel never changes the cpu of a process, all are
		 * started on the BSP and the userspace scheduling hasn't
		 * changed that yet either, we can be sure that BSP is the
		 * processor where the processes run now.
		 */
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
		/* FIXME set the cpu mask */
#endif
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quantum and priority are set explicitly rather than inherited 
		 * from the parent */
		rmp->priority   = rmp->max_priority;
		rmp->time_slice = m_ptr->m_lsys_sched_scheduling_start.quantum;
		/* System processes keep their original priority, not SPN */
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->m_lsys_sched_scheduling_start.parent,
				&parent_nr_n)) != OK)
			return rv;

		/* For user processes, calculate priority based on SPN */
		if (!is_system_proc(rmp)) {
			/* Inherit burst estimate from parent or use default */
			if (!is_system_proc(&schedproc[parent_nr_n])) {
				rmp->burst_estimate = schedproc[parent_nr_n].burst_estimate;
			}
			rmp->priority = calculate_spn_priority(rmp);
		} else {
			rmp->priority = schedproc[parent_nr_n].priority;
		}
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	pick_cpu(rmp);
	while ((rv = schedule_process(rmp, SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		/* don't try this CPU ever again */
		cpu_proc[rmp->cpu] = CPU_DEAD;
		pick_cpu(rmp);
	}

	if (rv != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into the "scheduler" field.
	 */

	m_ptr->m_sched_lsys_scheduling_start.scheduler = SCHED_PROC_NR;

	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
int do_nice(message *m_ptr)
{
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->m_pm_sched_scheduling_set_nice.endpoint, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OoQ msg "
		"%d\n", m_ptr->m_pm_sched_scheduling_set_nice.endpoint);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	new_q = m_ptr->m_pm_sched_scheduling_set_nice.maxprio;
	
	if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Store old values, in case we need to roll back the changes */
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;

	/* Update the proc entry and reschedule the process */
	rmp->max_priority = new_q;
	
	/* For user processes, recalculate priority based on SPN */
	if (!is_system_proc(rmp)) {
		rmp->priority = calculate_spn_priority(rmp);
	} else {
		rmp->priority = new_q;
	}

	if ((rv = schedule_process_local(rmp)) != OK) {
		/* Something went wrong when rescheduling the process, roll
		 * back the changes to proc struct */
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
	}

	return rv;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
static int schedule_process(struct schedproc * rmp, unsigned flags)
{
	int err;
	int new_prio, new_quantum, new_cpu, niced;

	pick_cpu(rmp);

	if (flags & SCHEDULE_CHANGE_PRIO)
		new_prio = rmp->priority;
	else
		new_prio = -1;

	if (flags & SCHEDULE_CHANGE_QUANTUM)
		new_quantum = rmp->time_slice;
	else
		new_quantum = -1;

	if (flags & SCHEDULE_CHANGE_CPU)
		new_cpu = rmp->cpu;
	else
		new_cpu = -1;

	niced = (rmp->max_priority > USER_Q);

	if ((err = sys_schedule(rmp->endpoint, new_prio,
		new_quantum, new_cpu, niced)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, err);
	}

	return err;
}

/*===========================================================================*
 *				init_scheduling				     *
 *===========================================================================*/
void init_scheduling(void)
{
	int r;

	balance_timeout = BALANCE_TIMEOUT * sys_hz();

	if ((r = sys_setalarm(balance_timeout, 0)) != OK)
		panic("sys_setalarm failed: %d", r);
}

/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function is called every N ticks to rebalance the queues for SPN.
 * It updates burst estimates and recalculates priorities for all processes.
 */
void balance_queues(void)
{
	struct schedproc *rmp;
	int r, proc_nr;
	clock_t current_time;

	current_time = get_system_time();

	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			/* For user processes, recalculate priority based on current burst estimate */
			if (!is_system_proc(rmp)) {
				/* Age the burst estimate slightly to prevent starvation */
				if (rmp->burst_estimate > MIN_BURST_ESTIMATE) {
					rmp->burst_estimate = (rmp->burst_estimate * 95) / 100;
				}
				
				/* Recalculate priority */
				int old_priority = rmp->priority;
				rmp->priority = calculate_spn_priority(rmp);
				
				/* Only reschedule if priority changed */
				if (rmp->priority != old_priority) {
					schedule_process_local(rmp);
				}
			}
		}
	}

	if ((r = sys_setalarm(balance_timeout, 0)) != OK)
		panic("sys_setalarm failed: %d", r);
}
