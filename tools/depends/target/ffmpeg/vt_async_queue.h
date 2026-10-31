#ifndef GOFUSE_VT_ASYNC_QUEUE_H
#define GOFUSE_VT_ASYNC_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GoFuseVTQueue GoFuseVTQueue;
typedef struct GoFuseVTTicket GoFuseVTTicket;

typedef enum {
    GOFUSE_VT_OK = 0,
    GOFUSE_VT_FULL = 1,
    GOFUSE_VT_STALE = 2,
    GOFUSE_VT_TIMEOUT = 3,
    GOFUSE_VT_FAILED = 4,
    GOFUSE_VT_CLOSED = 5,
    GOFUSE_VT_NOMEM = 6,
    GOFUSE_VT_BUSY = 7
} GoFuseVTResult;

/* Called for an output not transferred to the waiting frame. */
typedef void (*GoFuseVTReleaseOutput)(void *opaque, void *output);

/*
 * Decoder-owned bounded frame-lifecycle queue.
 * The frame owner and the VideoToolbox callback each retain one ticket.
 * The callback must complete every successfully submitted ticket exactly once.
 */
GoFuseVTQueue *gofuse_vt_queue_create(unsigned capacity,
                                     GoFuseVTReleaseOutput release_output,
                                     void *release_opaque);
uint64_t gofuse_vt_queue_generation(GoFuseVTQueue *queue);
unsigned gofuse_vt_queue_inflight(GoFuseVTQueue *queue);

/* Immediately retires older generations; pending callbacks still own their tickets. */
uint64_t gofuse_vt_queue_seek(GoFuseVTQueue *queue);

/* The owner and decoder callback each receive one implicit reference. */
GoFuseVTResult gofuse_vt_queue_reserve(GoFuseVTQueue *queue,
                                      uint64_t generation,
                                      int64_t pts_us,
                                      GoFuseVTTicket **ticket);

/* Wait for a completion to free capacity without discarding reference frames. */
GoFuseVTResult gofuse_vt_queue_wait_capacity(GoFuseVTQueue *queue,
                                            uint64_t generation,
                                            unsigned timeout_ms);

/*
 * Copy compressed input before submitting an asynchronous VideoToolbox frame.
 * The copy survives both the reusable codec bitstream and callback completion.
 * Maximum supported sample size is 16 MiB; do not reuse a ticket for new input.
 */
GoFuseVTResult gofuse_vt_ticket_copy_input(GoFuseVTTicket *ticket,
                                          const void *bytes,
                                          size_t size);
const void *gofuse_vt_ticket_input(GoFuseVTTicket *ticket, size_t *size);

/*
 * Transfers ownership of output into the ticket, or releases it if obsolete.
 * On a synchronous submission error, invoke this with success=0, output=NULL;
 * only then can the reserved callback reference be released.
 */
void gofuse_vt_ticket_complete(GoFuseVTTicket *ticket,
                              int success,
                              void *output);

/* A successful wait transfers ownership of the output to the owner. */
GoFuseVTResult gofuse_vt_ticket_wait(GoFuseVTTicket *ticket,
                                    unsigned timeout_ms,
                                    void **output);
int64_t gofuse_vt_ticket_pts(GoFuseVTTicket *ticket);
void gofuse_vt_ticket_release(GoFuseVTTicket *ticket);

/*
 * Closes the queue to further reservations. BUSY means the queue stays allocated
 * until pending callbacks finish and all owners release their tickets.
 * Call again after draining; never free callback-owned tickets directly.
 */
GoFuseVTResult gofuse_vt_queue_destroy(GoFuseVTQueue *queue);

/* Release the decoder's queue ownership. Deferred free occurs after every
 * outstanding ticket and active capacity waiter has completed/released.
 * The caller must never access the queue after this call. */
void gofuse_vt_queue_close(GoFuseVTQueue *queue);

#ifdef __cplusplus
}
#endif
#endif
