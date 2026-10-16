#ifndef THREADS_INTERRUPT_H
#define THREADS_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>

/* Interrupts on or off? */
enum intr_level
{
	INTR_OFF, /* Interrupts disabled. */
	INTR_ON	  /* Interrupts enabled. */
};

enum intr_level intr_get_level(void);
enum intr_level intr_set_level(enum intr_level);
enum intr_level intr_enable(void);
enum intr_level intr_disable(void);

/* Interrupt stack frame. */
struct gp_registers
{
	uint64_t r15;
	uint64_t r14;
	uint64_t r13;
	uint64_t r12;
	uint64_t r11;
	uint64_t r10;
	uint64_t r9;
	uint64_t r8;  // 추가된 범용 레지스터 15번까지
	uint64_t rsi; // 문자열/메모리 복사 때 원본 주소
	uint64_t rdi; // Destination Index. 복사 때 목적지 주소 1번째 인자 → 인자 전달할 때 argc
	uint64_t rbp; // Base Pointer. 현재 스택 프레임의 바닥 스택 프레임 추적 (callee-saved)
	uint64_t rdx; // Data. 곱셈/나눗셈 보조, I/O 3번째인자
	uint64_t rcx; // 카운터,반복 4번째 인자
	uint64_t rbx; // 값 보존용
	uint64_t rax; // 함수 리턴값,시스템콜 번호 전달,시스템 콜 리턴값
} __attribute__((packed));

struct intr_frame
{
	/* Pushed by intr_entry in intr-stubs.S.
	   These are the interrupted task's saved registers. */
	struct gp_registers R;
	uint16_t es;
	uint16_t __pad1;
	uint32_t __pad2;
	uint16_t ds;
	uint16_t __pad3;
	uint32_t __pad4;
	/* Pushed by intrNN_stub in intr-stubs.S. */
	uint64_t vec_no; /* Interrupt vector number. */
			 /* Sometimes pushed by the CPU,
			    otherwise for consistency pushed as 0 by intrNN_stub.
			    The CPU puts it just under `eip', but we move it here. */
	uint64_t error_code;
	/* Pushed by the CPU.
	   These are the interrupted task's saved registers. */
	uintptr_t rip;
	uint16_t cs;
	uint16_t __pad5;
	uint32_t __pad6;
	uint64_t eflags;
	uintptr_t rsp;
	uint16_t ss;
	uint16_t __pad7;
	uint32_t __pad8;
} __attribute__((packed));

typedef void intr_handler_func(struct intr_frame *);

void intr_init(void);
void intr_register_ext(uint8_t vec, intr_handler_func *, const char *name);
void intr_register_int(uint8_t vec, int dpl, enum intr_level,
		       intr_handler_func *, const char *name);
bool intr_context(void);
void intr_yield_on_return(void);

void intr_dump_frame(const struct intr_frame *);
const char *intr_name(uint8_t vec);

#endif /* threads/interrupt.h */
