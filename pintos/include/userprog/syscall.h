#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <debug.h>
#include "threads/synch.h"

void syscall_init (void);
void sys_exit (int status) NO_RETURN;
extern struct lock filesys_lock;

#endif /* userprog/syscall.h */