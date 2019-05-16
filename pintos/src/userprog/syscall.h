#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include <stdbool.h>

typedef int pid_t;
void syscall_init (void);
void close_file_by_owner(int tid);
bool is_valid_ptr(const void *ptr);

#endif /* userprog/syscall.h */
