#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/*-------------------------- project.1-Alarm_Clock -----------------------------*/
// global_tick 위치가 헷갈린다. 여기가 맞나?
// sleep_list에 있는 local tick(알람시간) 중 최솟값. = 가장 이른 알람시간
int64_t global_tick = INT64_MAX;
/*-------------------------- project.1-Alarm_Clock -----------------------------*/

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* 초당 PIT_FREQ 횟수를 인터럽트하도록 
8254 PIT(프로그래밍 가능 간격 타이머)를 설정하고 
해당 인터럽트를 등록합니다.
Sets up the 8254 Programmable Interval Timer (PIT) to
interrupt PIT_FREQ times per second, and registers the
corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* 짧은 지연(brief delays)을 구현하는 데 사용되는 loops_per_tick을 보정합니다.
Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* OS가 부팅된 이후, timer의 tick 수 반환. 
Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* 인자 THEN 이후 경과된 타이머 틱 수를 반환한다.
timer_ticks()에서 한 번 반환된 값이어야 합니다.
Returns the number of timer ticks elapsed since THEN, which
should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* ticks 시간동안 스레드 실행을 일시 중단
예시 : timer_sleep(8시간) -> 8시간 후에 깨워주세요.(8시에 깨워주세요 아님)
Suspends execution for approximately TICKS timer ticks.
*/
void
timer_sleep (int64_t ticks) {
	// timer_ticks : 현재 tick 값(현재시간)을 반환. 함수 호출시점의 ticks
	// 예시 : 지금시간 1시
	int64_t start = timer_ticks ();

	ASSERT (intr_get_level () == INTR_ON);
	/*	기존코드 이해목적 주석
		timer_elapsed : 
			인수 start 이후 경과된 tick 수 반환
			-> 인자 tick 값과 start이후 경과된 tick 비교, 
			ticks(sleep 요청시간)가 더 커서 sleep 상태 유지해야하면 yield

		thread_yield : 
			스레드 제어권을 cpu에게 양보하고(cpu는 제어권 받으면 스케줄링 시작함)
			현재 스레드는 ready_list에 추가한다(큐-> 제일 뒤에 추가됨)

		while 요건 : 함수호출기준 경과시간<요청시간 (== 실질적으로 sleep 필요)
		
	*/

	// 기존 코드 : busy_waiting 방식
	// while (timer_elapsed (start) < ticks)
	// 	thread_yield ();

	/* DONE : 스레드가 자기 자신을 sleep queue(sleep 대기열)로 이동시킨다.
		timer_sleep()이 호출될 때, 틱을 체크
		웨이크업까지 시간이 남아있다면
			-> ready_list에서 caller thread(=timer_sleep를 호출한 스레드)를 제거하고 삽입한다.

		Sample inplementation :
		if(timer_elapsed(start) < ticks)
			thread_sleep(적절한 값. );	// 직접 구현하세여
	*/
	if (timer_elapsed(start)<ticks)
		/* 
			알람을 맞춘다 생각하자.
			현재시간 + 요청시간 - 호출시점기준 소요시간
			1시 10분 + 8시간 - 10분 -> 9시
		*/
		thread_sleep(timer_ticks() + ticks - timer_elapsed(start));
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* 타이머 통계를 인쇄.
Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* 타이머 인터럽트 핸들러
Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();	// update the cpu usage for running process

	/* code to add: ​
		check sleep list and the global tick.​
		find any threads to wake up,​
		move them to the ready list if necessary.​
		update the global tick.​

		추가할 코드 : 
		수면 목록과 글로벌 틱을 확인하십시오.
		깨울 스레드 찾기,
		필요한 경우 준비 목록으로 이동합니다.
		글로벌 틱을 업데이트합니다.
	*/
	if (global_tick <= ticks)
		thread_awake(ticks);
}

/* LOOPS 반복이 둘 이상의 타이머 틱을 기다리면 true를 반환하고, 
그렇지 않으면 false를 반환합니다.
Returns true if LOOPS iterations waits for more than one timer
tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* 짧은 지연(brief delays)을 구현하기 위해 인자 loop 시간만큼 루프를 반복합니다.

코드 정렬이 타이밍에 상당한 영향을 줄 수 있으므로 
이 함수가 다른 위치에서 다르게 인라인되면 결과를 예측하기 어렵기 때문에 
NO_INLINE으로 표시되었습니다.

Iterates through a simple loop LOOPS times, for implementing
brief delays.

Marked NO_INLINE because code alignment can significantly
affect timings, so that if this function was inlined
differently in different places the results would be difficult
to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* 약 NUM/DENOM초 동안 sleep합니다.
Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
