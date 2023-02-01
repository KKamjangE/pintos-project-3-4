#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "lib/string.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include "devices/input.h"
#include "vm/vm.h"

typedef int pid_t;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void exit (int status) NO_RETURN;
void close (int fd);
static void check_address(void *addr);
static void halt (void) NO_RETURN;
static pid_t fork (const char *thread_name);
static int exec (const char *file);
static int wait (pid_t pid);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static int filesize (int fd);
static int read (int fd, void *buffer, unsigned length);
static int write (int fd, const void *buffer, unsigned length);
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void *mmap (void *addr, size_t length, int writable, int fd, off_t offset);
static bool is_invalid_mmap(void *addr, size_t length, int fd);
static void munmap(void *addr);
static bool is_invalid_fd(int fd);
static void intr_frame_cpy(struct intr_frame *f);
static void check_read_write(void *addr);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	switch (f->R.rax)
	{
	case SYS_HALT:
		halt();
		break;

	case SYS_EXIT:
		exit(f->R.rdi);
		break;

	case SYS_WAIT:
		f->R.rax = wait(f->R.rdi);
		break;

	case SYS_FORK:
		check_address((void *) f->R.rdi);
		intr_frame_cpy(f);
		f->R.rax = fork((const char *) f->R.rdi);
		break;

	case SYS_EXEC:
		check_address((void *) f->R.rdi);
		f->R.rax = exec((const char *) f->R.rdi);
		break;

	case SYS_CREATE:
		check_address((void *) f->R.rdi);
		f->R.rax = create((const char *) f->R.rdi, f->R.rsi);
		break;

	case SYS_REMOVE:
		check_address((void *) f->R.rdi);
		f->R.rax = remove((const char *) f->R.rdi);
		break;

	case SYS_OPEN:
		check_address((void *) f->R.rdi);
		f->R.rax = open((const char *) f->R.rdi);
		break;

	case SYS_FILESIZE:
		f->R.rax = filesize(f->R.rdi);
		break;

	case SYS_READ:
		//check_address((void *) f->R.rdi);
		check_read_write((void *) f->R.rsi);
		check_address((void *) f->R.rsi);
		f->R.rax = read(f->R.rdi, (void *) f->R.rsi, f->R.rdx);
		break;

	case SYS_WRITE:
		//check_address((void *) f->R.rdi);
		check_read_write((void *) f->R.rsi);
		check_address((void *) f->R.rsi);
		f->R.rax = write(f->R.rdi, (const void *) f->R.rsi, f->R.rdx);
		break;

	case SYS_SEEK:
		seek(f->R.rdi, f->R.rsi);
		break;

	case SYS_TELL:
		f->R.rax = tell(f->R.rdi);
		break;

	case SYS_CLOSE:
		close(f->R.rdi);
		break;
	
	case SYS_MMAP:
		f->R.rax = mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
	    break;

	case SYS_MUNMAP:
	    break;

	default:
		thread_exit ();
	}
}

static void
check_address(void *addr) {
	// TODO:
	// 1. 포인터 유효성 검증
	//		<유효하지 않은 포인터>
	//		- 널 포인터
	//		- virtual memory와 매핑 안 된 영역
	if (addr == NULL || is_kernel_vaddr(addr)) {
		// 2. 유저 영역을 벗어난 영역일 경우 프로세스 종료((exit(-1)))
		exit(-1);
	}
}

static void check_read_write(void *addr){
	struct page *check_page = spt_find_page(&thread_current()->spt, addr);
	if(!check_page->writable){
		exit(-1);
	}
}

static void
halt (void) {
	power_off();
}

void
exit (int status) {
	struct thread *curr = thread_current();
	curr->exit_status = status;
	printf ("%s: exit(%d)\n",curr->name, curr->exit_status);
	thread_exit();
}

static pid_t
fork (const char *thread_name) {
	pid_t pid = process_fork(thread_name, &thread_current()->user_tf);
	return pid;
}

static int
exec (const char *cmd_line) {
	char *cmd_line_cpy = palloc_get_page(PAL_ZERO);
	if (!cmd_line_cpy) {
		exit(-1);
	}

	int size = strlen(cmd_line) + 1; // 널 문자가 들어갈 공간
	strlcpy(cmd_line_cpy, cmd_line, size);

	if (process_exec(cmd_line_cpy) == -1) {
		exit(-1);
	}

	NOT_REACHED();
	return 1;
}

static int
wait (pid_t pid) {
	return process_wait(pid);
}

static bool
create (const char *file, unsigned initial_size) {
	return filesys_create(file, initial_size);
}

static bool
remove (const char *file) {
	return filesys_remove(file);
}

static int
open (const char *file) {

	struct thread *curr_t = thread_current();
	struct file *f = filesys_open(file);

	// TODO: next_fd가 필요할까?
	int fd = get_next_fd(curr_t->fdt);
	if (fd == -1)
	{
		file_close(f);
	}

	curr_t->next_fd = fd;

	if (f) { // FIXME: next_fd 갱신 로직 최적화
		curr_t->fdt[fd] = f;
		return fd;
	}
	else {
		return -1;
	}
}

