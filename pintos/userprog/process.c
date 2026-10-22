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
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

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

/* ===================================================================
 * get_child: 내 자식 목록(children)에서 TID 를 가진 자식 스레드를 찾는다.
 *   자식은 thread_create() 에서 부모의 children 리스트에 등록된다.
 *   못 찾으면 NULL.
 * =================================================================== */
static struct thread *
get_child (tid_t tid) {
	struct list *l = &thread_current ()->children;
	for (struct list_elem *e = list_begin (l); e != list_end (l); e = list_next (e)) {
		/* list_elem(child_elem) 주소로부터 그걸 품고 있는 struct thread 를 꺼낸다 */
		struct thread *t = list_entry (e, struct thread, child_elem);
		if (t->tid == tid)
			return t;
	}
	return NULL;
}

/* ===================================================================
 * process_fork: [부모가 실행] 나를 복제한 자식 프로세스를 만든다.
 *   반환값: 성공하면 자식 tid(= pid), 실패하면 TID_ERROR(-1).
 *   흐름: 레지스터 맡겨 두기 → 자식 스레드 생성 → 자식이 복제를 끝낼 때까지 대기
 * =================================================================== */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct thread *cur = thread_current ();

	/* ① 부모의 사용자 레지스터(syscall_handler 가 받은 f)를 내 구조체에 복사해 둔다.
	 *    자식(__do_fork)이 parent->parent_if 로 꺼내 간다.
	 *    부모는 아래 sema_down 에서 잠들어 있으므로 그동안 값이 바뀌지 않는다. */
	cur->parent_if = *if_;

	/* ② 새 스레드를 만든다. 그 스레드는 __do_fork(cur) 를 실행한다.
	 *    인자로 부모 자신(cur)을 넘겨서, 자식이 부모의 메모리·파일을 복사할 수 있게 한다. */
	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, cur);
	if (tid == TID_ERROR)
		return TID_ERROR;

	/* ③ 방금 만든 자식을 찾아서, 자식이 복제를 끝낼 때까지 잠든다.
	 *    기다리는 이유:
	 *      - 부모가 먼저 돌아가 메모리를 바꾸면 자식이 바뀐 내용을 복사할 수 있다.
	 *      - 복제가 실패했는지 알아야 -1 을 돌려줄 수 있다. */
	struct thread *child = get_child (tid);
	sema_down (&child->fork_sema);

	/* ④ 복제 실패: 자식은 이미 exit(-1) 하는 중이다.
	 *    process_wait 로 거둬 줘야 자식 구조체가 정리된다(안 하면 메모리 누수). */
	if (!child->fork_success) {
		process_wait (tid);
		return TID_ERROR;
	}

	/* ⑤ 성공: 자식 tid 를 돌려준다. Pintos 는 프로세스 1개 = 스레드 1개라 tid 가 곧 pid. */
	return tid;
}

#ifndef VM
/* ===================================================================
 * duplicate_pte: [자식이 실행] 부모의 페이지 "한 장"을 자식에게 복사한다.
 *   __do_fork 의 pml4_for_each 가 부모 페이지 테이블에 있는
 *   페이지마다 이 함수를 한 번씩 불러 준다. false 를 돌려주면 fork 실패.
 *   핵심: 같은 가상 주소(va)에 "새 물리 페이지"를 붙인다.
 *         → 주소는 같지만 메모리는 따로라서 서로의 값을 못 바꾼다.
 * =================================================================== */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();    /* 자식 */
	struct thread *parent = (struct thread *) aux; /* 부모 */
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. 커널 영역은 모든 프로세스가 같이 쓰고, pml4_create 가 이미 넣어 줬다.
	 *    복사하지 않고 "성공"으로 넘어간다(false 면 fork 전체가 실패하므로 true). */
	if (is_kernel_vaddr (va))
		return true;

	/* 2. 부모의 가상 주소 va 가 실제로 가리키는 페이지(커널에서 접근 가능한 주소)를 얻는다. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. 자식용 새 페이지를 사용자 메모리 풀에서 받는다. 메모리가 없으면 fork 실패. */
	newpage = palloc_get_page (PAL_USER);
	if (newpage == NULL)
		return false;

	/* 4. 부모 페이지 내용 4KB 를 그대로 복사하고,
	 *    쓰기 권한도 부모와 같게 맞춘다(코드 영역은 읽기 전용, 스택·데이터는 쓰기 가능). */
	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable (pte);

	/* 5. 자식 페이지 테이블에 "va → newpage" 매핑을 등록한다.
	 * 6. 실패하면 받은 페이지를 돌려주고 fork 실패를 알린다. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		palloc_free_page (newpage);
		return false;
	}
	return true;
}
#endif

/* ===================================================================
 * __do_fork: [자식이 실행] 부모를 복제해서 자식을 완성하고 사용자 모드로 출발한다.
 *   process_fork 의 thread_create 로 만들어진 새 스레드가 처음 실행하는 함수.
 *   복사하는 것: ① 레지스터 ② 메모리(페이지 테이블) ③ 열린 파일
 * =================================================================== */
