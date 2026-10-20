#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
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
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static void argument_stack (char **argv, int argc, struct intr_frame *if_);   /* ← 추가 */

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* 스레드 이름은 명령줄 전체("args-single onearg")가 아니라
	 * 프로그램 이름("args-single")만 쓴다. 종료 메시지에 이 이름이 찍힌다. */
	char thread_name[16];
	strlcpy (thread_name, file_name, sizeof thread_name);
	char *save_ptr;
	strtok_r (thread_name, " ", &save_ptr);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (thread_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* Clone current thread to new thread.*/
        
	struct thread *child = NULL;
	struct list_elem *e;
	struct thread *parent = thread_current();
	/*자식이 부모와 같은 곳에서 시작하기 위해 레지스터 복사*/
	memcpy (&thread_current()->parent_if,if_, sizeof (struct intr_frame));
	/*자식 스레드 */
        tid_t tid =thread_create (name,PRI_DEFAULT, __do_fork, thread_current ());

        if(tid == TID_ERROR )
	{
		return TID_ERROR;
	}

	 /*방금 만든 자식의 struct thread를 찾는 반복문*/
	for (e = list_begin (&parent->children); e != list_end (&parent ->children);
		e = list_next (e)) {
		struct thread *t = list_entry (e, struct thread, child_elem);
		if (t->tid == tid)
		{
			child = t;
			break;
		}
	}
	if (child == NULL)
		return -1;
	/*부모가 찾은 자식의 세마포어로 기다림*/
	sema_down (&child->fork_sema);

	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;
       /* 1. TODO: If the parent_page is kernel page, then return immediately.부모 페이지가 커널페이지면 바로 종료  */
       if (is_kernel_vaddr(va) )
       {
	return true;
       }
       /* 2. Resolve VA from the parent's page map level 4. */
        //부모의 pm14에서 va에 해당하는 실제 페이지를 찾아라
       parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to 자식 PAL_USER페이지를 새로 할당후 그 결과를 NEWPAGE에 넣어라 palloc_get_page(PAL_USER)
	 *    TODO: NEWPAGE. */
        newpage = palloc_get_page (PAL_USER);
	if (newpage == NULL)
	{
		return false;
	}
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result).부모 페이지를 새 페이지에 복사하고 부모 페이지가 쓸 수 있는지 확인해서 WRITABLE에다가 넣어라  *///memcpy(newpage, parent_page, PGSIZE)writable 여부는 duplicate_pte의 첫 번째 인자인 부모의 pte로 확인해. is_writable(pte)
        memcpy(newpage, parent_page, PGSIZE);
        writable = is_writable(pte);
	
	/* 5. Add new page to child's page table at address VA with WRITABLE자식 페이지 테이블의 va 주소에 새 페이지를 writable 권한으로 매핑.
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. *///페이지 삽입에 실패하면 에러 처리를 해라.이미 할당해둔 newpage를 palloc_free_page(newpage)로 해제하고 false를 리턴
		palloc_free_page (newpage);
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
__do_fork (void *aux) {
	struct intr_frame if_;//자식 상자
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) 부모의 레지스터 상태(if_)← 이걸 못 보냄*/
	struct intr_frame *parent_if = &parent->parent_if;
	bool succ = true;

	/* 레지스터 상태를 전부 가져와서 부모와 똑같이 실행 */
	memcpy (&if_,parent_if, sizeof (struct intr_frame));
	/* 자식 반환값 0 */
        if_.R.rax = 0;
	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif
	/* 3. 파일 디스크립터 복제 (두 번째 주석 부분) */
	/* 4. 부모를 깨우고 유저 모드로 진입 */
	
	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
        for (int i = 2;i < parent->next_fd; i++)
	{
		if(!parent->fd_table[i])
		{
                  continue;
		}
	current->fd_table[i] = file_duplicate(parent->fd_table[i]);

	if (!current->fd_table[i])
	{
		goto error;
	}
        }
	current->next_fd = parent->next_fd;


	process_init ();
        
	/* Finally, switch to the newly created process. */
	if (succ){/*성공하면 부모한테 신호 보내기*/
		sema_up(&current->fork_sema);
		do_iret (&if_);}