static int
filesize (int fd) {
	struct thread *curr_t = thread_current();
	struct file *file_p = curr_t->fdt[fd];
	if (file_p == NULL) {
		return -1;
	}
	return file_length(file_p);
}

static int
read (int fd, void *buffer, unsigned size) {

	if (fd == STDIN_FILENO)
	{	
		uint32_t i;
		uint8_t key;
		for (i=0; i<size; i++)
		{
			key = input_getc();
			*(char *)buffer++ = key;
			if (key == '\0')
			{
				i++;
				break;
			}
		}
		return i;
	}
	else if (is_invalid_fd(fd)) {
		return -1;
	}
	else if (fd == STDOUT_FILENO)
	{
		return -1;
	}
	else
	{	
		/* 파일 디스크립터에 해당하는 파일을 가져와야한다. */
		struct thread *curr_thread = thread_current();
		struct file **fdt = curr_thread->fdt;
		// TODO: lock acquire failed
		struct file *curr_file = fdt[fd];
		if (curr_file == NULL)
		{
			return -1;
		}
		lock_acquire(&filesys_lock);
		off_t read_size = file_read(curr_file, buffer, size);
		lock_release(&filesys_lock);
		return read_size;
	}
}

static int
write (int fd, const void *buffer, unsigned size) {


	if (fd == STDOUT_FILENO) {
		putbuf(buffer, size);
		return size;
	}	
	else if (fd == STDIN_FILENO) {
		return 0;
	}
	else if (is_invalid_fd(fd)) {
		return 0;
	}
	else 
	{
		int read_count;
		struct thread *curr_thread = thread_current();
		struct file **fdt = curr_thread->fdt;
		struct file *curr_file = fdt[fd];

		if (curr_file == NULL)
		{
			return 0;
		}
		lock_acquire(&filesys_lock);
		read_count = file_write(curr_file, buffer, size);
		lock_release(&filesys_lock);
		return read_count;
	}
}

static void
seek (int fd, unsigned position) {
	struct thread *curr_thread = thread_current();
	struct file **fdt = curr_thread->fdt;
	struct file *curr_file = fdt[fd];
	
	file_seek (curr_file, position);
}

static unsigned
tell (int fd) {
	struct thread *curr_thread = thread_current();
	struct file **fdt = curr_thread->fdt;
	struct file *curr_file = fdt[fd];
	return file_tell(curr_file);
}

void
close (int fd) {// FIXME: next_fd 갱신 로직 최적화
	if (!is_invalid_fd(fd))
	{	
		struct thread *curr = thread_current();
		struct file *file_p = curr->fdt[fd];
		if (file_p != NULL)
		{
			file_close(file_p);
			curr->fdt[fd] = NULL;
		}
	}
}

// static void *
// mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
// 	if(is_invalid_mmap(addr, length, fd)){
// 		return NULL;
//     }

// 	size_t copy_length = length;
// 	while (copy_length > 0)
// 	{
// 		/* fix: init인자 값 수정해야함 */
// 		if(!vm_alloc_page_with_initializer(VM_FILE, addr, writable, NULL, NULL)){
// 			return NULL;
// 		}
		
// 		copy_length -= PGSIZE;
// 	}
// }

// static size_t
// get_copy_length (size_t *length) {
// 	size_t return_value = 0;

// 	if(*length >= PGSIZE) {
// 		*length -= PGSIZE;
// 		return_value = PGSIZE;
// 	} else {
// 		return_value = length;
// 		*length = 0;
// 	}

// 	return return_value;
// }

static void *
mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
	/* TODO:
			- 인자들의 예외 처리
			- 예외 처리를 통과하면 do_mmap함수를 호출한다
			- file을 가져와서 do_mmap의 인자로 넘겨준다
	 */
	if(!is_invalid_mmap(addr, length, fd)){
		return NULL;
	}
	struct file *file = thread_current()->fdt[fd];
	if(file == NULL){ // file 유효성 검사
		return NULL;
	}

	return do_mmap(addr, length, writable, file, offset);
}

static bool
is_invalid_mmap(void *addr, size_t length, int fd){
	/* TODO: 
	 		- addr이 0인 경우 OK
			- fd로 열린 파일이 0바이트인 경우 OK
	 		- addr이 페이지 정렬되지 않은 경우 OK
	 		- 이미 있는 페이지에 덮어씌울 경우 OK
	 		- length가 0인 경우 OK
	 		- fd가 stdin/stdout인 경우 OK
	*/
	if(addr == 0){
		return false;
	}
	else if(filesize(fd) == 0){
		return false;
	}
	else if(pg_ofs(addr) != 0){
		return false;
	}
	else if(spt_find_page(&thread_current()->spt, addr)){
		return false;
	}
	else if(length <= 0){
		return false;
	}
	else if(fd == 1 || fd == 0){
		return false;
	}
	else{
		return true;
	}
}

static void munmap(void *addr){
	do_munmap(addr);
}

static bool
is_invalid_fd(int fd)
{
	return fd < 0 || fd >= FD_LIMIT_LEN;
}

static void
intr_frame_cpy(struct intr_frame *f) {
	struct thread *curr_thread = thread_current();

	memcpy(&curr_thread->user_tf, f, sizeof(struct intr_frame));
}