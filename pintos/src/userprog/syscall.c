#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

struct file_descriptor {
  int fd_num;
  tid_t owner;
  struct file *file_struct;
  struct list_elem elem;
};

struct list open_files;
struct lock fs_lock;

static void halt(void);
static pid_t exec(const char*);
static int wait(pid_t);
static void exit(int);
static bool create(const char *, unsigned);
static bool remove(const char *);
static int open(const char *);
static int filesize(int);
static int read(int, void *, unsigned);
static int write(int, const void *, unsigned);
static void seek(int, unsigned);
static unsigned tell(int);
static void close(int);
static struct file_descriptor *get_open_file(int);
static void close_open_file(int);
void close_file_by_owner(tid_t);
static void syscall_handler (struct intr_frame *);
static int allocate_fd(void);
bool is_valid_ptr(const void *);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  list_init(&open_files);
  lock_init(&fs_lock);
}

static void syscall_handler (struct intr_frame *f) {
  /*
  printf("System call number: %d\n", args[0]);
  if (args[0] == SYS_EXIT) {
    f->eax = args[1];
    printf("%s: exit(%d)\n", &thread_current ()->name, args[1]);
    thread_exit();
  }
  */
	uint32_t* args = ((uint32_t*) f->esp);
	if (!is_valid_ptr(args) || !is_valid_ptr(args + 1) ||
		!is_valid_ptr(args + 2) || !is_valid_ptr(args + 3)) {
		exit(-1);
		return;
	}

	switch (*args) {
		case SYS_EXIT:
			exit(*(args+1));
			break;
		case SYS_HALT:
			halt();
			break;
		case SYS_EXEC:
			f->eax = exec((char *) *(args+1));
			break;
		case SYS_WAIT:
			f->eax = wait(*(args+1));
			break;
		case SYS_CREATE:
			f->eax = create((char *) *(args+1), *(args+2));
			break;
		case SYS_REMOVE:
			f->eax = remove((char *) *(args+1));
			break;
		case SYS_OPEN:
			f->eax = open((char *) *(args+1));
			break;
		case SYS_FILESIZE:
			f->eax = filesize(*(args+1));
			break;
		case SYS_READ:
			f->eax = read(*(args+1), (void *) *(args+2), *(args+3));
			break;
		case SYS_WRITE:
			f->eax = write(*(args+1), (void *) *(args+2), *(args+3));
			break;
		case SYS_SEEK:
			seek(*(args+1), *(args+2));
			break;
		case SYS_TELL:
			f->eax = tell(*(args+1));
			break;
		case SYS_CLOSE:
			close(*(args+1));
			break;
		default:
			break;
	}
}

void halt(void) {
	shutdown_power_off();
}

pid_t exec(const char *process) {
	tid_t tid;
	struct thread *current;
	if (!is_valid_ptr(process)) {
		exit(-1);
	}

	current = thread_current();
	current->child_load_status = 0;
	tid = process_execute(process);
	lock_acquire(&current->lock_child);
	for (;current->child_load_status == 0;)
		cond_wait(&current->cond_child, &current->lock_child);
	if (current->child_load_status == -1)
		tid = -1;
	lock_release(&current->lock_child);
	return tid;
}

void exit(int status) {
	struct child_status *child;
	struct thread *current = thread_current();
	printf ("%s: exit(%d)\n", current->name, status);
	struct thread *parent = thread_get_by_id(current->parent_id);

	if (!parent)
		thread_exit();

	struct list_elem *e;
	for (e = list_tail(&parent->children);
	     (e = list_prev(e)) != list_head(&parent->children);) {
		child = list_entry(e, struct child_status, elem_child_status);
		if (child->child_id == current->tid) {
			lock_acquire(&parent->lock_child);
			child->is_exit_called = true;
			child->child_exit_status = status;
			lock_release(&parent->lock_child);
		}
	}

	thread_exit();
}

int wait(pid_t pid) {
	return process_wait(pid);
}

bool create(const char *file_name, unsigned size) {
	bool status;
	if (!is_valid_ptr(file_name))
		exit(-1);
	lock_acquire(&fs_lock);
	status = filesys_create(file_name, size);
	lock_release(&fs_lock);
	return status;
}

bool remove(const char *file_name) {
	bool status;
	if (!is_valid_ptr(file_name))
		exit(-1);
	lock_acquire(&fs_lock);
	status = filesys_remove(file_name);
	lock_release(&fs_lock);
	return status;
}

