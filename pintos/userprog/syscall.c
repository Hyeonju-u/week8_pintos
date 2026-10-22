#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/init.h"          /* ← 추가: power_off() */
#include "lib/kernel/stdio.h"      /* ← 추가: putbuf() */
#include "userprog/process.h"
#include "threads/synch.h"
#include "threads/palloc.h"      /* palloc_get_page: exec 명령줄 복사용 */
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

/* 파일 시스템 전역 락.
 * Pintos 파일 시스템은 여러 스레드가 동시에 쓰면 안전하지 않아서,
 * 파일을 건드리는 코드는 모두 이 락을 잡고 들어간다. (process.c 에서도 사용) */
struct lock filesys_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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

	lock_init (&filesys_lock);   /* 파일 시스템 락 초기화 (부팅 때 한 번) */
}

/* ===================================================================
 * sys_exit: 모든 "프로세스 끝내기"가 지나가는 하나의 출구
 *   - exit() 시스템 콜, 잘못된 주소 접근(page fault), fork 실패 등이
 *     전부 이 함수로 끝난다.
 *   - 종료 메시지("이름: exit(코드)")는 여기서 찍지 않는다.
 *     thread_exit → process_exit 에서 한 번만 찍는다. (두 번 찍히면 테스트 실패)
 * =================================================================== */
void
sys_exit (int status) {
	/* 파일 시스템 처리 도중(락을 잡은 채) 죽는 경우가 있다.
	 * process_exit 가 파일을 닫으려고 같은 락을 다시 잡는데,
	 * 이미 내가 잡고 있으면 lock_acquire 의 ASSERT 에 걸려 커널이 멈춘다.
	 * 그래서 내가 잡고 있으면 먼저 놓는다. */
	if (lock_held_by_current_thread (&filesys_lock))
		lock_release (&filesys_lock);

	/* 부모가 wait() 로 받아 갈 종료 코드를 내 struct thread 에 적어 둔다. */
	thread_current ()->exit_status = status;

	/* 스레드 종료. 안에서 process_exit() 가 불려 메시지 출력과 자원 정리를 한다.
	 * 이 함수는 돌아오지 않는다. */
	thread_exit ();
}

/* ===================================================================
 * 사용자 포인터 검사 함수들
 *   사용자 프로그램이 시스템 콜에 넘긴 주소는 믿을 수 없다.
 *   (NULL, 커널 주소, 매핑 안 된 주소일 수 있음)
 *   커널이 그런 주소를 그대로 읽으면 커널 page fault 로 OS 전체가 멈추므로,
 *   쓰기 전에 검사해서 잘못됐으면 그 프로세스만 exit(-1) 시킨다.
 * =================================================================== */

/* 주소 하나를 검사한다. */
static void
check_address (const void *a) {
	if (a == NULL                                   /* 1) NULL 포인터 (create-null, open-null) */
			|| !is_user_vaddr (a)                   /* 2) 커널 영역 주소 (KERN_BASE 이상) */
			|| !pml4_get_page (thread_current ()->pml4, a))   /* 3) 사용자 영역이지만 실제 페이지가 매핑 안 됨 */
		sys_exit (-1);
}

/* 버퍼 [b, b+n) 를 검사한다.
 * 처음 바이트와 마지막 바이트를 둘 다 보면, 버퍼가 두 페이지에 걸쳐 있어도
 * (read-boundary, write-boundary 테스트) 두 페이지가 모두 검사된다. */
static void
check_buffer (const void *b, unsigned n) {
	if (n > 0) {                                    /* 크기 0이면 메모리를 안 건드리므로 통과 */
		check_address (b);                          /* 첫 바이트 */
		check_address ((const char *) b + n - 1);   /* 마지막 바이트 */
	}
}

/* 문자열을 '\0' 이 나올 때까지 한 글자씩 검사한다.
 * 길이를 미리 모르므로, 검사 → 읽기 순서로 한 칸씩 전진한다.
 * 문자열이 페이지 경계를 넘어가도(create-bound, open-boundary) 안전하다. */