static void
__do_fork (void *aux) {
	struct intr_frame if_;                          /* 자식이 사용자 모드로 돌아갈 때 쓸 레지스터 */
	struct thread *parent = (struct thread *) aux;  /* process_fork 가 넘겨 준 부모 */
	struct thread *current = thread_current ();     /* 나(자식) */

	/* 부모가 process_fork 에서 맡겨 둔 사용자 레지스터 */
	struct intr_frame *parent_if = &parent->parent_if;

	/* ① 레지스터 복사.
	 *    rip(다음 실행할 위치)가 부모의 fork() 호출 다음이라서
	 *    자식도 fork() 다음 줄부터 실행하게 된다. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* ★ rax 는 반환값 레지스터. 0 으로 바꿔서 "자식 쪽 fork() 는 0 을 반환"하게 한다.
	 *   (부모 쪽 fork() 는 syscall_handler 에서 자식 pid 를 rax 에 넣는다) */
	if_.R.rax = 0;

	/* ② 메모리 복사.
	 *    자식용 빈 페이지 테이블을 만들고, CPU 가 그걸 쓰도록 바꾼 뒤(process_activate),
	 *    부모의 사용자 페이지를 전부 duplicate_pte 로 복사한다. */
	current->pml4 = pml4_create ();
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

	/* ③ 열린 파일 복사.
	 *    file_duplicate 는 같은 파일을 가리키고 "읽기 위치까지 같은" 새 struct file 을 만든다.
	 *      - fork-read : 부모가 읽던 위치부터 자식이 이어서 읽는다.
	 *      - fork-close: 자식이 close 해도 자식 복사본만 닫혀서 부모는 계속 읽을 수 있다.
	 *    포인터만 복사하면(current->fdt[fd] = parent->fdt[fd]) 자식이 닫을 때
	 *    부모 파일까지 해제돼서 fork-close 가 실패한다. */
	lock_acquire (&filesys_lock);
	for (int fd = 2; fd < FDT_MAX; fd++) {
		if (parent->fdt[fd] == NULL)            /* 빈 칸은 건너뜀 */
			continue;
		current->fdt[fd] = file_duplicate (parent->fdt[fd]);   /* 같은 fd 번호에 복사 */
		if (current->fdt[fd] == NULL)           /* 메모리 부족 등으로 실패 */
			goto error;                         /* 락은 error 의 sys_exit 가 풀어 준다 */
	}
	/* ③-2 실행 파일도 복사한다 (rox-child, rox-multichild).
	 *      file_duplicate 는 쓰기 금지 상태까지 그대로 복사한다.
	 *      그래서 부모가 먼저 끝나 자기 것을 닫아도, 자식이 실행 중인 동안은
	 *      그 실행 파일에 쓰기가 계속 막혀 있다. */
	if (parent->running_file != NULL) {
		current->running_file = file_duplicate (parent->running_file);
		if (current->running_file == NULL)
			goto error;
	}
	lock_release (&filesys_lock);

	process_init ();

	/* ④ 복제 성공을 적고, process_fork 에서 잠든 부모를 깨운다. */
	current->fork_success = true;
	sema_up (&current->fork_sema);

	/* ⑤ if_ 의 레지스터를 CPU 에 올리고 사용자 모드로 점프한다. 돌아오지 않는다. */
	do_iret (&if_);

error:
	/* 실패해도 반드시 부모를 깨워야 한다. 안 그러면 부모가 sema_down 에서 영원히 잠든다.
	 * fork_success 는 false 그대로이므로 부모는 실패를 알게 된다. */
	sema_up (&current->fork_sema);
	sys_exit (-1);
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

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
/* ===================================================================
 * process_wait: 자식 CHILD_TID 가 끝날 때까지 기다렸다가 종료 코드를 돌려준다.
 *   - 내 자식이 아니거나 이미 wait 한 자식이면 바로 -1.
 *   - 자식과의 약속(세마포어 두 개):
 *       wait_sema : 자식이 끝나면 up  → 부모가 깨어남
 *       exit_sema : 부모가 종료 코드를 읽으면 up → 자식이 완전히 사라짐
 * =================================================================== */
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

/* ===================================================================
 * process_exit: 프로세스가 끝날 때 thread_exit() 안에서 불린다.
 *   하는 일: ① 종료 메시지 출력 ② 열린 파일·fd 표 정리 ③ 메모리 정리
 *           ④ 부모와의 wait 약속 지키기
 * =================================================================== */
void
process_exit (void) {
	struct thread *curr = thread_current ();

	/* ① 사용자 프로세스(페이지 테이블이 있는 스레드)일 때만 종료 메시지를 찍는다.
	 *    커널 스레드는 찍으면 안 된다. 테스트는 정확히 "이름: exit(코드)" 한 줄을 기대한다. */
	if (curr->pml4 != NULL)
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

	/* ② 열린 파일을 모두 닫고 fd 표 페이지를 돌려준다.
	 *    안 하면 프로세스가 끝날 때마다 메모리가 새서 multi-oom 이 실패한다. */
	if (curr->fdt != NULL) {
		lock_acquire (&filesys_lock);
		for (int fd = 2; fd < FDT_MAX; fd++)
			if (curr->fdt[fd] != NULL)
				file_close (curr->fdt[fd]);
		lock_release (&filesys_lock);
		palloc_free_page (curr->fdt);           /* thread_create 에서 받은 페이지 반납 */
		curr->fdt = NULL;
	}

	/* ②-2 실행 파일을 닫는다 (rox).
	 *      file_close 안에서 file_allow_write 가 불려 쓰기 금지가 풀린다.
	 *      반드시 아래 sema_up (wait_sema) 보다 먼저 해야 한다:
	 *      rox-child 는 부모가 wait 에서 깨어나자마자 그 파일에 쓰기를 시도한다. */
	if (curr->running_file != NULL) {
		lock_acquire (&filesys_lock);
		file_close (curr->running_file);
		lock_release (&filesys_lock);
		curr->running_file = NULL;
	}

	/* ③ 사용자 메모리(페이지 테이블과 페이지들)를 해제한다. */
	process_cleanup ();

	/* ④-1 내가 먼저 죽으면 내 자식들을 wait 해 줄 부모가 없다.
	 *      자식들이 process_exit 끝에서 exit_sema 를 기다리며 영원히 멈추지 않도록 미리 풀어 준다. */
	struct list_elem *e;
	for (e = list_begin (&curr->children); e != list_end (&curr->children);
			e = list_next (e))
		sema_up (&list_entry (e, struct thread, child_elem)->exit_sema);

	/* ④-2 wait 중인 부모를 깨우고(wait_sema), 부모가 exit_status 를 다 읽을 때까지 기다린다(exit_sema).
	 *      이 기다림이 없으면 부모가 읽기 전에 내 struct thread 가 해제될 수 있다. */
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
	/* 실행 파일을 열고 읽는 동안 다른 프로세스가 파일 시스템을 못 건드리게 락을 잡는다.
	 * 아래 어디서 goto done 이 일어나도 done 에서 풀리도록 맨 처음에 잡는다. */
	lock_acquire (&filesys_lock);

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

	/* rox: 실행 중인 프로그램 파일은 아무도 고치지 못하게 막는다.
	 *   1) exec 로 프로그램이 바뀌는 경우, 이전에 실행하던 파일은 닫는다(쓰기 금지 해제).
	 *   2) 새 실행 파일에 쓰기 금지를 건다. 이제 누가 write 해도 0 바이트만 쓰인다.
	 *   3) 닫지 않고 running_file 에 보관한다. 닫으면 쓰기 금지가 풀리기 때문이다.
	 *      process_exit 에서 닫는다. */
	if (t->running_file != NULL)
		file_close (t->running_file);
	file_deny_write (file);
	t->running_file = file;

done:
	/* 실패했을 때만 여기서 닫는다. 성공했으면 위에서 running_file 로 넘겼다. */
	if (!success)
		file_close (file);
	lock_release (&filesys_lock);
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
