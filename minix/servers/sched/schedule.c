#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>

static unsigned balance_timeout;
#define BALANCE_TIMEOUT	5
static int schedule_process(struct schedproc * rmp, unsigned flags);
#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4
#define SCHEDULE_CHANGE_ALL (SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM | SCHEDULE_CHANGE_CPU)
#define schedule_process_local(p) schedule_process(p, SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)
#define schedule_process_migrate(p) schedule_process(p, SCHEDULE_CHANGE_CPU)
#define CPU_DEAD -1
#define cpu_is_available(c)	(cpu_proc[c] >= 0)
#define DEFAULT_USER_TIME_SLICE 200
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)
static unsigned cpu_proc[CONFIG_MAX_CPUS];
static void pick_cpu(struct schedproc * proc) {
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;
	if (machine.processors_count == 1) { proc->cpu = machine.bsp_id; return; }
	if (is_system_proc(proc)) { proc->cpu = machine.bsp_id; return; }
	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		if (!cpu_is_available(c)) continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) { cpu_load = cpu_proc[c]; cpu = c; }
	}
	proc->cpu = cpu;
	cpu_proc[cpu]++;
#else
	proc->cpu = 0;
#endif
}

/*=========== do_noquantum (LÓGICA FCFS) ===========*/
int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;
	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) { return EBADEPT; }
	rmp = &schedproc[proc_nr_n];

	/* Em FCFS, não penalizamos o processo. Não fazemos nada com a prioridade. */

	if ((rv = schedule_process_local(rmp)) != OK) { return rv; }
	return OK;
}

/*=========== do_stop_scheduling ===========*/
int do_stop_scheduling(message *m_ptr) {
	register struct schedproc *rmp;
	int proc_nr_n;
	if (!accept_message(m_ptr)) return EPERM;
	if (sched_isokendpt(m_ptr->m_lsys_sched_scheduling_stop.endpoint, &proc_nr_n) != OK) { return EBADEPT; }
	rmp = &schedproc[proc_nr_n];
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0;
	return OK;
}

/*=========== do_start_scheduling (LÓGICA FCFS)===========*/
int do_start_scheduling(message *m_ptr) {
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	assert(m_ptr->m_type == SCHEDULING_START || m_ptr->m_type == SCHEDULING_INHERIT);
	if (!accept_message(m_ptr)) return EPERM;
	if ((rv = sched_isemtyendpt(m_ptr->m_lsys_sched_scheduling_start.endpoint, &proc_nr_n)) != OK) { return rv; }
	rmp = &schedproc[proc_nr_n];
	rmp->endpoint = m_ptr->m_lsys_sched_scheduling_start.endpoint;
	rmp->parent = m_ptr->m_lsys_sched_scheduling_start.parent;
	rmp->max_priority = m_ptr->m_lsys_sched_scheduling_start.maxprio;
	if (rmp->max_priority >= NR_SCHED_QUEUES) { return EINVAL; }
	
    /* Em FCFS, todos os processos de usuário vão para a mesma fila de prioridade. */
    if (rmp->max_priority >= USER_Q) {
        rmp->max_priority = USER_Q;
    }

	if (rmp->endpoint == rmp->parent) {
		rmp->priority = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
#endif
	}

	switch (m_ptr->m_type) {

	case SCHEDULING_START:
	    rmp->priority   = rmp->max_priority;
	    rmp->time_slice = m_ptr->m_lsys_sched_scheduling_start.quantum;
	    break;
	    
	case SCHEDULING_INHERIT:
	    if ((rv = sched_isokendpt(m_ptr->m_lsys_sched_scheduling_start.parent,
	        &parent_nr_n)) != OK)
	        return rv;
	    rmp->priority = USER_Q; /* Força todos os filhos de usuário para a mesma fila */
	    
	    /* AQUI ESTÁ A MUDANÇA CRUCIAL */
	    rmp->time_slice = 5000; /* Dê um quantum gigante (5000 ticks) */
	
	    break;
	    
	default: 
	    assert(0);
	}
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) { return rv; }
	rmp->flags = IN_USE;
	pick_cpu(rmp);
	while ((rv = schedule_process(rmp, SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		cpu_proc[rmp->cpu] = CPU_DEAD;
		pick_cpu(rmp);
	}
	if (rv != OK) { return rv; }
	m_ptr->m_sched_lsys_scheduling_start.scheduler = SCHED_PROC_NR;
	return OK;
}

/*=========== do_nice (LÓGICA FCFS) ===========*/
int do_nice(message *m_ptr) {
    /* Em FCFS, nice não tem efeito, pois todos estão na mesma fila. */
    return OK;
}

/*=========== schedule_process ===========*/
static int schedule_process(struct schedproc * rmp, unsigned flags) {
	int err, new_prio, new_quantum, new_cpu, niced;
	pick_cpu(rmp);
	if (flags & SCHEDULE_CHANGE_PRIO) new_prio = rmp->priority;
	else new_prio = -1;
	if (flags & SCHEDULE_CHANGE_QUANTUM) new_quantum = rmp->time_slice;
	else new_quantum = -1;
	if (flags & SCHEDULE_CHANGE_CPU) new_cpu = rmp->cpu;
	else new_cpu = -1;
	niced = (rmp->max_priority > USER_Q);
	if ((err = sys_schedule(rmp->endpoint, new_prio, new_quantum, new_cpu, niced)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n", rmp->endpoint, err);
	}
	return err;
}

/*=========== init_scheduling ===========*/
void init_scheduling(void)
{
    /* NOSSA IMPRESSÃO DIGITAL ÚNICA */
    printf("SCHEDULER: >>> MEU ESCALONADOR FCFS GLOBAL ESTA ATIVO! <<<\n");

    int r;
    balance_timeout = BALANCE_TIMEOUT * sys_hz();
    if ((r = sys_setalarm(balance_timeout, 0)) != OK)
        panic("sys_setalarm failed: %d", r);
}

/*=========== balance_queues (LÓGICA FCFS) ===========*/
void balance_queues(void) {
    /* Em FCFS, não há balanceamento de filas, pois não mudamos a prioridade. */
	int r;
	if ((r = sys_setalarm(balance_timeout, 0)) != OK) panic("sys_setalarm failed: %d", r);
}
