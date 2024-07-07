// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>

#include "os_graph.h"
#include "os_threadpool.h"
#include "log/log.h"
#include "utils.h"

#define NUM_THREADS		4

static int sum;
static os_graph_t *graph;
static os_threadpool_t *tp;
static pthread_mutex_t graph_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	unsigned int idx;
} graph_task_args_t;

void execute(void *arg)
{
	graph_task_args_t *task_args = (graph_task_args_t *)arg;
	unsigned int idx = task_args->idx;
	os_node_t *node;

	pthread_mutex_lock(&graph_mutex);

	node = graph->nodes[idx];

	if (graph->visited[idx] == PROCESSING) {
		sum += node->info;
		graph->visited[idx] = DONE;

		/**
		 * For every neighbour node, if it is not visited
		 * calculates total sum taking it into account.
		 */
		for (unsigned int i = 0; i < node->num_neighbours; i++) {
			if (graph->visited[node->neighbours[i]] == NOT_VISITED) {
				graph->visited[node->neighbours[i]] = PROCESSING;

				graph_task_args_t *new_task_args = malloc(sizeof(graph_task_args_t));

				DIE(!new_task_args, "malloc");

				new_task_args->idx = node->neighbours[i];

				os_task_t *new_task = create_task(execute, new_task_args, free);

				enqueue_task(tp, new_task);
			}
		}
	}

	pthread_mutex_unlock(&graph_mutex);
}

void process_node(unsigned int idx)
{
	graph_task_args_t *task_args = malloc(sizeof(graph_task_args_t));

	DIE(!task_args, "malloc");

	pthread_mutex_lock(&graph_mutex);

	/**
	 * For the current node, set the status to processing and
	 * creates a task to calculate it's sum
	 */
	task_args->idx = idx;
	graph->visited[idx] = PROCESSING;

	os_task_t *task = create_task(execute, task_args, free);

	enqueue_task(tp, task);

	pthread_mutex_unlock(&graph_mutex);
}

int main(int argc, char *argv[])
{
	FILE *input_file;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s input_file\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	input_file = fopen(argv[1], "r");
	DIE(input_file == NULL, "fopen");

	graph = create_graph_from_file(input_file);

	int rc = pthread_mutex_init(&graph_mutex, NULL);

	DIE(rc != 0, "pthread_mutex_init");

	tp = create_threadpool(NUM_THREADS);
	process_node(0);
	wait_for_completion(tp);
	destroy_threadpool(tp);

	rc = pthread_mutex_destroy(&graph_mutex);

	DIE(rc != 0, "pthread_mutex_destroy");

	printf("%d", sum);

	return 0;
}