int open(const char *file_name) {
	struct file *f;
	struct file_descriptor *fd;
	int status = -1;
	if (!is_valid_ptr(file_name))
		exit(-1);
	lock_acquire(&fs_lock);
	f = filesys_open(file_name);
	if (!f)
		goto open_done;

	fd = calloc(1, sizeof *fd);
	fd->fd_num = allocate_fd();
	fd->owner = thread_current()->tid;
	fd->file_struct = f;
	list_push_back(&open_files, &fd->elem);
	status = fd->fd_num;

open_done:
	lock_release(&fs_lock);
	return status;
}

int read(int fd, void *buffer, unsigned size) {
	struct file_descriptor *_fd;
	int status = 0;
	if (!is_valid_ptr(buffer) || !is_valid_ptr(buffer + size - 1))
		exit(-1);
	lock_acquire(&fs_lock);
	if (fd == STDOUT_FILENO) {
		lock_release(&fs_lock);
		return -1;
	}
	if (fd == STDIN_FILENO) {
		unsigned i;
		for (i = 0; i != size; ++i)
			*(uint8_t *)(buffer + i) = input_getc();
		lock_release(&fs_lock);
		return size;
	}
	_fd = get_open_file(fd);
	if (_fd)
		status = file_read(_fd->file_struct, buffer, size);
	lock_release(&fs_lock);
	return status;
}

int write(int fd, const void *buffer, unsigned size) {
	struct file_descriptor *_fd;
	int status = 0;
	if (!is_valid_ptr(buffer) || !is_valid_ptr(buffer+size-1))
		exit(-1);
	lock_acquire(&fs_lock);
	if (fd == STDIN_FILENO) {
		lock_release(&fs_lock);
		return -1;
	}
	if (fd == STDOUT_FILENO) {
		putbuf(buffer, size);
		lock_release(&fs_lock);
		return size;
	}
	_fd = get_open_file(fd);
	if (_fd)
		status = file_write(_fd->file_struct, buffer, size);
	lock_release(&fs_lock);
	return status;
}

void seek(int fd, unsigned position UNUSED) {
	struct file_descriptor *_fd;
	lock_acquire(&fs_lock);
	_fd = get_open_file(fd);
	if (_fd)
		(void) file_tell(_fd->file_struct);
	lock_release(&fs_lock);
	return;
}

unsigned tell(int fd) {
	struct file_descriptor *_fd;
	int status = 0;
	lock_acquire(&fs_lock);
	_fd = get_open_file(fd);
	if (_fd)
		status = file_tell(_fd->file_struct);
	lock_release(&fs_lock);
	return status;
}

void close(int fd) {
	struct file_descriptor *_fd;
	lock_acquire(&fs_lock);
	_fd = get_open_file(fd);
	if (_fd && _fd->owner == thread_current()->tid)
		close_open_file(fd);
	lock_release(&fs_lock);
}

struct file_descriptor *get_open_file(int fd) {
	struct list_elem *e;
	struct file_descriptor *_fd;
	for (e = list_tail(&open_files);(e = list_prev(e)) != list_head(&open_files);) {
		_fd = list_entry(e, struct file_descriptor, elem);
		if (_fd->fd_num == fd)
			return _fd;
	}
	return NULL;
}

void close_open_file(int fd) {
	struct list_elem *e;
	struct list_elem *prev;
	struct file_descriptor *_fd;
	for (e = list_end(&open_files); e != list_head(&open_files); e=prev) {
		prev = list_prev(e);
		_fd = list_entry(e, struct file_descriptor, elem);
		if (_fd->fd_num == fd) {
			list_remove(e);
			file_close(_fd->file_struct);
			free(_fd);
			return;
		}
	}
}

int filesize(int fd) {
	struct file_descriptor *_fd;
	int status = -1;
	lock_acquire(&fs_lock);
	_fd = get_open_file(fd);
	if (_fd)
		status = file_length(_fd->file_struct);
	lock_release(&fs_lock);
	return status;
}

static int allocate_fd() {
	static int fd_current = 1;
	return ++fd_current;
}

/* user pointer check */
bool is_valid_ptr(const void *ptr) {
	struct thread *current = thread_current();

	if (ptr != NULL && is_user_vaddr(ptr))
		return (pagedir_get_page(current->pagedir, ptr)) != NULL;

	return false;
}

void close_file_by_owner(tid_t tid) {
	struct list_elem *e;
	struct list_elem *next;
	struct file_descriptor *fd;

	for (e = list_begin(&open_files); e != list_tail(&open_files); e = next) {
		next = list_next(e);
		fd = list_entry(e, struct file_descriptor, elem);
		if (fd->owner == tid) {
			list_remove(e);
			file_close(fd->file_struct);
			free(fd);
		}
	}
}
