#include "cocaine/lines.hpp"

using namespace cocaine::lines;

int socket_t::fd() {
    int fd;
    size_t size = sizeof(fd);

    getsockopt(ZMQ_FD, &fd, &size);

    return fd;
}

bool socket_t::pending(int event) {
#if ZMQ_VERSION > 30000
    int events;
#else
    unsigned long events;
#endif

    size_t size = sizeof(events);

    getsockopt(ZMQ_EVENTS, &events, &size);

    return events & event;
}

bool socket_t::more() {
#if ZMQ_VERSION > 30000
    int rcvmore;
#else
    int64_t rcvmore;
#endif

    size_t size = sizeof(rcvmore);

    getsockopt(ZMQ_RCVMORE, &rcvmore, &size);

    return rcvmore != 0;
}

#if ZMQ_VERSION > 30000
bool socket_t::label() {
    int rcvlabel;
    size_t size = sizeof(rcvlabel);

    getsockopt(ZMQ_RCVLABEL, &rcvlabel, &size);

    return rcvlabel != 0;
}
#endif