static void
check_string (const char *s) {
	for (;; s++) {
		check_address (s);                          /* 읽기 전에 먼저 검사 */
		if (*s == '\0')                             /* 문자열 끝이면 통과 */
			return;
	}
}

/* ===================================================================
 * fd(파일 디스크립터) 표 도우미
 *   - fd 표는 프로세스마다 가진 배열: fdt[fd] = 열린 파일(struct file *)
 *   - 0번 = 키보드(stdin), 1번 = 화면(stdout) 이라 표에 넣지 않고 2번부터 쓴다.
 *   - 표 자체는 thread_create() 에서 한 페이지(포인터 512칸)를 받아 둔다.
 * =================================================================== */

/* fd 번호로 열린 파일을 찾는다.
 * 범위 밖 번호(음수, 0, 1, 너무 큰 수)나 빈 칸이면 NULL. */
static struct file *
fd_get (int fd) {
	if (fd < 2 || fd >= FDT_MAX)
		return NULL;
	return thread_current ()->fdt[fd];
}

/* 비어 있는 가장 작은 번호에 FILE 을 넣고, 그 번호를 돌려준다.
 * 닫힌 칸을 다시 쓰므로 fd 번호가 끝없이 커지지 않는다.
 * 표가 꽉 찼으면 -1. */
static int
fd_add (struct file *file) {
	struct file **fdt = thread_current ()->fdt;
	for (int fd = 2; fd < FDT_MAX; fd++) {
		if (fdt[fd] == NULL) {
			fdt[fd] = file;
			return fd;
		}
	}
	return -1;
}

/* ===================================================================
 * 파일 시스템 콜
 *   모두 같은 모양이다:
 *     ① 사용자 포인터 검사  ② fd 로 파일 찾기
 *     ③ filesys_lock 잡고 파일 시스템 함수 호출  ④ 결과 반환
 *   Pintos 파일 시스템은 동시에 여러 스레드가 들어오면 안전하지 않아서
 *   파일을 건드리는 동안에는 항상 filesys_lock 을 잡는다.
 *   포인터 검사는 락을 잡기 "전에" 한다. 검사에 걸려 죽을 때 락을 쥐고 있지 않게.
 * =================================================================== */

/* create(name, size): 크기가 SIZE 인 새 파일을 만든다. 성공하면 true. */
static bool
sys_create (const char *name, unsigned size) {
	check_string (name);
	lock_acquire (&filesys_lock);
	bool ok = filesys_create (name, size);    /* 이미 있거나 이름이 너무 길면 false */
	lock_release (&filesys_lock);
	return ok;
}

/* remove(name): 파일을 지운다. 성공하면 true. */
static bool
sys_remove (const char *name) {
	check_string (name);
	lock_acquire (&filesys_lock);
	bool ok = filesys_remove (name);
	lock_release (&filesys_lock);
	return ok;
}

/* open(name): 파일을 열고 fd 번호를 돌려준다. 실패하면 -1. */
static int
sys_open (const char *name) {
	check_string (name);
	lock_acquire (&filesys_lock);
	struct file *file = filesys_open (name);  /* 없는 파일이면 NULL (open-missing) */
	int fd = -1;
	if (file != NULL) {
		fd = fd_add (file);                   /* fd 표 빈 칸에 등록 */
		if (fd == -1)                         /* 표가 꽉 찼으면 방금 연 파일을 다시 닫는다 */
			file_close (file);
	}
	lock_release (&filesys_lock);
	return fd;                                /* 같은 파일을 두 번 열면 서로 다른 fd (open-twice) */
}

/* filesize(fd): 파일 크기(바이트). 잘못된 fd 면 -1. */
static int
sys_filesize (int fd) {
	struct file *file = fd_get (fd);
	if (file == NULL)
		return -1;
	lock_acquire (&filesys_lock);
	int n = file_length (file);
	lock_release (&filesys_lock);
	return n;
}

