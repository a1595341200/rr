#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/fcntl.h>

#include "read_trace.h"
#include "replayer.h"

#include "../share/sys.h"
#include "../share/trace.h"
#include "../share/util.h"
#include "../share/ipc.h"
#include "../share/hpc.h"

#define SKID_SIZE 			50

/**
 * function goes to the n-th conditional branch
 */
static void compensate_branch_count(struct context *ctx, int sig)
{
	uint64_t rbc_now, rbc_rec;

	rbc_now = read_rbc_up(ctx->hpc);
	rbc_rec = ctx->trace.rbc_up;

	printf("rbc_now: %llu   rbc_rec: %llu\n", rbc_now, rbc_rec);

	/* if the skid size was too small, go back to the last checkpoint and
	 * re-execute the program.
	 */
	if (rbc_now > rbc_rec) {
		/* checkpointing is not implemented yet - so we fail */
		fprintf(stderr, "hpc overcounted in asynchronous event, recorded: %llu  now: %llu\n", rbc_rec, rbc_now);
		assert(rbc_now < rbc_rec);
	}

	while (1) {
		struct user_regs_struct regs;
		read_child_registers(ctx->child_tid, &regs);
		rbc_now = read_rbc_up(ctx->hpc);
		assert(signal_pending(ctx->status) == 0);

		if (rbc_now < rbc_rec) {
			singlestep(ctx, 0);
		} else if (rbc_now == rbc_rec) {
			/* the eflags register has two bits that are set when an interrupt is pending:
			 * bit 8:  TF (trap flag)
			 * bit 17: VM (virtual 8086 mode)
			 *
			 * we enable these two bits in the eflags register to make sure that the register
			 * files match
			 *
			 */
			regs.eflags |= (1 << 7);
			regs.eflags |= (1 << 16);
			if (!compare_register_files("now", &regs, "rec", &ctx->trace.recorded_regs, 0, 0)) {
				/* A SIGSEGV can be triggered by a regular instruction; it is not necessarily sent by
				 * another process. We check this condition here.
				 */
				printf("we found the crappy spot\n");
				if (sig == SIGSEGV) {
					printf("pending 1: %d\n", ctx->pending_sig);
					print_inst(ctx->child_tid);
					singlestep(ctx, 0);
					printf("pending 2: %d\n", ctx->pending_sig);
					print_inst(ctx->child_tid);
					if (ctx->pending_sig == SIGSEGV) {
						/* deliver the signal */
						singlestep(ctx, SIGSEGV);
						printf("awsome!!\n");
						printf("pending 3: %d\n", ctx->pending_sig);
						assert(ctx->pending_sig == 0);
						break;
					}

					printf("pending 4: %d\n", ctx->pending_sig);
				}
				/* set the signal such that it is delivered when the process continues */
				ctx->pending_sig = sig;
			}
			/* check that we do not get unexpected signal in the single-stepping process */
			singlestep(ctx, 0);
		} else {
			fprintf(stderr, "internal error: cannot find correct spot for signal(%d) delivery -- bailing out\n", sig);
			sys_exit();
		}
	}
}

void rep_process_signal(struct context *ctx)
{
	struct trace* trace = &(ctx->trace);
	int tid = ctx->child_tid;
	int sig = -trace->stop_reason;

	/* if the there is still a signal pending here, two signals in a row must be delivered?\n */
	assert(ctx->pending_sig == 0);

	switch (sig) {

	/* set the eax and edx register to the recorded values */
	case -SIG_SEGV_RDTSC:
	{
		struct user_regs_struct regs;
		int size;

		/* goto the event */
		goto_next_event(ctx);

		/* make sure we are there */
		assert(WSTOPSIG(ctx->status) == SIGSEGV);

		char* inst = get_inst(tid, 0, &size);
		assert(strncmp(inst,"rdtsc",5) == 0);
		read_child_registers(tid, &regs);
		regs.eax = trace->recorded_regs.eax;
		regs.edx = trace->recorded_regs.edx;
		regs.eip += size;
		write_child_registers(tid, &regs);
		sys_free((void**) &inst);

		compare_register_files("rdtsv_now", &regs, "rdsc_rec", &ctx->trace.recorded_regs, 1, 1);

		/* this signal should not be recognized by the application */
		ctx->pending_sig = 0;
		break;
	}

	case -USR_SCHED:
	{
		assert(trace->rbc_up > 0);

		/* if the current architecture over-counts the event in question,
		 * substract the overcount here */
		reset_hpc(ctx, trace->rbc_up - SKID_SIZE);
		goto_next_event(ctx);
		/* make sure that the signal came from hpc */
		if (fcntl(ctx->hpc->rbc_down.fd, F_GETOWN) == ctx->child_tid) {
			/* this signal should not be recognized by the application */
			ctx->pending_sig = 0;
			stop_hpc_down(ctx);
			compensate_branch_count(ctx, sig);
			stop_hpc(ctx);
		} else {
			fprintf(stderr, "internal error: next event should be: %d but it is: %d -- bailing out\n", -USR_SCHED, ctx->event);
			sys_exit();
		}

		break;
	}

	case SIGIO:
	case SIGCHLD:
	case SIGSEGV:
	{
		/* synchronous signal (signal received in a system call) */
		if (trace->rbc_up == 0) {
			ctx->pending_sig = sig;
			return;
		}

		printf("rbc: %llu  we will deliver signal: %d\n", trace->rbc_up,sig);
		// setup and start replay counters
		reset_hpc(ctx, trace->rbc_up - SKID_SIZE);
		printf("setting replay counters: retired branch count = %llu\n", trace->rbc_up);

		// single-step if the number of instructions to the next event is "small"
		if (trace->rbc_up <= 1000) {

			compensate_branch_count(ctx, sig);
			stop_hpc_down(ctx);
			stop_hpc(ctx);
		} else {
			printf("large count\n");
			assert(1==0);
			sys_ptrace_cont(tid);
			sys_waitpid(tid, &ctx->status);
			// make sure we ere interrupted by ptrace
			assert(WSTOPSIG(ctx->status) == SIGIO);

			//DO NOT FORGET TO STOP HPC!!!
			compensate_branch_count(ctx, sig);
		}

		break;
	}

	default:
	printf("unknown signal %d -- bailing out\n", sig);
	sys_exit();
		break;
	}
}
