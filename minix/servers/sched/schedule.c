#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include <sys/stat.h> // Para a checagem de arquivo

/* Variável global para controlar nosso modo de escalonamento */
static int fcfs_mode_active = 0;

/* O resto do código padrão... */
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

/*=========== do_noquantum ===========*/
int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;
	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) { return EBADEPT; }
	rmp = &schedproc[proc_nr_n];

	/* Se o modo FCFS estiver ativo E for um processo de usuário... */
	if (fcfs_mode_active && rmp->priority >= USER_Q) {
		/* ... não faz nada com a prioridade (lógica FCFS) */
	} else {
		/* Senão, aplica a política MLFQ padrão */
		if (rmp->priority < MIN_USER_Q) {
			rmp->priority += 1;
		}
	}

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

/*=========== do_start_scheduling ===========*/
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
	if (rmp->endpoint == rmp->parent) {
		rmp->priority = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
#endif
	}
	switch (m_ptr->m_type) {
	case SCHEDULING_START:
		rmp->priority = rmp->max_priority;
		rmp->time_slice = m_ptr->m_lsys_sched_scheduling_start.quantum;
		break;
	case SCHEDULING_INHERIT:
		if ((rv = sched_isokendpt(m_ptr->m_lsys_sched_scheduling_start.parent, &parent_nr_n)) != OK) return rv;
		rmp->priority = schedproc[parent_nr_n].priority;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
	default:
		assert(0);
	}

    /* Se o modo FCFS estiver ativo, força todos os novos processos de usuário para uma única fila */
    if (fcfs_mode_active && rmp->priority >= USER_Q) {
        rmp->priority = USER_Q;
        rmp->max_priority = USER_Q;
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

/*=========== do_nice (VERSÃO ORIGINAL) ===========*/
int do_nice(message *m_ptr) {
    struct schedproc *rmp;
    int rv, proc_nr_n;
    unsigned new_q, old_q, old_max_q;
    if (!accept_message(m_ptr)) return EPERM;
    if (sched_isokendpt(m_ptr->m_pm_sched_scheduling_set_nice.endpoint, &proc_nr_n) != OK) { return EBADEPT; }
    rmp = &schedproc[proc_nr_n];
    new_q = m_ptr->m_pm_sched_scheduling_set_nice.maxprio;
    if (new_q >= NR_SCHED_QUEUES) { return EINVAL; }
    old_q = rmp->priority;
    old_max_q = rmp->max_priority;
    rmp->max_priority = rmp->priority = new_q;
    if ((rv = schedule_process_local(rmp)) != OK) {
        rmp->priority = old_q;
        rmp->max_priority = old_max_q;
    }
    return rv;
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

/*=========== init_scheduling (MODIFICADO) ===========*/
void init_scheduling(void)
{
	int r;
    struct stat st;

    /* Checa se o arquivo /tmp/fcfs_on existe */
    if (stat("/tmp/fcfs_on", &st) == 0) {
        printf("SCHED: FCFS mode is ON.\n");
        fcfs_mode_active = 1;
    } else {
        printf("SCHED: FCFS mode is OFF (default MLFQ).\n");
        fcfs_mode_active = 0;
    }

	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	if ((r = sys_setalarm(balance_timeout, 0)) != OK) panic("sys_setalarm failed: %d", r);
}

/*=========== balance_queues ===========*/
void balance_queues(void)
{
	struct schedproc *rmp;
	int r, proc_nr;

    /* Se o modo FCFS estiver ativo, não fazemos balanceamento */
    if (fcfs_mode_active) {
        if ((r = sys_setalarm(balance_timeout, 0)) != OK) panic("sys_setalarm failed: %d", r);
        return;
    }

	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if (rmp->priority > rmp->max_priority) {
				rmp->priority -= 1;
				schedule_process_local(rmp);
			}
		}
	}
	if ((r = sys_setalarm(balance_timeout, 0)) != OK) panic("sys_setalarm failed: %d", r);
}