/* read(fd, buffer, size): 최대 SIZE 바이트를 BUFFER 로 읽고, 실제로 읽은 바이트 수를 돌려준다. */
static int
sys_read (int fd, void *buffer, unsigned size) {
	check_buffer (buffer, size);              /* 커널이 이 버퍼에 쓸 것이므로 먼저 검사 (read-bad-ptr) */

	if (fd == 0) {                            /* fd 0 = 키보드: 한 글자씩 받아 채운다 */
		for (unsigned i = 0; i < size; i++)
			((uint8_t *) buffer)[i] = input_getc ();
		return size;
	}

	struct file *file = fd_get (fd);
	if (file == NULL)                         /* fd 1(stdout)이나 열리지 않은 fd (read-stdout, read-bad-fd) */
		return -1;
	lock_acquire (&filesys_lock);
	int n = file_read (file, buffer, size);   /* 파일 끝이면 size 보다 적게 읽힌다 */
	lock_release (&filesys_lock);
	return n;
}

/* write(fd, buffer, size): BUFFER 의 SIZE 바이트를 쓰고, 실제로 쓴 바이트 수를 돌려준다. */
static int
sys_write (int fd, const void *buffer, unsigned size) {
	check_buffer (buffer, size);              /* 커널이 이 버퍼를 읽을 것이므로 먼저 검사 (write-bad-ptr) */

	if (fd == 1) {                            /* fd 1 = 화면: putbuf 로 한 번에 출력 */
		putbuf (buffer, size);                /* (한 번에 찍어야 다른 출력과 섞이지 않는다) */
		return size;
	}

	struct file *file = fd_get (fd);
	if (file == NULL)                         /* fd 0(stdin)이나 열리지 않은 fd (write-stdin, write-bad-fd) */
		return -1;
	lock_acquire (&filesys_lock);
	int n = file_write (file, buffer, size);  /* 파일 크기를 넘어서는 못 쓴다 (Project 2 파일 시스템) */
	lock_release (&filesys_lock);
	return n;
}

/* seek(fd, position): 다음에 읽고 쓸 위치를 POSITION 으로 옮긴다. */
static void
sys_seek (int fd, unsigned pos) {
	struct file *file = fd_get (fd);
	if (file == NULL)
		return;
	lock_acquire (&filesys_lock);
	file_seek (file, pos);
	lock_release (&filesys_lock);
}

/* tell(fd): 다음에 읽고 쓸 위치를 돌려준다. */
static unsigned
sys_tell (int fd) {
	struct file *file = fd_get (fd);
	if (file == NULL)
		return 0;
	lock_acquire (&filesys_lock);
	unsigned pos = file_tell (file);
	lock_release (&filesys_lock);
	return pos;
}

/* close(fd): 파일을 닫고 fd 칸을 비운다. 잘못된 fd 면 아무것도 안 한다. */
static void
sys_close (int fd) {
	struct file *file = fd_get (fd);
	if (file == NULL)                         /* 없는 fd, 이미 닫은 fd (close-bad-fd, close-twice) */
		return;
	thread_current ()->fdt[fd] = NULL;        /* 칸을 먼저 비워서 같은 fd 로 다시 닫지 못하게 한다 */
	lock_acquire (&filesys_lock);
	file_close (file);
	lock_release (&filesys_lock);
}

/* ===================================================================
 * exec(cmd_line): 지금 프로세스를 CMD_LINE 프로그램으로 바꾼다.
 *   - 새 프로세스를 만들지 않는다. pid, 열린 파일은 그대로이고 메모리만 새 프로그램으로 바뀐다.
 *   - 성공하면 돌아오지 않는다(새 프로그램의 main 으로 점프).
 *   - 실패하면(없는 파일 등) 이 프로세스는 exit(-1) 로 끝난다. (exec-missing)
 *   흐름: 문자열 검사 → 커널 페이지에 복사 → process_exec → (실패 시) exit(-1)
 * =================================================================== */