error:
        sema_up (&current->fork_sema);
	thread_exit ();
}



/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
    char *command = f_name;

    /*]프로그램 이름을 포함해 최대 32개. */
    char *argv[32];
    uintptr_t user_argv[32];
    int argc = 0;

    char *save_ptr;
    char *token;

    /* 1. 명령행을 공백으로 분리 */
    for (token = strtok_r (command, " ", &save_ptr);
         token != NULL;
         token = strtok_r (NULL, " ", &save_ptr)) {

        if (argc == 32) {
            palloc_free_page (command);
            return -1;
        }

        argv[argc++] = token;
    }

    if (argc == 0) {
        palloc_free_page (command);
        return -1;
    }

    /*
     * 문자열 + 정렬 여유 + argv 포인터들
     * + NULL 포인터 + 가짜 반환 주소.
     */
    size_t stack_bytes = 7 + (argc + 2) * sizeof (uintptr_t);

    for (int i = 0; i < argc; i++) {
        stack_bytes += strlen (argv[i]) + 1;
    }

    if (stack_bytes > PGSIZE) {
        palloc_free_page (command);
        return -1;
    }

    /* 2. 사용자 프로그램의 초기 레지스터 상태를 준비한다. */
    struct intr_frame if_;
    memset (&if_, 0, sizeof if_);

    if_.ds = if_.es = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;

    process_cleanup ();

    /* 전체 명령행이 아닌 프로그램 이름만 전달한다. */
    if (!load (argv[0], &if_)) {
        palloc_free_page (command);
        return -1;
    }

    /* 3. 인자 문자열을 사용자 스택에 복사한다. */
    for (int i = argc - 1; i >= 0; i--) {
        size_t length = strlen (argv[i]) + 1;

        if_.rsp -= length;
        memcpy ((void *) if_.rsp, argv[i], length);

        /* 사용자 스택에 들어간 문자열의 주소를 기록한다. */
        user_argv[i] = if_.rsp;
    }

    /* 4. 스택을 8바이트 경계에 맞춘다. */
    while (if_.rsp % 8 != 0) {
        if_.rsp--;
        *(uint8_t *) if_.rsp = 0;
    }

    /* 5. argv[argc] = NULL을 넣는다. */
    uintptr_t zero = 0;

    if_.rsp -= sizeof zero;
    memcpy ((void *) if_.rsp, &zero, sizeof zero);

    /* 6. 문자열 주소들을 넣어서 argv 배열을 만든다. */
    for (int i = argc - 1; i >= 0; i--) {
        if_.rsp -= sizeof (uintptr_t);
        memcpy ((void *) if_.rsp,
                &user_argv[i],
                sizeof (uintptr_t));
    }

    /* 7. _start(argc, argv)에 전달할 레지스터를 설정한다. */
    if_.R.rdi = argc;
    if_.R.rsi = if_.rsp;

    /* 8. 가짜 반환 주소를 넣는다. */
    if_.rsp -= sizeof zero;
    memcpy ((void *) if_.rsp, &zero, sizeof zero);

    /* 문자열을 사용자 스택에 복사했으므로 원본은 해제한다. */
    palloc_free_page (command);

    /* 9. 준비한 상태로 사용자 프로그램을 실행한다. */
    do_iret (&if_);
    NOT_REACHED ();
}

/* argv[0..argc-1] 문자열들을 x86-64 호출 규약에 맞게 사용자 스택에 쌓고,
 * main(argc, argv)의 인자가 되도록 rdi, rsi 레지스터를 채운다. */
