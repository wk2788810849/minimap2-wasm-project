#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include "kthread.h"

#if (defined(WIN32) || defined(_WIN32)) && defined(_MSC_VER)
#define __sync_fetch_and_add(ptr, addend)     _InterlockedExchangeAdd((void*)ptr, addend)
#endif

/************
 * kt_for() *
 ************/

struct kt_for_t;

typedef struct {
	struct kt_for_t *t;
	long i;
} ktf_worker_t;

typedef struct kt_for_t {
	int n_threads;
	long n;
	ktf_worker_t *w;
	void (*func)(void*,long,int);
	void *data;
} kt_for_t;

static inline long steal_work(kt_for_t *t)
{
	int i, min_i = -1;
	long k, min = LONG_MAX;
	for (i = 0; i < t->n_threads; ++i)
		if (min > t->w[i].i) min = t->w[i].i, min_i = i;
	k = __sync_fetch_and_add(&t->w[min_i].i, t->n_threads);
	return k >= t->n? -1 : k;
}

static void *ktf_worker(void *data)
{
	ktf_worker_t *w = (ktf_worker_t*)data;
	long i;
	for (;;) {
		i = __sync_fetch_and_add(&w->i, w->t->n_threads);
		if (i >= w->t->n) break;
		w->t->func(w->t->data, i, w - w->t->w);
	}
	while ((i = steal_work(w->t)) >= 0)
		w->t->func(w->t->data, i, w - w->t->w);
	pthread_exit(0);
}

// void kt_for(int n_threads, void (*func)(void*,long,int), void *data, long n)
// {
// 	if (n_threads > 1) {
// 		int i;
// 		kt_for_t t;
// 		pthread_t *tid;
// 		t.func = func, t.data = data, t.n_threads = n_threads, t.n = n;
// 		t.w = (ktf_worker_t*)calloc(n_threads, sizeof(ktf_worker_t));
// 		tid = (pthread_t*)calloc(n_threads, sizeof(pthread_t));
// 		for (i = 0; i < n_threads; ++i)
// 			t.w[i].t = &t, t.w[i].i = i;
// 		for (i = 0; i < n_threads; ++i) pthread_create(&tid[i], 0, ktf_worker, &t.w[i]);
// 		for (i = 0; i < n_threads; ++i) pthread_join(tid[i], 0);
// 		free(tid); free(t.w);
// 	} else {
// 		long j;
// 		for (j = 0; j < n; ++j) func(data, j, 0);
// 	}
// }

// This is the new, single-threaded version of kt_for.  单线程版本
void kt_for(int n_threads, void (*func)(void*, long, int), void *data, long n)
{
	// The n_threads argument is now ignored, but kept for compatibility.
	// We always run the simple, sequential loop.
	long j;
	for (j = 0; j < n; ++j) {
		// The last argument is the thread ID, which is always 0 in single-threaded mode.
		func(data, j, 0);
	}
}

/*****************
 * kt_pipeline() *
 *****************/

struct ktp_t;

typedef struct {
	struct ktp_t *pl;
	int64_t index;
	int step;
	void *data;
} ktp_worker_t;

typedef struct ktp_t {
	void *shared;
	void *(*func)(void*, int, void*);
	int64_t index;
	int n_workers, n_steps;
	ktp_worker_t *workers;
	pthread_mutex_t mutex;
	pthread_cond_t cv;
} ktp_t;

static void *ktp_worker(void *data)
{
	ktp_worker_t *w = (ktp_worker_t*)data;
	ktp_t *p = w->pl;
	while (w->step < p->n_steps) {
		// test whether we can kick off the job with this worker
		pthread_mutex_lock(&p->mutex);
		for (;;) {
			int i;
			// test whether another worker is doing the same step
			for (i = 0; i < p->n_workers; ++i) {
				if (w == &p->workers[i]) continue; // ignore itself
				if (p->workers[i].step <= w->step && p->workers[i].index < w->index)
					break;
			}
			if (i == p->n_workers) break; // no workers with smaller indices are doing w->step or the previous steps
			pthread_cond_wait(&p->cv, &p->mutex);
		}
		pthread_mutex_unlock(&p->mutex);

		// working on w->step
		w->data = p->func(p->shared, w->step, w->step? w->data : 0); // for the first step, input is NULL

		// update step and let other workers know
		pthread_mutex_lock(&p->mutex);
		w->step = w->step == p->n_steps - 1 || w->data? (w->step + 1) % p->n_steps : p->n_steps;
		if (w->step == 0) w->index = p->index++;
		pthread_cond_broadcast(&p->cv);
		pthread_mutex_unlock(&p->mutex);
	}
	pthread_exit(0);
}

// 原始的 kt_pipeline 实现，基于多线程
// void kt_pipeline(int n_threads, void *(*func)(void*, int, void*), void *shared_data, int n_steps)
// {
// 	ktp_t aux;
// 	pthread_t *tid;
// 	int i;

// 	if (n_threads < 1) n_threads = 1;
// 	aux.n_workers = n_threads;
// 	aux.n_steps = n_steps;
// 	aux.func = func;
// 	aux.shared = shared_data;
// 	aux.index = 0;
// 	pthread_mutex_init(&aux.mutex, 0);
// 	pthread_cond_init(&aux.cv, 0);

// 	aux.workers = (ktp_worker_t*)calloc(n_threads, sizeof(ktp_worker_t));
// 	for (i = 0; i < n_threads; ++i) {
// 		ktp_worker_t *w = &aux.workers[i];
// 		w->step = 0; w->pl = &aux; w->data = 0;
// 		w->index = aux.index++;
// 	}

// 	tid = (pthread_t*)calloc(n_threads, sizeof(pthread_t));
// 	for (i = 0; i < n_threads; ++i) pthread_create(&tid[i], 0, ktp_worker, &aux.workers[i]);
// 	for (i = 0; i < n_threads; ++i) pthread_join(tid[i], 0);
// 	free(tid); free(aux.workers);

// 	pthread_mutex_destroy(&aux.mutex);
// 	pthread_cond_destroy(&aux.cv);
// }



/*********************************
 * kt_pipeline() - SINGLE-THREADED gemini 的单线程模拟版本
 *********************************/

/*
 * This is a single-threaded, drop-in replacement for the original kt_pipeline.
 * It mimics the pipeline logic sequentially without using any pthreads.
 * This makes it compatible with WebAssembly (WASM) and other environments
 * where threading is not available or desired.
 */
void kt_pipeline(int n_threads, void *(*func)(void*, int, void*), void *shared_data, int n_steps)
{
	void *step_data = NULL; // 用于在流水线步骤之间传递数据的指针

	// 外部无限循环，模拟不断从数据源拉取新任务
	for (;;) {
		// --- 步骤 0: 流水线的起点 ---
		// 调用一次 func 获取一个初始数据块 (输入为 NULL)
		step_data = func(shared_data, 0, NULL);
		
		// 如果步骤0返回NULL，说明数据源已经耗尽，整个流水线结束。
		if (step_data == NULL) {
			break;
		}

		// --- 步骤 1 到 n_steps-1: 处理数据块 ---
		// 内部循环，将获取到的数据块依次传递给后续的步骤
		int step;
		for (step = 1; step < n_steps; ++step) {
			// 调用 func，将上一步的输出 (step_data)作为当前步骤的输入
			step_data = func(shared_data, step, step_data);

			// 如果任何中间步骤返回NULL，表示这个数据块的处理提前终止。
			// 我们应该跳出内部循环，然后返回外部循环去获取下一个数据块。
			if (step_data == NULL) {
				break;
			}
		}
	}
}