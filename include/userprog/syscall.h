#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/synch.h" // Project 2


void syscall_init (void);
struct lock filesys_lock; // Project 2

#endif /* userprog/syscall.h */