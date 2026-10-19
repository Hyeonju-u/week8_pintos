#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/init.h"     /* ← 추가: power_off() */
#include "lib/kernel/stdio.h" /* ← 추가: putbuf() */
#include "filesys/filesys.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081	    /* Segment selector msr */
#define MSR_LSTAR 0xc0000082	    /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
				((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
		  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 커널이 BUFFER의 SIZE바이트를 읽을 수 있는지 확인한다. */
static bool
readable_buffer(const void *buffer, size_t size)
{
	if (size == 0)
	{
		return true;
	}

	uintptr_t start = (uintptr_t)buffer;

	if (start == 0 || start >= KERN_BASE)
	{
		return false;
	}

	/* 주소 덧셈 오버플로와 커널 영역 침범을 함께 방지한다. */
	if (size - 1 > (KERN_BASE - 1) - start)
	{
		return false;
	}

	uintptr_t last = start + size - 1;
	uintptr_t page = (uintptr_t)pg_round_down((void *)start);

	/* 버퍼가 걸치는 모든 페이지를 확인한다. */
	for (; page <= last; page += PGSIZE)
	{
		if (pml4_get_page(thread_current()->pml4,
				  (void *)page) == NULL)
		{
			return false;
		}
	}

	return true;
}
static bool readlable_string(const char *str)
{ 

	for (;; str++)
	{
		if (!readable_buffer(str,1) )
		return false;
		if (*str =='\0')
		return true;

	}
}


void syscall_handler(struct intr_frame *f)
{
	/* 시스템 콜 번호는 rax, 인자는 rdi, rsi, rdx 순서로 들어온다. */
	switch (f->R.rax)
	{
	case SYS_HALT:
		power_off();
		break;

	case SYS_EXIT:
		/* exit(status): 종료 코드를 저장해 두고 스레드를 끝낸다.
		 * 메시지 출력은 process_exit에서 한다. */
		thread_current()->exit_status = (int)f->R.rdi;
		thread_exit();
		break;

	case SYS_FORK:
	{
	        f->R.rax = process_fork((const char *)f->R.rdi, f);
		break;
	}
        case SYS_EXEC:
	{      
		const char *file = (const char *)f->R.rdi;
		/*문자열 검사*/
		if (!readlable_string(file)){
		thread_current()->exit_status = -1;
		thread_exit();
		}
		/*process_exec가 유저 메모리 지우기전에 커널페이지로 복사*/
		char *fn_copy = palloc_get_page (0);
		if (fn_copy == NULL)
		{
		  thread_current()->exit_status = -1;
		  thread_exit();	
		}
		/*받는곳 fn_copy 보내는 곳 file */
	        strlcpy (fn_copy,file, PGSIZE);
		/* 지금 프로세스를 새 프로그램으로 바꾸기*/
		process_exec(fn_copy);
                /* 프로그램 종료*/
		thread_current()->exit_status = -1;
		thread_exit();
                break;
	}
	case SYS_WAIT:
	{
		f->R.rax = process_wait(f->R.rdi);
		break;
	}
        case SYS_CREATE:
	{
	    const char *file = (const char *)f->R.rdi;
	    unsigned initial_size = (unsigned)f->R.rsi;
	    if (!readlable_string(file))
	    {
		thread_current()->exit_status = -1;
		thread_exit();
	    }
             f->R.rax = filesys_create(file, initial_size);
	     break;
	}
	SYS_REMOVE:
	{
		const char *file = (const char *)f->R.rdi;
		if (!readlable_string(file))
	    {
		thread_current()->exit_status = -1;
		thread_exit();
	    }
		f->R.rax = filesys_remove(f->R.rdi);

		break;
	}
        case SYS_OPEN:
	{
               const char *file = (const char *)f->R.rdi;
	       if (!readlable_string(file))
	    {
		thread_current()->exit_status = -1;
		thread_exit();
	    }
	    f->R.rax = filesys_open(f->R.rdi);

               break;
	}
	case SYS_FILESIZE:
	{
                int fd = (int)f->R.rdi;
		break;

	}
        case SYS_READ:
	{       int fd = (int)f->R.rdi;
		const void *buffer = (const void *)f->R.rsi;
		unsigned size = (unsigned)f->R.rdx;
		break;
	}

        case SYS_WRITE:
	{
		/* write(fd, buffer, size): 지금은 fd 1(화면)만 지원한다. */
		int fd = (int)f->R.rdi;
		const void *buffer = (const void *)f->R.rsi;
		unsigned size = (unsigned)f->R.rdx;
		if (!readable_buffer(buffer, size)) // readable_buffer 읽을수 없으면
		{
			thread_current()->exit_status = -1;
			thread_exit();
		}

		if (fd == 1)
		{
			putbuf(buffer, size);
			f->R.rax = size; /* 반환값은 rax에 넣는다: 쓴 바이트 수 */
		}
		else
			f->R.rax = -1;
		break;
	}

	case SYS_TELL:
	{  
	   int fd = (int)f->R.rdi;
           break;
	}	


        case SYS_CLOSE:
	{
		int fd = (int)f->R.rdi;
		break;
	}


	default:
	
		/* 아직 만들지 않은 시스템 콜은 프로세스를 -1로 종료 */
		thread_current()->exit_status = -1;
		thread_exit();
	
	}
}