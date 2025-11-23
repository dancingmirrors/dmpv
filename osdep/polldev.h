#pragma once

#include <poll.h>

// Only supports POLLIN and POLLOUT.
int polldev(struct pollfd fds[], nfds_t nfds, int timeout);