static void
argument_stack (char **argv, int argc, struct intr_frame *if_) {
	char *arg_addr[64];     /* 스택에 복사한 각 문자열의 사용자 주소 */
	int i;

	/* 1. 문자열 자체를 스택에 복사 (뒤의 인자부터, '\0' 포함) */
	for (i = argc - 1; i >= 0; i--) {
		size_t len = strlen (argv[i]) + 1;
		if_->rsp -= len;
		memcpy ((void *) if_->rsp, argv[i], len);
		arg_addr[i] = (char *) if_->rsp;
	}

	/* 2. rsp를 8의 배수로 내린다 (word-align). 빈 칸은 0으로 채운다. */
	while (if_->rsp % 8 != 0) {
		if_->rsp--;
		*(uint8_t *) if_->rsp = 0;
	}

	/* 3. argv[argc] = NULL (C 표준: argv 배열은 NULL로 끝난다) */
	if_->rsp -= sizeof (char *);
	*(char **) if_->rsp = NULL;

	/* 4. argv[argc-1] ~ argv[0]: 문자열들의 주소를 쌓는다 */
	for (i = argc - 1; i >= 0; i--) {
		if_->rsp -= sizeof (char *);
		*(char **) if_->rsp = arg_addr[i];
	}

	/* 5. main(argc, argv) 인자 전달: 1번째 인자 rdi, 2번째 인자 rsi */
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp;          /* 방금 쌓은 argv[0]의 위치 = argv 배열 시작 */

	/* 6. 가짜 반환 주소 0. 보통 함수처럼 스택 맨 위에 반환 주소가 있는 모양을 맞춘다. */
	if_->rsp -= sizeof (void *);
	*(void **) if_->rsp = NULL;
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
int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current ();
	struct thread *child = NULL;
	struct list_elem *e;

	/* 1. 내 자식 목록에서 child_tid를 가진 스레드를 찾는다. */
	for (e = list_begin (&curr->children); e != list_end (&curr->children);
			e = list_next (e)) {
		struct thread *t = list_entry (e, struct thread, child_elem);
		if (t->tid == child_tid) {
			child = t;
			break;
		}
	}
	/* 내 자식이 아니거나, 이미 wait해서 목록에서 빠졌으면 바로 -1 */
	if (child == NULL)
		return -1;

	/* 2. 자식이 끝날 때까지 잠든다. 자식이 process_exit에서 up 해 준다. */
	sema_down (&child->wait_sema);

	/* 3. 자식이 남긴 종료 코드를 읽고, 다시 wait하지 못하게 목록에서 뺀다. */
	int status = child->exit_status;
	list_remove (&child->child_elem);

	/* 4. "다 읽었으니 이제 사라져도 돼"라고 자식에게 알린다. */
	sema_up (&child->exit_sema);
	return status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	/* 사용자 프로세스(페이지 테이블이 있는 스레드)일 때만 종료 메시지를 찍는다.
	 * 테스트는 정확히 "이름: exit(코드)" 한 줄을 기대한다. */
	if (curr->pml4 != NULL)
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
		palloc_free_page (curr->fd_table);

	process_cleanup ();// 지금 프로세스의 유저 메모리를 전부 지움

	/* 내가 먼저 죽으면 내 자식들을 기다려 줄 부모가 없으니 풀어 준다. */
	struct list_elem *e;
	for (e = list_begin (&curr->children); e != list_end (&curr->children);
			e = list_next (e))
		sema_up (&list_entry (e, struct thread, child_elem)->exit_sema);

	/* 기다리는 부모를 깨우고, 부모가 exit_status를 읽을 때까지 기다린다.
	 * 이 기다림이 없으면 부모가 읽기 전에 내 struct thread가 해제될 수 있다. */
	sema_up (&curr->wait_sema);
	sema_down (&curr->exit_sema);
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
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

struct ELF64_PHDR {
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

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
				bool writable = (phdr.p_flags & PF_W) != 0;
			uint64_t file_page = phdr.p_offset & ~PGMASK;
			uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
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
static bool install_page (void *upage, void *kpage, bool writable);

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
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
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
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
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
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
