/*
 * rcutorture.h: simple user-level performance/stress test of RCU.
 *
 * Usage:
 * 	./rcu <nreaders> rperf [ <seconds> ]
 * 		Run a read-side performance test with the specified
 * 		number of readers for <seconds> seconds.
 * 		Thus "./rcu 16 rperf 2" would run 16 readers on even-numbered
 * 		CPUs from 0 to 30.
 * 	./rcu <nupdaters> uperf [ <seconds> ]
 * 		Run an update-side performance test with the specified
 * 		number of updaters and specified duration.
 * 	./rcu <nreaders> perf [ <seconds> ]
 * 		Run a combined read/update performance test with the specified
 * 		number of readers and one updater and specified duration.
 *
 * The above tests produce output as follows:
 *
 * n_reads: 46008000  n_updates: 146026  nreaders: 2  nupdaters: 1 duration: 1
 * ns/read: 43.4707  ns/update: 6848.1
 *
 * The first line lists the total number of RCU reads and updates executed
 * during the test, the number of reader threads, the number of updater
 * threads, and the duration of the test in seconds.  The second line
 * lists the average duration of each type of operation in nanoseconds,
 * or "nan" if the corresponding type of operation was not performed.
 *
 * 	./rcu <nreaders> stress [ <seconds> ]
 * 		Run a stress test with the specified number of readers and
 * 		one updater.  None of the threads are affinitied to any
 * 		particular CPU.
 *
 * This test produces output as follows:
 *
 * n_reads: 114633217  n_updates: 3903415  n_mberror: 0
 * rcu_stress_count: 114618391 14826 0 0 0 0 0 0 0 0 0
 *
 * The first line lists the number of RCU read and update operations
 * executed, followed by the number of memory-ordering violations
 * (which will be zero in a correct RCU implementation).  The second
 * line lists the number of readers observing progressively more stale
 * data.  A correct RCU implementation will have all but the first two
 * numbers non-zero.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (c) 2008 Paul E. McKenney, IBM Corporation.
 */

/*
 * Test variables.
 */

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "qemu/atomic.h"
#include "rcu.h"
#include "compiler.h"
#include "qemu-thread.h"

#include "qemu-thread-posix.c"
#include "rcu.c"

long long n_reads = 0LL;
long n_updates = 0L;
int nthreadsrunning;

char argsbuf[64];

#define GOFLAG_INIT 0
#define GOFLAG_RUN  1
#define GOFLAG_STOP 2

volatile int goflag = GOFLAG_INIT;

#define RCU_READ_RUN 1000

#define NR_THREADS 100
static pthread_t threads[NR_THREADS];

static void create_thread(void *(*func)(void *), void *arg)
{
	pthread_t tid;
	int i;

	if (pthread_create(&tid, NULL, func, arg) != 0) {
		perror("create_thread:pthread_create");
		exit(-1);
	}
	for (i = 0; i < NR_THREADS; i++) {
		if (__sync_bool_compare_and_swap(&threads[i], 0, tid))
			break;
	}
	if (i >= NR_THREADS) {
		fprintf(stderr, "Thread limit of %d exceeded!\n", NR_THREADS);
		exit(-1);
	}
}

static void wait_all_threads(void)
{
	int i;
	void *vp;
	pthread_t tid;

	for (i = 1; i < NR_THREADS; i++) {
		tid = threads[i];
		if (tid != 0) {
			if (pthread_join(tid, &vp) != 0) {
				perror("wait_thread:pthread_join");
				exit(-1);
			}
			threads[i] = 0;
		}
	}
}

/*
 * Performance test.
 */

void *rcu_read_perf_test(void *arg)
{
	int i;
	long long n_reads_local = 0;

	rcu_register_thread();
	__sync_fetch_and_add(&nthreadsrunning, 1);
	rcu_thread_offline();
	while (goflag == GOFLAG_INIT)
		poll(NULL, 0, 1);
	rcu_thread_online();
	while (goflag == GOFLAG_RUN) {
		for (i = 0; i < RCU_READ_RUN; i++) {
			rcu_read_lock();
			rcu_read_unlock();
		}
		n_reads_local += RCU_READ_RUN;
		rcu_quiescent_state();
	}
	__sync_fetch_and_add(&n_reads, n_reads_local);
	rcu_unregister_thread();

	return (NULL);
}

