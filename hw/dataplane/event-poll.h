#ifndef EVENT_POLL_H
#define EVENT_POLL_H

#include <sys/epoll.h>
#include "hw/event_notifier.h"

typedef struct EventHandler EventHandler;
typedef void EventCallback(EventHandler *handler);
struct EventHandler
{
    EventNotifier *notifier;    /* eventfd */
    EventCallback *callback;    /* callback function */
};

typedef struct {
    int epoll_fd;               /* epoll(2) file descriptor */
} EventPoll;

static void event_poll_init(EventPoll *poll)
{
    /* Create epoll file descriptor */
    poll->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (poll->epoll_fd < 0) {
        fprintf(stderr, "epoll_create1 failed: %m\n");
        exit(1);
    }
}

static void event_poll_cleanup(EventPoll *poll)
{
    close(poll->epoll_fd);
    poll->epoll_fd = -1;
}

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

/* Block until the next event and invoke its callback
 *
 * Signals must be masked, EINTR should never happen.  This is true for QEMU
 * threads.
 */
static void event_poll(EventPoll *poll)
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
    handler->callback(handler);
}

#endif /* EVENT_POLL_H */
