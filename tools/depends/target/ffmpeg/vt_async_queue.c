#include "vt_async_queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct GoFuseVTQueue {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    GoFuseVTReleaseOutput release_output;
    void *release_opaque;
    uint64_t generation;
    unsigned capacity;
    unsigned inflight;
    unsigned live_tickets;
    unsigned capacity_waiters;
    int closing;
    int deferred_free;
};

struct GoFuseVTTicket {
    GoFuseVTQueue *queue;
    uint64_t generation;
    int64_t pts_us;
    unsigned refs;
    int completed;
    int claimed;
    GoFuseVTResult result;
    void *output;
    unsigned char *input;
    size_t input_size;
};

static void free_queue(GoFuseVTQueue *queue)
{
    pthread_cond_destroy(&queue->changed);
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

static struct timespec deadline_after(unsigned milliseconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

GoFuseVTQueue *gofuse_vt_queue_create(unsigned capacity,
                                     GoFuseVTReleaseOutput release_output,
                                     void *release_opaque)
{
    if (capacity == 0 || capacity > 8 || !release_output)
        return NULL;

    GoFuseVTQueue *queue = calloc(1, sizeof(*queue));
    if (!queue)
        return NULL;
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue);
        return NULL;
    }
    if (pthread_cond_init(&queue->changed, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    queue->capacity = capacity;
    queue->generation = 1;
    queue->release_output = release_output;
    queue->release_opaque = release_opaque;
    return queue;
}

uint64_t gofuse_vt_queue_generation(GoFuseVTQueue *queue)
{
    if (!queue)
        return 0;
    pthread_mutex_lock(&queue->mutex);
    uint64_t generation = queue->closing ? 0 : queue->generation;
    pthread_mutex_unlock(&queue->mutex);
    return generation;
}

unsigned gofuse_vt_queue_inflight(GoFuseVTQueue *queue)
{
    if (!queue)
        return 0;
    pthread_mutex_lock(&queue->mutex);
    unsigned count = queue->inflight;
    pthread_mutex_unlock(&queue->mutex);
    return count;
}

uint64_t gofuse_vt_queue_seek(GoFuseVTQueue *queue)
{
    if (!queue)
        return 0;
    pthread_mutex_lock(&queue->mutex);
    if (queue->closing) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    queue->generation++;
    if (queue->generation == 0)
        queue->generation = 1;
    pthread_cond_broadcast(&queue->changed);
    uint64_t generation = queue->generation;
    pthread_mutex_unlock(&queue->mutex);
    return generation;
}

GoFuseVTResult gofuse_vt_queue_reserve(GoFuseVTQueue *queue,
                                      uint64_t generation,
                                      int64_t pts_us,
                                      GoFuseVTTicket **out_ticket)
{
    if (!queue || !out_ticket)
        return GOFUSE_VT_FAILED;
    *out_ticket = NULL;
    pthread_mutex_lock(&queue->mutex);
    GoFuseVTResult result = GOFUSE_VT_OK;
    if (queue->closing)
        result = GOFUSE_VT_CLOSED;
    else if (!generation || generation != queue->generation)
        result = GOFUSE_VT_STALE;
    else if (queue->inflight >= queue->capacity)
        result = GOFUSE_VT_FULL;
    if (result == GOFUSE_VT_OK) {
        GoFuseVTTicket *ticket = calloc(1, sizeof(*ticket));
        if (!ticket)
            result = GOFUSE_VT_NOMEM;
        else {
            ticket->queue = queue;
            ticket->generation = generation;
            ticket->pts_us = pts_us;
            ticket->refs = 2; /* owner and one callback */
            ticket->result = GOFUSE_VT_TIMEOUT;
            queue->inflight++;
            queue->live_tickets++;
            *out_ticket = ticket;
        }
    }
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

GoFuseVTResult gofuse_vt_ticket_copy_input(GoFuseVTTicket *ticket,
                                          const void *bytes,
                                          size_t size)
{
    if (!ticket || !bytes || size == 0 || size > 16U * 1024U * 1024U)
        return GOFUSE_VT_FAILED;
    unsigned char *copy = malloc(size);
    if (!copy)
        return GOFUSE_VT_NOMEM;
    memcpy(copy, bytes, size);

    GoFuseVTQueue *queue = ticket->queue;
    pthread_mutex_lock(&queue->mutex);
    GoFuseVTResult result = GOFUSE_VT_OK;
    if (queue->closing)
        result = GOFUSE_VT_CLOSED;
    else if (ticket->generation != queue->generation)
        result = GOFUSE_VT_STALE;
    else if (ticket->completed || ticket->input)
        result = GOFUSE_VT_FAILED;
    if (result == GOFUSE_VT_OK) {
        ticket->input = copy;
        ticket->input_size = size;
        copy = NULL;
    }
    pthread_mutex_unlock(&queue->mutex);
    free(copy);
    return result;
}

const void *gofuse_vt_ticket_input(GoFuseVTTicket *ticket, size_t *size)
{
    if (!ticket || !size)
        return NULL;
    GoFuseVTQueue *queue = ticket->queue;
    pthread_mutex_lock(&queue->mutex);
    *size = ticket->input_size;
    const void *bytes = ticket->input;
    pthread_mutex_unlock(&queue->mutex);
    /* The owner must retain its ticket for the duration of the read. */
    return bytes;
}

GoFuseVTResult gofuse_vt_queue_wait_capacity(GoFuseVTQueue *queue,
                                            uint64_t generation,
                                            unsigned timeout_ms)
{
    if (!queue)
        return GOFUSE_VT_FAILED;
    const struct timespec deadline = deadline_after(timeout_ms);
    pthread_mutex_lock(&queue->mutex);
    ++queue->capacity_waiters;
    int status = 0;
    while (!queue->closing && queue->generation == generation &&
           queue->inflight >= queue->capacity && status == 0)
        status = pthread_cond_timedwait(&queue->changed, &queue->mutex, &deadline);
    GoFuseVTResult result;
    if (queue->closing)
        result = GOFUSE_VT_CLOSED;
    else if (!generation || queue->generation != generation)
        result = GOFUSE_VT_STALE;
    else if (queue->inflight < queue->capacity)
        result = GOFUSE_VT_OK;
    else
        result = status == ETIMEDOUT ? GOFUSE_VT_TIMEOUT : GOFUSE_VT_FAILED;
    --queue->capacity_waiters;
    const int free_after = queue->deferred_free && !queue->live_tickets &&
                           !queue->capacity_waiters;
    pthread_mutex_unlock(&queue->mutex);
    if (free_after)
        free_queue(queue);
    return result;
}

void gofuse_vt_ticket_complete(GoFuseVTTicket *ticket, int success, void *output)
{
    if (!ticket)
        return;
    GoFuseVTQueue *queue = ticket->queue;
    GoFuseVTReleaseOutput release_output = queue->release_output;
    void *release_opaque = queue->release_opaque;
    int free_ticket = 0;
    int free_after = 0;
    void *orphaned_output = NULL;

    pthread_mutex_lock(&queue->mutex);
    if (ticket->completed) {
        /* The callback contract permits exactly one completion per reservation. */
        pthread_mutex_unlock(&queue->mutex);
        if (output)
            release_output(release_opaque, output);
        return;
    }
    ticket->completed = 1;
    if (ticket->generation != queue->generation || queue->closing)
        ticket->result = GOFUSE_VT_STALE;
    else if (!success || !output)
        ticket->result = GOFUSE_VT_FAILED;
    else {
        ticket->result = GOFUSE_VT_OK;
        ticket->output = output;
        output = NULL;
    }
    queue->inflight--;
    ticket->refs--; /* the callback reference */
    if (ticket->refs == 0) {
        orphaned_output = ticket->output;
        ticket->output = NULL;
        queue->live_tickets--;
        free_ticket = 1;
        free_after = queue->deferred_free && !queue->live_tickets &&
                     !queue->capacity_waiters;
    }
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->mutex);

    if (output)
        release_output(release_opaque, output);
    if (orphaned_output)
        release_output(release_opaque, orphaned_output);
    if (free_ticket) {
        free(ticket->input);
        free(ticket);
    }
    if (free_after)
        free_queue(queue);
}

GoFuseVTResult gofuse_vt_ticket_wait(GoFuseVTTicket *ticket,
                                    unsigned timeout_ms,
                                    void **output)
{
    if (!ticket || !output)
        return GOFUSE_VT_FAILED;
    *output = NULL;
    GoFuseVTQueue *queue = ticket->queue;
    struct timespec deadline = deadline_after(timeout_ms);
    pthread_mutex_lock(&queue->mutex);
    int status = 0;
    while (!ticket->completed && ticket->generation == queue->generation &&
           !queue->closing && status == 0)
        status = pthread_cond_timedwait(&queue->changed, &queue->mutex, &deadline);

    GoFuseVTResult result;
    if (queue->closing)
        result = GOFUSE_VT_CLOSED;
    else if (ticket->generation != queue->generation)
        result = GOFUSE_VT_STALE;
    else if (!ticket->completed)
        result = (status == 0 || status == ETIMEDOUT)
            ? GOFUSE_VT_TIMEOUT : GOFUSE_VT_FAILED;
    else if (ticket->result == GOFUSE_VT_OK && !ticket->claimed && ticket->output) {
        result = GOFUSE_VT_OK;
        *output = ticket->output;
        ticket->output = NULL;
        ticket->claimed = 1;
    } else
        result = ticket->result == GOFUSE_VT_OK ? GOFUSE_VT_FAILED : ticket->result;
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

int64_t gofuse_vt_ticket_pts(GoFuseVTTicket *ticket)
{
    return ticket ? ticket->pts_us : -1;
}

void gofuse_vt_ticket_release(GoFuseVTTicket *ticket)
{
    if (!ticket)
        return;
    GoFuseVTQueue *queue = ticket->queue;
    GoFuseVTReleaseOutput release_output = queue->release_output;
    void *release_opaque = queue->release_opaque;
    void *orphaned_output = NULL;
    int free_ticket = 0;
    int free_after = 0;

    pthread_mutex_lock(&queue->mutex);
    ticket->refs--;
    if (!ticket->refs) {
        orphaned_output = ticket->output;
        ticket->output = NULL;
        queue->live_tickets--;
        free_ticket = 1;
        free_after = queue->deferred_free && !queue->live_tickets &&
                     !queue->capacity_waiters;
    }
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->mutex);

    if (orphaned_output)
        release_output(release_opaque, orphaned_output);
    if (free_ticket) {
        free(ticket->input);
        free(ticket);
    }
    if (free_after)
        free_queue(queue);
}

GoFuseVTResult gofuse_vt_queue_destroy(GoFuseVTQueue *queue)
{
    if (!queue)
        return GOFUSE_VT_FAILED;
    pthread_mutex_lock(&queue->mutex);
    queue->closing = 1;
    queue->generation++;
    pthread_cond_broadcast(&queue->changed);
    if (queue->live_tickets != 0 || queue->capacity_waiters != 0) {
        pthread_mutex_unlock(&queue->mutex);
        return GOFUSE_VT_BUSY;
    }
    pthread_mutex_unlock(&queue->mutex);
    free_queue(queue);
    return GOFUSE_VT_OK;
}

void gofuse_vt_queue_close(GoFuseVTQueue *queue)
{
    if (!queue)
        return;
    pthread_mutex_lock(&queue->mutex);
    queue->closing = 1;
    queue->deferred_free = 1;
    ++queue->generation;
    pthread_cond_broadcast(&queue->changed);
    const int free_now = !queue->live_tickets && !queue->capacity_waiters;
    pthread_mutex_unlock(&queue->mutex);
    if (free_now)
        free_queue(queue);
}