static int
sys_exec (const char *cmd_line) {
	/* 1. 사용자 문자열 검사 (exec-bad-ptr, exec-boundary) */
	check_string (cmd_line);

	/* 2. 커널 페이지에 복사한다.
	 *    process_exec 는 지금 주소 공간(사용자 메모리)을 지우고 새 프로그램을 올린다.
	 *    cmd_line 은 그 지워질 사용자 메모리 안에 있으므로, 지우기 전에 옮겨 둬야 한다.
	 *    이 페이지는 process_exec 가 다 쓰고 해제한다. */
	char *copy = palloc_get_page (0);
	if (copy == NULL)
		sys_exit (-1);
	strlcpy (copy, cmd_line, PGSIZE);

	/* 3. 프로그램 교체. 성공하면 do_iret 으로 새 프로그램이 시작되어 여기로 돌아오지 않는다. */
	process_exec (copy);

	/* 4. 여기까지 왔으면 실패. 원래 메모리는 이미 지워졌으므로 돌아갈 곳이 없다 → -1 로 종료 */
	sys_exit (-1);
}

/* ===================================================================
 * syscall_handler: 시스템 콜이 들어오는 입구
 *   사용자 프로그램이 syscall 명령을 실행하면 syscall-entry.S 가
 *   사용자 레지스터를 struct intr_frame(f) 모양으로 저장하고 이 함수를 부른다.
 *   - 시스템 콜 번호: f->R.rax
 *   - 인자: 1번째 rdi, 2번째 rsi, 3번째 rdx (x86-64 호출 규약)
 *   - 반환값: f->R.rax 에 넣으면 사용자 쪽 함수의 반환값이 된다.
 * =================================================================== */
void
syscall_handler (struct intr_frame *f) {
	uint64_t a1 = f->R.rdi, a2 = f->R.rsi, a3 = f->R.rdx;

	switch (f->R.rax) {
		case SYS_HALT:                        /* halt(): Pintos 전원 끄기 */
			power_off ();
			break;

		case SYS_EXIT:                        /* exit(status) */
			sys_exit ((int) a1);
			break;

		case SYS_FORK:                        /* fork(name) */
			check_string ((const char *) a1); /* 자식 이름도 사용자 문자열이라 검사 */
			/* f 를 통째로 넘기는 게 핵심: f 가 "fork 를 부른 순간의 사용자 레지스터"다.
			 * 자식은 이걸 복사해서 fork() 다음 줄부터 실행한다.
			 * (thread->tf 는 커널 레지스터라 쓰면 안 된다) */
			f->R.rax = process_fork ((const char *) a1, f);   /* 부모의 반환값 = 자식 pid */
			break;

		case SYS_EXEC:                        /* exec(cmd_line): 성공하면 돌아오지 않는다 */
			f->R.rax = sys_exec ((const char *) a1);
			break;

		case SYS_WAIT:                        /* wait(pid): 자식이 끝날 때까지 기다리고 종료 코드를 받는다 */
			f->R.rax = process_wait ((tid_t) a1);
			break;

		case SYS_CREATE:   f->R.rax = sys_create ((const char *) a1, (unsigned) a2); break;
		case SYS_REMOVE:   f->R.rax = sys_remove ((const char *) a1); break;
		case SYS_OPEN:     f->R.rax = sys_open ((const char *) a1); break;
		case SYS_FILESIZE: f->R.rax = sys_filesize ((int) a1); break;
		case SYS_READ:     f->R.rax = sys_read ((int) a1, (void *) a2, (unsigned) a3); break;
		case SYS_WRITE:    f->R.rax = sys_write ((int) a1, (const void *) a2, (unsigned) a3); break;
		case SYS_SEEK:     sys_seek ((int) a1, (unsigned) a2); break;             /* 반환값 없음 */
		case SYS_TELL:     f->R.rax = sys_tell ((int) a1); break;
		case SYS_CLOSE:    sys_close ((int) a1); break;                           /* 반환값 없음 */

		default:                              /* 아직 만들지 않은 시스템 콜(exec 등)은 -1 로 종료 */
			sys_exit (-1);
	}
}
