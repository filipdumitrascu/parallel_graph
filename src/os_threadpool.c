// SPDX-License-Identifier: BSD-3-Clause

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "os_threadpool.h"
#include "log/log.h"
#include "utils.h"

/* Create a task that would be executed by a thread. */
os_task_t *create_task(void (*action)(void *), void *arg, void (*destroy_arg)(void *))
{
	os_task_t *t = malloc(sizeof(*t));

	DIE(!t, "malloc");

	t->action = action;		// the function
	t->argument = arg;		// arguments for the function
	t->destroy_arg = destroy_arg;	// destroy argument function

	return t;
}

/* Destroy task. */
void destroy_task(os_task_t *t)
{
	if (t->destroy_arg)
		t->destroy_arg(t->argument);
	free(t);
}

/* Put a new task to threadpool task queue. */
void enqueue_task(os_threadpool_t *tp, os_task_t *t)
{
	/**
	 * Lock the mutex
	 */
	pthread_mutex_lock(&tp->mutex);

	/**
	 * Add the new task to the end of the queue
	 */
	list_add_tail(&tp->head, &t->list);

	atomic_fetch_add(&tp->tasks_added, 1);

	/**
	 * Signal that a task is available
	 */
	pthread_cond_signal(&tp->cond);

	/**
	 * Unlock the mutex
	 */
	pthread_mutex_unlock(&tp->mutex);
}

/*
 * Check if queue is empty.
 * This function should be called in a synchronized manner.
 */
static int queue_is_empty(os_threadpool_t *tp)
{
	return list_empty(&tp->head);
}

/*
 * Get a task from threadpool task queue.
 * Block if no task is available.
 * Return NULL if work is complete, i.e. no task will become available,
 * i.e. all threads are going to block.
 */
os_task_t *dequeue_task(os_threadpool_t *tp)
{
	os_task_t *t = NULL;

	/**
	 * Lock the mutex
	 */
	pthread_mutex_lock(&tp->mutex);

	/**
	 * Block if no task is available
	 */
	while (queue_is_empty(tp) && atomic_load(&tp->tasks_added) == 0)
		pthread_cond_wait(&tp->cond, &tp->mutex);

	if (!queue_is_empty(tp)) {
		/**
		 * Retrieve the task from the front of the queue
		 */
		t = list_entry(tp->head.next, os_task_t, list);

		/**
		 * Delete the element from the queue
		 */
		list_del(&t->list);
	}

	/**
	 * Unlock the mutex
	 */
	pthread_mutex_unlock(&tp->mutex);

	return t;
}

/* Loop function for threads */
static void *thread_loop_function(void *arg)
{
	os_threadpool_t *tp = (os_threadpool_t *) arg;

	while (1) {
		os_task_t *t;

		t = dequeue_task(tp);
		if (t == NULL)
			break;
		t->action(t->argument);
		destroy_task(t);
	}

	return NULL;
}

/* Wait completion of all threads. This is to be called by the main thread. */
void wait_for_completion(os_threadpool_t *tp)
{
	/**
	 * While the mutex is locked, wakes up all the threads waiting
	 */
	pthread_mutex_lock(&tp->mutex);

	pthread_cond_broadcast(&tp->cond);

	pthread_mutex_unlock(&tp->mutex);

	/* Join all worker threads. */
	for (unsigned int i = 0; i < tp->num_threads; i++)
		pthread_join(tp->threads[i], NULL);
}

/* Create a new threadpool. */
os_threadpool_t *create_threadpool(unsigned int num_threads)
{
	os_threadpool_t *tp = NULL;
	int rc;

	tp = malloc(sizeof(*tp));
	DIE(tp == NULL, "malloc");

	list_init(&tp->head);

	rc = pthread_mutex_init(&tp->mutex, NULL);
	DIE(rc != 0, "pthread_mutex_init");

	rc = pthread_cond_init(&tp->cond, NULL);
	DIE(rc != 0, "pthread_cond_init");

	atomic_init(&tp->tasks_added, 0);
	tp->num_threads = num_threads;
	tp->threads = malloc(num_threads * sizeof(*tp->threads));
	DIE(tp->threads == NULL, "malloc");
	for (unsigned int i = 0; i < num_threads; ++i) {
		rc = pthread_create(&tp->threads[i], NULL, &thread_loop_function, (void *) tp);
		DIE(rc < 0, "pthread_create");
	}

	return tp;
}

/* Destroy a threadpool. Assume all threads have been joined. */
void destroy_threadpool(os_threadpool_t *tp)
{
	os_list_node_t *n, *p;

	int rc = pthread_mutex_destroy(&tp->mutex);

	DIE(rc != 0, "pthread_mutex_destroy");

	rc = pthread_cond_destroy(&tp->cond);
	DIE(rc != 0, "pthread_cond_destroy");

	list_for_each_safe(n, p, &tp->head) {
		list_del(n);
		destroy_task(list_entry(n, os_task_t, list));
	}

	free(tp->threads);
	free(tp);
}
