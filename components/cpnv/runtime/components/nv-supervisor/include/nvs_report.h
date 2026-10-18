/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVS_REPORT_H
#define NVS_REPORT_H
#include <stdint.h>
int nvs_pidfd_matches(int pidfd, int executable_fd, int pid);
int nvs_report_send(int socket_fd, int writer_pidfd, int pid, uint64_t nonce);
int nvs_report_receive(int socket_fd, int executable_fd, uint64_t nonce, int *pid_out);
#endif