void *rcu_update_perf_test(void *arg)
{
	long long n_updates_local = 0;

	__sync_fetch_and_add(&nthreadsrunning, 1);
	while (goflag == GOFLAG_INIT)
		poll(NULL, 0, 1);
	while (goflag == GOFLAG_RUN) {
		synchronize_rcu();
		n_updates_local++;
	}
	__sync_fetch_and_add(&n_updates, n_updates_local);
	return NULL;
}

void perftestinit(void)
{
	nthreadsrunning = 0;
}

void perftestrun(int nthreads, int duration, int nreaders, int nupdaters)
{
	int t;

	while (atomic_read(&nthreadsrunning) < nthreads)
		poll(NULL, 0, 1);
	goflag = GOFLAG_RUN;
	sleep(duration);
	goflag = GOFLAG_STOP;
	wait_all_threads();
	printf("n_reads: %lld  n_updates: %ld  nreaders: %d  nupdaters: %d duration: %d\n",
	       n_reads, n_updates, nreaders, nupdaters, duration);
	printf("ns/read: %g  ns/update: %g\n",
	       ((duration * 1000*1000*1000.*(double)nreaders) /
		(double)n_reads),
	       ((duration * 1000*1000*1000.*(double)nupdaters) /
		(double)n_updates));
	exit(0);
}

void perftest(int nreaders, int duration)
{
	int i;

	perftestinit();
	for (i = 0; i < nreaders; i++) {
		create_thread(rcu_read_perf_test, NULL);
	}
	create_thread(rcu_update_perf_test, NULL);
	perftestrun(i + 1, duration, nreaders, 1);
}

void rperftest(int nreaders, int duration)
{
	int i;

	perftestinit();
	for (i = 0; i < nreaders; i++) {
		create_thread(rcu_read_perf_test, NULL);
	}
	perftestrun(i, duration, nreaders, 0);
}

void uperftest(int nupdaters, int duration)
{
	int i;

	perftestinit();
	for (i = 0; i < nupdaters; i++) {
		create_thread(rcu_update_perf_test, NULL);
	}
	perftestrun(i, duration, 0, nupdaters);
}

/*
 * Stress test.
 */

#define RCU_STRESS_PIPE_LEN 10

struct rcu_stress {
	int pipe_count;
	int mbtest;
};

struct rcu_stress rcu_stress_array[RCU_STRESS_PIPE_LEN] = { { 0 } };
struct rcu_stress *rcu_stress_current;
int rcu_stress_idx = 0;

int n_mberror = 0;
long long rcu_stress_count[RCU_STRESS_PIPE_LEN + 1];

int garbage = 0;

void *rcu_read_stress_test(void *arg)
{
	int i;
	int itercnt = 0;
	struct rcu_stress *p;
	int pc;
	long long n_reads_local;

	rcu_register_thread();
	rcu_thread_offline();
	while (goflag == GOFLAG_INIT)
		poll(NULL, 0, 1);
	rcu_thread_online();
	while (goflag == GOFLAG_RUN) {
		rcu_read_lock();
		p = rcu_dereference(rcu_stress_current);
		if (p->mbtest == 0)
			n_mberror++;
		rcu_read_lock();
		for (i = 0; i < 100; i++)
			garbage++;
		rcu_read_unlock();
		pc = p->pipe_count;
		rcu_read_unlock();
		if ((pc > RCU_STRESS_PIPE_LEN) || (pc < 0))
			pc = RCU_STRESS_PIPE_LEN;
		__sync_fetch_and_add(&rcu_stress_count[pc], 1);
		n_reads_local++;
		rcu_quiescent_state();
		if ((++itercnt % 0x1000) == 0) {
			synchronize_rcu();
		}
	}
	__sync_fetch_and_add(&n_reads, n_reads_local);
	rcu_thread_offline();
	rcu_unregister_thread();

	return (NULL);
}

