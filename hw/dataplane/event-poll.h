#ifndef EVENT_POLL_H
#define EVENT_POLL_H

#include <sys/epoll.h>
#include "hw/event_notifier.h"

typedef struct EventHandler EventHandler;
typedef bool EventCallback(EventHandler *handler);
struct EventHandler
{
    EventNotifier *notifier;        /* eventfd */
    EventCallback *callback;        /* callback function */
};

typedef struct {
    int epoll_fd;                   /* epoll(2) file descriptor */
    EventNotifier stop_notifier;    /* stop poll notifier */
    EventHandler stop_handler;      /* stop poll handler */
} EventPoll;

/* Add an event notifier and its callback for polling */
static void event_poll_add(EventPoll *poll, EventHandler *handler, EventNotifier *notifier, EventCallback *callback)
{
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.ptr = handler,
    };
    handler->notifier = notifier;
    handler->callback = callback;
    if (epoll_ctl(poll->epoll_fd, EPOLL_CTL_ADD, event_notifier_get_fd(notifier), &event) != 0) {
        fprintf(stderr, "failed to add event handler to epoll: %m\n");
        exit(1);
    }
}

/* Event callback for stopping the event_poll_run() loop */
static bool handle_stop(EventHandler *handler)
{
    return false; /* stop event loop */
}

static void event_poll_init(EventPoll *poll)
{
    /* Create epoll file descriptor */
    poll->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (poll->epoll_fd < 0) {
        fprintf(stderr, "epoll_create1 failed: %m\n");
        exit(1);
    }

    /* Set up stop notifier */
    if (event_notifier_init(&poll->stop_notifier, 0) < 0) {
        fprintf(stderr, "failed to init stop notifier\n");
        exit(1);
    }
    event_poll_add(poll, &poll->stop_handler,
                   &poll->stop_notifier, handle_stop);
}

static void event_poll_cleanup(EventPoll *poll)
{
    event_notifier_cleanup(&poll->stop_notifier);
    close(poll->epoll_fd);
    poll->epoll_fd = -1;
}

/* Block until the next event and invoke its callback
 *
 * Signals must be masked, EINTR should never happen.  This is true for QEMU
 * threads.
 */
static bool event_poll(EventPoll *poll)
{
    EventHandler *handler;
    struct epoll_event event;
    int nevents;

    /* Wait for the next event.  Only do one event per call to keep the
     * function simple, this could be changed later. */
    nevents = epoll_wait(poll->epoll_fd, &event, 1, -1);
    if (unlikely(nevents != 1)) {
        fprintf(stderr, "epoll_wait failed: %m\n");
        exit(1); /* should never happen */
    }

    /* Find out which event handler has become active */
    handler = event.data.ptr;

    /* Clear the eventfd */
    event_notifier_test_and_clear(handler->notifier);

    /* Handle the event */
    return handler->callback(handler);
}

static void event_poll_run(EventPoll *poll)
{
    while (event_poll(poll)) {
        /* do nothing */
    }
}

/* Stop the event_poll_run() loop
 *
 * This function can be used from another thread.
 */
static void event_poll_stop(EventPoll *poll)
{
    uint64_t dummy = 1;
    int eventfd = event_notifier_get_fd(&poll->stop_notifier);
    ssize_t unused;

    unused = write(eventfd, &dummy, sizeof dummy);
}

#endif /* EVENT_POLL_H */
