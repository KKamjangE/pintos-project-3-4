#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "lib/user/syscall.h"
#include "threads/malloc.h"

#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);
static int parse_file_name(char **argv, const char *file_name);
static void pass_arguments(int argc, char **argv, struct intr_frame *if_);
int get_next_fd(struct file **fdt);
static struct thread *get_child_with_id(tid_t child_tid);

/* General process initializer for initd and other process. */
static void
process_init(void)
{
	// struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	char *argv[64] = {
		NULL,
	};

	parse_file_name(argv, file_name);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create(argv[0], PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
	{
		palloc_free_page(fn_copy);
	}
	return tid;
}

/* A thread function that launches first user process. */
static void
initd(void *f_name)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	process_init();

	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_)
{
	struct thread *curr_thread = thread_current();
	curr_thread->user_tf = *if_;
	/* Clone current thread to new thread.*/
	tid_t tid = thread_create(name,
							  PRI_DEFAULT, __do_fork, curr_thread);

	// TODO:
	if (tid == -1)
	{
		return TID_ERROR;
	}

	struct thread *child_thread = get_child_with_id(tid);

	if (!child_thread || child_thread->exit_status == -1)
	{
		return TID_ERROR;
	}

	sema_down(&child_thread->fork_sema);
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *new_page;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* 2. Resolve VA from the parent's page map level 4. */
	if (is_kernel_vaddr(va))
	{
		return true;
	}
	parent_page = pml4_get_page(parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	new_page = palloc_get_page(PAL_USER);

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(new_page, parent_page, PGSIZE);
	// writable = is_writable(pml4e_walk(parent->pml4, parent_page, 0));
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page(current->pml4, va, new_page, writable))
	{
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(new_page);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork(void *aux)
{
	struct intr_frame if_;
	struct thread *parent = (struct thread *)aux;
	struct thread *current = thread_current(); // 자식 쓰레드
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = &parent->user_tf;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy(&if_, parent_if, sizeof(struct intr_frame));
	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate(current);
#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
	{
		goto error;
	}
#endif
	// ! 작업
	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	/* 부모의 file을 자식에 복사 */
	struct file **parent_fdt = parent->fdt;
	struct file **child_fdt = current->fdt;

	child_fdt[0] = (struct file *) 1;
	child_fdt[1] = (struct file *) 2;

	for (int i = 2; i < FD_LIMIT_LEN; i++)
	{
		struct file *f = parent_fdt[i];
		if (f)
		{
			child_fdt[i] = file_duplicate(f);
		}
	}

	current->next_fd = parent->next_fd;

	process_init();

	/* Finally, switch to the newly created process. */
	if (succ)
	{
		sema_up(&current->fork_sema);
		do_iret(&if_);
	}
error:
	current->exit_status = -1;
	sema_up(&current->fork_sema);
	exit(-1);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup();

	/* And then load the binary */
	success = load(file_name, &_if);
	// hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true);

	/* If load failed, quit. */
	palloc_free_page(file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret(&_if);
	NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid)
{

	struct thread *curr_thread = thread_current(); /* 부모 쓰레드 */
	struct list *child_list = &curr_thread->child_list;

	if (!list_empty(child_list))
	{
		struct thread *child_thread;
		struct list_elem *e;

		for (e = list_begin(child_list); e != list_end(child_list); e = list_next(e))
		{
			struct thread *tmp_thread = list_entry(e, struct thread, child_elem);
			if (tmp_thread->tid == child_tid)
			{
				child_thread = tmp_thread;
				break;
			}
		}

		if (child_thread == NULL)
			return -1;

		sema_down(&child_thread->wait_sema);
		list_remove(&child_thread->child_elem);
		sema_up(&child_thread->free_sema);
		return child_thread->exit_status;
	}

	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{

	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	struct thread *curr = thread_current();

	/* 현재 쓰레드의 fdt에 있는 파일을 close */
	struct file **fdt = curr->fdt;

	fdt[0] = NULL;
	fdt[1] = NULL;
	for (int i = 2; i < FD_LIMIT_LEN; i++)
	{
		close(i);
	}
	// fdt 반환
	palloc_free_multiple(fdt, 3);

	sema_up(&curr->wait_sema);
	sema_down(&curr->free_sema);
	process_cleanup();

	free(curr->spt.spt_hash_table);
	curr->spt.spt_hash_table = NULL;
}

/* Free the current process's resources. */
static void
process_cleanup(void)
{
	struct thread *curr = thread_current();

#ifdef VM
	supplemental_page_table_kill(&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL)
	{
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate(NULL);
		pml4_destroy(pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
	/* Activate thread's page tables. */
	pml4_activate(next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* FILE_NAME에서 현재 스레드로 ELF 실행 파일을 로드합니다.
 * 실행 파일의 진입점을 *RIP에 저장하고 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공하면 true를 반환하고, 그렇지 않으면 false를 반환합니다. */
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* 커맨드 라인 파싱 */
	char *argv[64] = {
		NULL,
	};
	int argc;
	argc = parse_file_name(argv, file_name);

	/* Allocate and activate page directory. */
	/* 페이지 디렉토리를 할당하고 활성화합니다. */
	t->pml4 = pml4_create();
	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current());

	/* Open executable file. */
	/* 실행 파일을 Open합니다. */
	file = filesys_open(file_name);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", file_name);
		goto done;
	}

	/* 실행파일 저장 */
	int fd = get_next_fd(t->fdt);
	// if (fd == -1)
	// {
	// 	file_close(file);
	// 	goto done;
	// }
	t->next_fd = fd;
	t->fdt[fd] = file;
	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	/* 프로그램 헤더를 읽습니다. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++)
	{
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type)
		{
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			/* 이 세그먼트를 무시합니다. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file))
			{
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0)
				{
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					/* 정상 세그먼트.
					 * 디스크에서 초기 부분을 읽고 나머지 부분을 0으로 합니다. */
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				}
				else
				{
					/* Entirely zero.
					 * Don't read anything from disk. */
					/* 완전한 zero.
					 * 디스크에서 아무것도 읽지 마십시오. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable)){
					goto done;
				}
			}
			else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack(if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* 유저 스택에 인자 저장하기 */
	pass_arguments(argc, argv, if_);

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// if (!success) {
	// 	file_close (file);
	// }

	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
/* 주소 업데이트 시 파일의 OFS 오프셋에서 시작하는 세그먼트를 로드합니다.
 * 총 가상 메모리의 READ_BYTS + ZERO_BYTS 바이트는 다음과 같이 초기화됩니다.
 *
 * - READ_BYTES bytes는 UPAGE의 오프셋 OFS에서 시작하는 FILE을 읽어야 합니다.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES는 0이어야 합니다.
 *
 * 쓰기 가능한 페이지인 경우 이 기능으로 초기화된 페이지는 사용자 프로세스에서
 * 쓰기 가능해야 하며 그렇지 않은 경우 읽기 전용이어야 합니다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류 또는 디스크 읽기 오류가
 * 발생하면 false를 반환합니다. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		/* 이 페이지를 채우는 방법을 계산하십시오.
		 * 우리는 FILE에서 PAGE_READ_BYTES 바이트를 읽고 마지막
		 * PAGE_ZERO_BYTES 바이트를 0으로 만들 것이다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		/* 메모리에서 한 페이지를 가져오십시오. */
		uint8_t *kpage = palloc_get_page(PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		/* 이 페이지를 로드합니다. */
		if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
		{
			palloc_free_page(kpage);
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		/* 프로세스의 주소 공간에 페이지를 추가합니다. */
		if (!install_page(upage, kpage, writable))
		{
			printf("fail\n");
			palloc_free_page(kpage);
			return false;
		}

		/* Advance. */
		/* 발전? */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
/* USER_STACK에서 0으로 초기화된 페이지를 매핑하여 최소 스택 생성 */
static bool
setup_stack(struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page(kpage);
	}

	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
/* 사용자 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의
 * 매핑을 페이지 테이블에 추가합니다.
 * 쓰기 기능이 가능하면 사용자 프로세스가 페이지를 수정할 수 있습니다.
 * 그렇지 않으면 읽기 전용입니다.
 * UPAGE가 아직 매핑되지 않아야 합니다.
 * KPAGE는 아마도 palloc_get_page()를 가진 사용자 풀에서
 * 얻은 페이지일 것이다.
 * 성공 시 true를 반환하고, UPAGE가 이미 매핑되었거나
 * 메모리 할당이 실패한 경우 false를 반환합니다. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	/* 해당 가상 주소에 아직 페이지가 없는지 확인한 다음
	 * 해당 페이지를 매핑합니다. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

bool
lazy_load_segment(struct page *page, void *aux)
{
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	/* TODO: 파일에서 세그먼트 로드 */
	/* TODO: 이 기능은 주소 VA에서 첫 번째 페이지 오류가 발생할 때 호출됩니다. */
	/* TODO: VA는 이 함수를 호출할 때 사용할 수 있습니다. */
	struct lazy_load_aux *lazy_load_aux = (struct lazy_load_aux *)aux;
	void *kpage = page->frame->kva;
	size_t page_read_bytes = lazy_load_aux->page_read_bytes;
	struct file * load_file = lazy_load_aux->file;
	off_t load_oft = lazy_load_aux->ofs;

	file_seek(load_file,load_oft);

	if (file_read(load_file, kpage, page_read_bytes) != (int)page_read_bytes) {
		// FIXME: kpage 반환 필요?
		// spt_kill 참고
		free(aux);
		return false;
	}
	memset(kpage + page_read_bytes, 0, lazy_load_aux->page_zero_bytes);

	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
 /* 주소 upage에서 파일의 OFS 오프셋에서 시작하는 세그먼트를 로드합니다.
 * 총 가상 메모리의 READ_BYTS + ZERO_BYTS 바이트는 다음과 같이 초기화됩니다.
 *
 * - READ_BYTES bytes는 UPAGE의 오프셋 OFS에서 시작하는 FILE을 읽어야 합니다.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES는 0이어야 합니다.
 *
 * 쓰기 가능한 페이지인 경우 이 기능으로 초기화된 페이지는 사용자 프로세스에서
 * 쓰기 가능해야 하며 그렇지 않은 경우 읽기 전용이어야 합니다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류 또는 디스크 읽기 오류가
 * 발생하면 false를 반환합니다. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	struct supplemental_page_table curr_spt = thread_current()->spt;

	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		/* TODO: lazy_load_segment에 정보를 전달하도록 aux를 설정합니다. */
		int malloc_size = sizeof(struct lazy_load_aux);
		struct lazy_load_aux *aux = malloc(malloc_size);
		*aux = (struct lazy_load_aux) {
			.file = file,
			.ofs = ofs,
			.page_read_bytes = page_read_bytes,
			.page_zero_bytes = page_zero_bytes,
		};

		//원래 load_segment 에 있던 함수의 원형에서 VM_ANON 을 입력하도록 되어있음.
		if (!vm_alloc_page_with_initializer(VM_ANON, upage,	writable, lazy_load_segment, aux)){
			free(aux);
			return false;
		}

		struct page * curr_page = spt_find_page(&curr_spt, upage);
		curr_page->aux_size = malloc_size;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

// setup_stack에서 vm_alloc_page를 사용하면 이 함수를 사용할 필요가 없다.
/* stack 초기화 */
static bool
init_stack(struct page *page UNUSED, void *aux){
	struct intr_frame *if_ = aux;

	if_->rsp = USER_STACK;

	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
/* USER_STACK에 스택 페이지를 생성합니다. 성공하면 true를 반환합니다. */
static bool
setup_stack(struct intr_frame *if_)
{
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: 스택을 stack_bottom에 매핑하고 페이지를 즉시 할당합니다.
	 * TODO: 성공하면 rsp를 적절하게 설정합니다.
	 * TODO: 페이지를 스택으로 표시해야 합니다. */
	/* TODO: Your code goes here */
	struct supplemental_page_table curr_spt = thread_current()->spt;

	void *aux = if_;
	success = vm_alloc_page_with_initializer(VM_ANON, stack_bottom, true, init_stack, aux);
	/* vm_alloc_page를 사용해서 init_stack과 aux를 사용하지 않아도된다.
	 * 즉시 claim하기 때문이다.
	 */
	// success = vm_alloc_page(VM_ANON, stack_bottom, true);

    if (success) {
		vm_claim_page(stack_bottom); // 브리기태임 굿 ! 영화 무비 공부 스터디 (4조 이름)
		// if_->rsp = USER_STACK; // vm_alloc_page 사용시 추가
	}

	struct page * curr_page = spt_find_page(&curr_spt, stack_bottom);
	curr_page->aux_size = -1;

	return success;
}
#endif /* VM */

/* next_fd를 찾아주는 함수 */
int get_next_fd(struct file **fdt)
{
	for (int i = 2; i < FD_LIMIT_LEN; i++)
	{
		struct file *f = fdt[i];
		if (!f)
		{
			return i;
		}
	}
	return -1;
}

/* 공백을 구분자로 하여 FILE_NAME(command line)을 단어 단위로 나눈다.
	{ 파일 이름, 인자, 인자, ... }
	분할된 단어(token)의 개수(argc)를 반환한다. */
static int
parse_file_name(char **argv, const char *file_name)
{
	int argc = 0;
	char *token, *save_ptr;
	const char DELIMITER[2] = " ";

	for (token = strtok_r(file_name, DELIMITER, &save_ptr); token != NULL;
		 token = strtok_r(NULL, DELIMITER, &save_ptr))
	{
		argv[argc++] = token;
	}

	return argc;
}

/* 인자와 파일 이름을 사용자 스택에 차례대로 저장하고,
	%rsi가 argv를 가리키도록 하고, %rdi는 argc를 가리키도록 한다. */
static void
pass_arguments(int argc, char **argv, struct intr_frame *if_)
{
	/* 인자를 커맨드 라인의 오른쪽에서 왼쪽순으로
		스택에 넣기 위해 argv의 뒤에서부터 순회를 시작 */
	for (int i = argc - 1; i >= 0; i--)
	{
		int arg_size = strlen(argv[i]) + 1;
		if_->rsp -= arg_size;
		memcpy((void *)if_->rsp, (void *)argv[i], arg_size);

		/* 두 번째 순회에서 스택에 각 주소값을 넣어야 하기 때문에
			argv[i]에 주소값을 재할당 */
		argv[i] = (char *)if_->rsp;
	}

	/* 바이트 정렬을 위해 주소값을 8의 배수로 맞추고 남는 공간은 0으로 채우기 */
	int mod = if_->rsp % ALIGNMENT;
	if (mod)
	{
		if_->rsp -= mod;
		memset((uint8_t *)if_->rsp, 0, mod);
	}

	/* null pointer sentinel (required by the C standard) */
	if_->rsp -= CHARP_SIZE;
	memset((void *)if_->rsp, 0, CHARP_SIZE);

	/* 각 인자의 주소값을 스택에 넣기 */
	for (int i = argc - 1; i >= 0; i--)
	{
		if_->rsp -= CHARP_SIZE;
		memcpy((void *)if_->rsp, &argv[i], CHARP_SIZE);
	}

	/* 마지막으로, 여느 스택 프레임과 동일한 구조를 갖추기 위해
		가짜 반환 주소(return address) 넣기 */
	if_->rsp -= ADDR_SIZE; /* stack top */
	memset((void *)if_->rsp, 0, ADDR_SIZE);

	/* rdi, rsi 초기화 */
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp + ADDR_SIZE; /* argv[0]의 주소 */
}

struct thread *
get_child_with_id(tid_t child_tid)
{
	struct thread *curr_thread = thread_current();
	struct list *child_list = &curr_thread->child_list;

	if (!list_empty(child_list))
	{
		struct list_elem *e;

		for (e = list_begin(child_list); e != list_end(child_list); e = list_next(e))
		{
			struct thread *child_thread = list_entry(e, struct thread, child_elem);
			if (child_thread->tid == child_tid)
				return child_thread;
		}
	}
	return NULL;
}