#if 0
void rcu_update_stress_test_rcu(struct rcu_head *head)
{
	if (pthread_mutex_lock(&call_rcu_test_mutex) != 0) {
		perror("pthread_mutex_lock");
		exit(-1);
	}
	if (pthread_cond_signal(&call_rcu_test_cond) != 0) {
		perror("pthread_cond_signal");
		exit(-1);
	}
	if (pthread_mutex_unlock(&call_rcu_test_mutex) != 0) {
		perror("pthread_mutex_unlock");
		exit(-1);
	}
}
#endif

void *rcu_update_stress_test(void *arg)
{
	int i;
	struct rcu_stress *p;
#if 0
	struct rcu_head rh;
#endif

	while (goflag == GOFLAG_INIT)
		poll(NULL, 0, 1);
	while (goflag == GOFLAG_RUN) {
		i = rcu_stress_idx + 1;
		if (i >= RCU_STRESS_PIPE_LEN)
			i = 0;
		p = &rcu_stress_array[i];
		p->mbtest = 0;
		__sync_synchronize();
		p->pipe_count = 0;
		p->mbtest = 1;
		rcu_assign_pointer(rcu_stress_current, p);
		rcu_stress_idx = i;
		for (i = 0; i < RCU_STRESS_PIPE_LEN; i++)
			if (i != rcu_stress_idx)
				rcu_stress_array[i].pipe_count++;
#if 0
		if (n_updates & 0x1)
			synchronize_rcu();
		else {
			if (pthread_mutex_lock(&call_rcu_test_mutex) != 0) {
				perror("pthread_mutex_lock");
				exit(-1);
			}
			call_rcu(&rh, rcu_update_stress_test_rcu);
			if (pthread_cond_wait(&call_rcu_test_cond,
					      &call_rcu_test_mutex) != 0) {
				perror("pthread_cond_wait");
				exit(-1);
			}
			if (pthread_mutex_unlock(&call_rcu_test_mutex) != 0) {
				perror("pthread_mutex_unlock");
				exit(-1);
			}
		}
#else
			synchronize_rcu();
#endif
		n_updates++;
	}
	return NULL;
}

void *rcu_fake_update_stress_test(void *arg)
{
	while (goflag == GOFLAG_INIT)
		poll(NULL, 0, 1);
	while (goflag == GOFLAG_RUN) {
		synchronize_rcu();
		poll(NULL, 0, 1);
	}
	return NULL;
}

void stresstest(int nreaders, int duration)
{
	int i;
	int t;
	long long *p;
	long long sum;

	rcu_stress_current = &rcu_stress_array[0];
	rcu_stress_current->pipe_count = 0;
	rcu_stress_current->mbtest = 1;
	for (i = 0; i < nreaders; i++)
		create_thread(rcu_read_stress_test, NULL);
	create_thread(rcu_update_stress_test, NULL);
	for (i = 0; i < 5; i++)
		create_thread(rcu_fake_update_stress_test, NULL);
	goflag = GOFLAG_RUN;
	sleep(duration);
	goflag = GOFLAG_STOP;
	wait_all_threads();
	printf("n_reads: %lld  n_updates: %ld  n_mberror: %d\n",
	       n_reads, n_updates, n_mberror);
	printf("rcu_stress_count:");
	for (i = 0; i <= RCU_STRESS_PIPE_LEN; i++) {
		printf(" %lld", rcu_stress_count[i]);
	}
	printf("\n");
	exit(0);
}

/*
 * Mainprogram.
 */

void usage(int argc, char *argv[])
{
	fprintf(stderr, "Usage: %s [nreaders [ perf | stress ] ]\n", argv[0]);
	exit(-1);
}

int main(int argc, char *argv[])
{
	int nreaders = 1;
	int duration = 1;

	if (argc >= 2) {
		nreaders = strtoul(argv[1], NULL, 0);
	}
	if (argc > 3) {
		duration = strtoul(argv[3], NULL, 0);
	}
	if (argc < 3 || strcmp(argv[2], "stress") == 0)
		stresstest(nreaders, duration);
	else if (strcmp(argv[2], "rperf") == 0)
		rperftest(nreaders, duration);
	else if (strcmp(argv[2], "uperf") == 0)
		uperftest(nreaders, duration);
	else if (strcmp(argv[2], "perf") == 0)
		perftest(nreaders, duration);
	usage(argc, argv);
	return 0;
}
