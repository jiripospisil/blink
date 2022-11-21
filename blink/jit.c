/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "blink/assert.h"
#include "blink/dll.h"
#include "blink/end.h"
#include "blink/endian.h"
#include "blink/jit.h"
#include "blink/lock.h"
#include "blink/log.h"
#include "blink/macros.h"

#if (defined(__x86_64__) || defined(__aarch64__)) && \
    !__has_feature(memory_sanitizer)

/**
 * @fileoverview Just-In-Time Function Threader
 *
 * This file implements an abstraction for assembling executable code at
 * runtime. This is intended to be used in cases where it's desirable to
 * have fast "threaded" pathways, between existing functions, which were
 * compiled statically. We need this because virtual machine dispatching
 * isn't very fast, when it's implemented by loops or indirect branches.
 * Modern CPUs go much faster if glue code without branches is outputted
 * to memory at runtime, i.e. a small function that calls the functions.
 */

#define kAmdXor      0x31
#define kAmdJmp      0xe9
#define kAmdCall     0xe8
#define kAmdJmpAx    "\377\340"
#define kAmdCallAx   "\377\320"
#define kAmdDispMin  INT32_MIN
#define kAmdDispMax  INT32_MAX
#define kAmdDispMask 0xffffffffu
#define kAmdRex      0x40  // turns ah/ch/dh/bh into spl/bpl/sil/dil
#define kAmdRexb     0x41  // turns 0007 (r/m) of modrm into r8..r15
#define kAmdRexr     0x44  // turns 0070 (reg) of modrm into r8..r15
#define kAmdRexw     0x48  // makes instruction 64-bit
#define kAmdMovImm   0xb8
#define kAmdAx       0  // first function result
#define kAmdCx       1  // third function parameter
#define kAmdDx       2  // fourth function parameter, second function result
#define kAmdBx       3  // generic saved register
#define kAmdSp       4  // stack pointer
#define kAmdBp       5  // backtrace pointer
#define kAmdSi       6  // second function parameter
#define kAmdDi       7  // first function parameter

#define kArmJmp      0x14000000u  // B
#define kArmCall     0x94000000u  // BL
#define kArmMovNex   0xf2800000u  // sets sub-word of register to immediate
#define kArmMovZex   0xd2800000u  // load immediate into reg w/ zero-extend
#define kArmMovSex   0x92800000u  // load 1's complement imm w/ sign-extend
#define kArmDispMin  -33554432    // can jump -2**25 ints backward
#define kArmDispMax  +33554431    // can jump +2**25-1 ints forward
#define kArmDispMask 0x03ffffff   // mask of branch displacement
#define kArmRegOff   0            // bit offset of destination register
#define kArmRegMask  0x0000001fu  // mask of destination register
#define kArmImmOff   5            // bit offset of mov immediate value
#define kArmImmMask  0x001fffe0u  // bit offset of mov immediate value
#define kArmImmMax   0xffffu      // maximum immediate value per instruction
#define kArmIdxOff   21           // bit offset of u16[4] sub-word index
#define kArmIdxMask  0x00600000u  // mask of u16[4] sub-word index

#ifdef MAP_FIXED_NOREPLACE
// The mmap() address parameter without MAP_FIXED is documented by
// Linux as a hint for locality. However our testing indicates the
// kernel is still likely to assign addresses that're outrageously
// far away from what was requested. So we're just going to choose
// something that's past the program break, and hope for the best.
#define MAP_DEMAND MAP_FIXED_NOREPLACE
#else
#define MAP_DEMAND 0
#endif

#define JITSTAGE_CONTAINER(e) DLL_CONTAINER(struct JitStage, list, e)

struct JitStage {
  int start;
  int index;
  hook_t *hook;
  dll_element list;
};

#if defined(__x86_64__)
static const u8 kPrologue[] = {
    0x55,              // push %rbp
    0x48, 0x89, 0xe5,  // mov  %rsp,%rbp
    0x53,              // push %rbx
    0x53,              // push %rbx
    0x48, 0x89, 0xfb,  // mov  %rdi,%rbx
};
static const u8 kEpilogue[] = {
    0x48, 0x8b, 0x5d, 0xf8,  // mov -0x8(%rbp),%rbx
    0xc9,                    // leave
    0xc3,                    // ret
};
#elif defined(__aarch64__)
static const u32 kPrologue[] = {
    0xa9be7bfd,  // stp x29, x30, [sp, #-32]!
    0x910003fd,  // mov x29, sp
    0xf9000bf3,  // str x19, [sp, #16]
    0xaa0003f3,  // mov x19, x0
};
static const u32 kEpilogue[] = {
    0xf9400bf3,  // ldr x19, [sp, #16]
    0xa8c27bfd,  // ldp x29, x30, [sp], #32
    0xd65f03c0,  // ret
};
#endif

static long GetSystemPageSize(void) {
  long pagesize;
  unassert((pagesize = sysconf(_SC_PAGESIZE)) > 0);
  unassert(IS2POW(pagesize));
  unassert(pagesize <= kJitPageSize);
  return pagesize;
}

static void DestroyJitPage(struct JitPage *jp) {
  dll_element *e;
  while ((e = dll_first(jp->staged))) {
    jp->staged = dll_remove(jp->staged, e);
    free(JITSTAGE_CONTAINER(e));
  }
  unassert(!munmap(jp->addr, kJitPageSize));
  free(jp);
}

/**
 * Initializes memory object for Just-In-Time (JIT) threader.
 *
 * The `jit` struct itself is owned by the caller. Internal memory
 * associated with this object should be reclaimed later by calling
 * DestroyJit().
 *
 * @return 0 on success
 */
int InitJit(struct Jit *jit) {
  memset(jit, 0, sizeof(*jit));
  pthread_mutex_init(&jit->lock, 0);
  return 0;
}

/**
 * Destroyes initialized JIT object.
 *
 * Passing a value not previously initialized by InitJit() is undefined.
 *
 * @return 0 on success
 */
int DestroyJit(struct Jit *jit) {
  dll_element *e;
  LOCK(&jit->lock);
  while ((e = dll_first(jit->pages))) {
    jit->pages = dll_remove(jit->pages, e);
    DestroyJitPage(JITPAGE_CONTAINER(e));
  }
  UNLOCK(&jit->lock);
  unassert(!pthread_mutex_destroy(&jit->lock));
  return 0;
}

/**
 * Disables Just-In-Time threader.
 */
int DisableJit(struct Jit *jit) {
  atomic_store_explicit(&jit->disabled, true, memory_order_release);
  return 0;
}

/**
 * Returns true if DisableJit() was called or AcquireJit() had failed.
 */
bool IsJitDisabled(struct Jit *jit) {
  return atomic_load_explicit(&jit->disabled, memory_order_acquire);
}

/**
 * Starts writing chunk of code to JIT memory.
 *
 * The returned object becomes owned by the calling thread until it is
 * relinquished by passing the result to ReleaseJit(). Any given chunk
 * can't exceed the JIT page size in length. Many chunks may be placed
 * in the same page by multiple threads.
 *
 * @param reserve is the anticipated number of bytes needed; passing
 *     a negative or unreasonably large number is undefined behavior
 * @return object representing a page of JIT memory, having at least
 *     `reserve` bytes of memory, or NULL if out of memory, in which
 *     case `jit` will enter the disabled state, after which this'll
 *     always return NULL
 */
struct JitPage *AcquireJit(struct Jit *jit, long reserve) {
  dll_element *e;
  intptr_t distance;
  struct JitPage *jp;
  unassert(reserve > 0);
  unassert(reserve <= kJitPageSize - sizeof(struct JitPage));
  LOCK(&jit->lock);
  if (!jit->disabled) {
    if (!jit->brk) {
      // we're going to politely ask the kernel for addresses starting
      // arbitrary megabytes past the end of our own executable's .bss
      // section. we'll cross our fingers, and hope that gives us room
      // away from a brk()-based libc malloc() function which may have
      // already allocated memory in this space. the reason it matters
      // is because the x86 and arm isas impose limits on displacement
      jit->brk = (u8 *)ROUNDUP((intptr_t)END_OF_IMAGE, kJitPageSize) + 1048576;
    }
    e = dll_first(jit->pages);
    if (e && (jp = JITPAGE_CONTAINER(e))->index + reserve <= kJitPageSize) {
      jit->pages = dll_remove(jit->pages, &jp->list);
    } else if ((jp = (struct JitPage *)calloc(1, sizeof(struct JitPage)))) {
      for (;;) {
        jp->addr = (u8 *)mmap(jit->brk, kJitPageSize, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_DEMAND, -1, 0);
        if (jp->addr != MAP_FAILED) {
          distance = ABS(jp->addr - END_OF_IMAGE);
          if (distance > kArmDispMax * 4 / 2) {
            LOG_ONCE(
                LOGF("mmap() returned suboptimal address %p that's %" PRIdPTR
                     " bytes away from our program image which ends near %p",
                     jp, distance, END_OF_IMAGE));
          }
          jit->brk = jp->addr + kJitPageSize;
          dll_init(&jp->list);
          break;
        } else if (errno == EEXIST) {
          jit->brk += kJitPageSize;
          continue;
        } else {
          LOGF("mmap() error at %p is %s", jit->brk, strerror(errno));
          DisableJit(jit);
          free(jp);
          jp = 0;
          break;
        }
      }
    }
  } else {
    jp = 0;
  }
  UNLOCK(&jit->lock);
  if (jp) {
    unassert(!(jp->start & (kJitPageAlign - 1)));
    unassert(jp->start == jp->index);
  }
  return jp;
}

/**
 * Returns number of bytes of space remaining in JIT memory page.
 *
 * @return number of bytes of space that can be appended into, or -1 if
 *     if an append operation previously failed due to lack of space
 */
long GetJitRemaining(struct JitPage *jp) {
  return kJitPageSize - jp->index;
}

/**
 * Returns current program counter or instruction pointer of JIT page.
 *
 * @return absolute instruction pointer memory address in bytes
 */
intptr_t GetJitPc(struct JitPage *jp) {
  return (intptr_t)jp->addr + jp->index;
}

/**
 * Appends bytes to JIT page.
 *
 * Errors here safely propagate to ReleaseJit().
 *
 * @return true if room was available, otherwise false
 */
bool AppendJit(struct JitPage *jp, const void *data, long size) {
  unassert(size > 0);
  if (size <= GetJitRemaining(jp)) {
    memcpy(jp->addr + jp->index, data, size);
    jp->index += size;
    return true;
  } else {
    jp->index = kJitPageSize + 1;
    return false;
  }
}

static int CommitJit(struct JitPage *jp, long pagesize) {
  long pageoff;
  int count = 0;
  dll_element *e;
  struct JitStage *js;
  unassert(jp->start == jp->index);
  unassert(!(jp->committed & (pagesize - 1)));
  pageoff = ROUNDDOWN(jp->start, pagesize);
  if (pageoff > jp->committed) {
    // 1. OpenBSD requires we maintain a W^X invariant.
    // 2. AARCH64 cache flush is so hard only the kernel knows how.
    unassert(jp->start == jp->index);
    unassert(!mprotect(jp->addr + jp->committed, pageoff - jp->committed,
                       PROT_READ | PROT_EXEC));
    unassert(jp->start == jp->index);
    while ((e = dll_first(jp->staged))) {
      js = JITSTAGE_CONTAINER(e);
      if (js->index <= pageoff) {
        atomic_store_explicit(js->hook, (intptr_t)jp->addr + js->start,
                              memory_order_release);
        jp->staged = dll_remove(jp->staged, e);
        free(js);
        ++count;
      } else {
        break;
      }
    }
    jp->committed = pageoff;
  }
  return count;
}

static void ReinsertPage(struct Jit *jit, struct JitPage *jp) {
  unassert(jp->start == jp->index);
  if (jp->index < kJitPageSize) {
    jit->pages = dll_make_first(jit->pages, &jp->list);
  } else {
    jit->pages = dll_make_last(jit->pages, &jp->list);
  }
}

/**
 * Forces pending hooks to be written out.
 */
int FlushJit(struct Jit *jit) {
  int count = 0;
  long pagesize;
  dll_element *e;
  struct JitPage *jp;
  struct JitStage *js;
  pagesize = GetSystemPageSize();
  LOCK(&jit->lock);
StartOver:
  for (e = dll_first(jit->pages); e; e = dll_next(jit->pages, e)) {
    jp = JITPAGE_CONTAINER(e);
    if (jp->start >= kJitPageSize) break;
    if (!dll_is_empty(jp->staged)) {
      jit->pages = dll_remove(jit->pages, e);
      UNLOCK(&jit->lock);
      js = JITSTAGE_CONTAINER(dll_last(jp->staged));
      jp->start = ROUNDUP(js->index, pagesize);
      jp->index = jp->start;
      count += CommitJit(jp, pagesize);
      LOCK(&jit->lock);
      ReinsertPage(jit, jp);
      goto StartOver;
    }
  }
  UNLOCK(&jit->lock);
  return count;
}

/**
 * Finishes writing chunk of code to JIT page.
 *
 * @return pointer to start of chunk, or NULL if an append operation
 *     had previously failed due to lack of space
 */
intptr_t ReleaseJit(struct Jit *jit, struct JitPage *jp, hook_t *hook,
                    intptr_t staging) {
  u8 *addr;
  struct JitStage *js;
  unassert(jp->index >= jp->start);
  unassert(jp->start >= jp->committed);
  if (jp->index > jp->start) {
    if (jp->index <= kJitPageSize) {
      addr = jp->addr + jp->start;
      jp->index = ROUNDUP(jp->index, kJitPageAlign);
      if (hook) {
        atomic_store_explicit(hook, staging, memory_order_release);
        if ((js = (struct JitStage *)calloc(1, sizeof(struct JitStage)))) {
          dll_init(&js->list);
          js->hook = hook;
          js->start = jp->start;
          js->index = jp->index;
          jp->staged = dll_make_last(jp->staged, &js->list);
        }
      }
      if (jp->index + kJitPageFit > kJitPageSize) {
        jp->index = kJitPageSize;
      }
    } else if (jp->start) {
      addr = 0;  // fail and let it try again
    } else {
      LOG_ONCE(LOGF("kJitPageSize needs to be increased"));
      if (hook) {
        atomic_store_explicit(hook, staging, memory_order_release);
      }
      addr = 0;
    }
    jp->start = jp->index;
    unassert(jp->start == jp->index);
    CommitJit(jp, GetSystemPageSize());
    unassert(jp->start == jp->index);
  } else {
    addr = 0;
  }
  LOCK(&jit->lock);
  ReinsertPage(jit, jp);
  UNLOCK(&jit->lock);
  return (intptr_t)addr;
}

/**
 * Begins writing function definition to JIT memory.
 *
 * This will acquire a page of JIT memory and insert a function
 * prologue. Code may be added to the function using methods like
 * AppendJitCall(). When a function is completed, FinishJit() should be
 * called. The calling thread is granted exclusive ownership of the
 * returned page of JIT memory, until it's relinquished by FinishJit().
 *
 * @return function builder object
 */
struct JitPage *StartJit(struct Jit *jit) {
  struct JitPage *jp;
  if ((jp = AcquireJit(jit, 4096))) {
    AppendJit(jp, kPrologue, sizeof(kPrologue));
  }
  return jp;
}

/**
 * Finishes writing function definition to JIT memory.
 *
 * Errors that happened earlier in AppendJit*() methods will safely
 * propagate to this function resulting in an error.
 *
 * @param jp is function builder object that was returned by StartJit();
 *     this function always relinquishes the calling thread's ownership
 *     of this object, even if this function returns an error
 * @return address of generated function, or NULL if an error occurred
 *     at some point in the function writing process
 */
intptr_t FinishJit(struct Jit *jit, struct JitPage *jp, hook_t *hook,
                   intptr_t staging) {
  AppendJit(jp, kEpilogue, sizeof(kEpilogue));
  return ReleaseJit(jit, jp, hook, staging);
}

/**
 * Abandons writing function definition to JIT memory.
 *
 * @param jp becomes owned by `jit` again after this call
 */
int AbandonJit(struct Jit *jit, struct JitPage *jp) {
  jp->index = jp->start;
  LOCK(&jit->lock);
  ReinsertPage(jit, jp);
  UNLOCK(&jit->lock);
  return 0;
}

/**
 * Finishes function by having it tail call a previously created one.
 *
 * Splicing a `chunk` that wasn't created by StartJit() is undefined.
 *
 * @param chunk is a jit function that was created earlier, or null in
 *     which case this method is identical to FinishJit()
 * @return address of generated function, or NULL if an error occurred
 *     at some point in the function writing process
 */
intptr_t SpliceJit(struct Jit *jit, struct JitPage *jp, hook_t *hook,
                   intptr_t staging, intptr_t chunk) {
  unassert(!chunk || !memcmp((u8 *)chunk, kPrologue, sizeof(kPrologue)));
  if (chunk) {
    AppendJitJmp(jp, (u8 *)chunk + sizeof(kPrologue));
    return ReleaseJit(jit, jp, hook, staging);
  } else {
    return FinishJit(jit, jp, hook, staging);
  }
}

static bool AppendJitMovReg(struct JitPage *jp, int dst, int src) {
#if defined(__x86_64__)
  unassert(!(dst & ~15));
  unassert(!(src & ~15));
  u8 buf[3];
  buf[0] = kAmdRexw | (dst & 8 ? kAmdRexr : 0) | (dst & 8 ? kAmdRexb : 0);
  buf[1] = 0x89;
  buf[2] = 0300 | (src & 7) << 3 | (dst & 7);
#elif defined(__aarch64__)
  //               src            target
  //              ┌─┴─┐           ┌─┴─┐
  // 0b10101010000000000000001111110011 mov x19, x0
  // 0b10101010000000010000001111110100 mov x20, x1
  // 0b10101010000101000000001111100001 mov x1, x20
  // 0b10101010000100110000001111100000 mov x0, x19
  unassert(!(dst & ~31));
  unassert(!(src & ~31));
  u32 buf[1] = {0xaa0003e0 | src << 16 | dst};
#endif
  return AppendJit(jp, buf, sizeof(buf));
}

/**
 * Sets function parameter.
 *
 * @param jp is function builder object returned by StartJit()
 * @param param is the 0-indexed function parameter (6 total)
 * @param value is the constant value to use as the parameter
 * @return true if room was available, otherwise false
 */
bool AppendJitSetArg(struct JitPage *jp, int param, u64 value) {
  unassert(0 <= param && param < 6);
  jp->setargs |= 1 << param;
#if defined(__x86_64__)
  u8 reg[6] = {kAmdDi, kAmdSi, kAmdDx, kAmdCx, 8, 9};
  param = reg[param];
#endif
  return AppendJitSetReg(jp, param, value);
}

/**
 * Appends function call instruction to JIT memory.
 *
 * @param jp is function builder object returned by StartJit()
 * @param func points to another callee function in memory
 * @return true if room was available, otherwise false
 */
bool AppendJitCall(struct JitPage *jp, void *func) {
  int n;
  intptr_t disp, addr;
  addr = (intptr_t)func;
#if defined(__x86_64__)
  u8 buf[5];
  if (~jp->setargs & 1) {
    AppendJitMovReg(jp, kAmdDi, kAmdBx);
  }
  jp->setargs = 0;
  disp = addr - (GetJitPc(jp) + 5);
  if (kAmdDispMin <= disp && disp <= kAmdDispMax) {
    // AMD function calls are encoded using an 0xE8 byte, followed by a
    // 32-bit signed two's complement little-endian integer, containing
    // the relative location between the function being called, and the
    // instruction at the location that follows our 5 byte call opcode.
    buf[0] = kAmdCall;
    Write32(buf + 1, disp & kAmdDispMask);
    n = 5;
  } else {
    AppendJitSetReg(jp, kAmdAx, addr);
    buf[0] = kAmdCallAx[0];
    buf[1] = kAmdCallAx[1];
    n = 2;
  }
#elif defined(__aarch64__)
  uint32_t buf[1];
  if (~jp->setargs & 1) {
    AppendJitMovReg(jp, 0, 19);
  }
  jp->setargs = 0;
  // ARM function calls are encoded as:
  //
  //       BL          displacement
  //     ┌─┴──┐┌────────────┴───────────┐
  //   0b100101sddddddddddddddddddddddddd
  //
  // Where:
  //
  //   - BL (0x94000000) is what ARM calls its CALL instruction
  //
  //   - sddddddddddddddddddddddddd is a 26-bit two's complement integer
  //     of how far away the function is that's being called. This is
  //     measured in terms of instructions rather than bytes. Unlike AMD
  //     the count here starts at the Program Counter (PC) address where
  //     the BL INSN is stored, rather than the one that follows.
  //
  // Displacement is computed as such:
  //
  //   INSN = BL | (((FUNC - PC) >> 2) & 0x03ffffffu)
  //
  // The inverse of the above operation is:
  //
  //   FUNC = PC + ((i32)((u32)(INSN & 0x03ffffffu) << 6) >> 4)
  //
  disp = (addr - GetJitPc(jp)) >> 2;
  unassert(kArmDispMin <= disp && disp <= kArmDispMax);
  buf[0] = kArmCall | (disp & kArmDispMask);
  n = 4;
#endif
  return AppendJit(jp, buf, n);
}

/**
 * Appends unconditional branch instruction to JIT memory.
 *
 * @param jp is function builder object returned by StartJit()
 * @param code points to some other code address in memory
 * @return true if room was available, otherwise false
 */
bool AppendJitJmp(struct JitPage *jp, void *code) {
  int n;
  intptr_t disp, addr;
  addr = (intptr_t)code;
#if defined(__x86_64__)
  u8 buf[5];
  disp = addr - (GetJitPc(jp) + 5);
  if (kAmdDispMin <= disp && disp <= kAmdDispMax) {
    buf[0] = kAmdJmp;
    Write32(buf + 1, disp & kAmdDispMask);
    n = 5;
  } else {
    AppendJitSetReg(jp, kAmdAx, addr);
    buf[0] = kAmdJmpAx[0];
    buf[1] = kAmdJmpAx[1];
    n = 2;
  }
#elif defined(__aarch64__)
  uint32_t buf[1];
  disp = (addr - GetJitPc(jp)) >> 2;
  unassert(kArmDispMin <= disp && disp <= kArmDispMax);
  buf[0] = kArmJmp | (disp & kArmDispMask);
  n = 4;
#endif
  return AppendJit(jp, buf, n);
}

/**
 * Sets register to immediate value.
 *
 * @param jp is function builder object returned by StartJit()
 * @param param is the zero-based index into the register file
 * @param value is the constant value to use as the parameter
 * @return true if room was available, otherwise false
 */
bool AppendJitSetReg(struct JitPage *jp, int reg, u64 value) {
  int n = 0;
#if defined(__x86_64__)
  u8 buf[10];
  u8 rex = 0;
  if (reg & 8) rex |= kAmdRexb;
  if (!value) {
    if (reg & 8) rex |= kAmdRexr;
    if (rex) buf[n++] = rex;
    buf[n++] = kAmdXor;
    buf[n++] = 0300 | (reg & 7) << 3 | (reg & 7);
  } else {
    if (value > 0xffffffff) rex |= kAmdRexw;
    if (rex) buf[n++] = rex;
    buf[n++] = kAmdMovImm | (reg & 7);
    if ((rex & kAmdRexw) != kAmdRexw) {
      Write32(buf + n, value);
      n += 4;
    } else {
      Write64(buf + n, value);
      n += 8;
    }
  }
#elif defined(__aarch64__)
  // ARM immediate moves are encoded as:
  //
  //     ┌64-bit
  //     │
  //     │┌{sign,???,zero,non}-extending
  //     ││
  //     ││       ┌short[4] index
  //     ││       │
  //     ││  MOV  │    immediate   register
  //     │├┐┌─┴──┐├┐┌──────┴───────┐┌─┴─┐
  //   0bmxx100101iivvvvvvvvvvvvvvvvrrrrr
  //
  // Which allows 16 bits to be loaded into a register at a time, with
  // tricks for clearing other parts of the register. For example, the
  // sign-extending mode will set the higher order shorts to all ones,
  // and it expects the immediate to be encoded using ones' complement
  int i;
  u32 op;
  u32 buf[4];
  unassert(!(reg & ~kArmRegMask));
  // TODO: This could be improved some more.
  if ((i64)value < 0 && (i64)value >= -0x8000) {
    buf[n++] = kArmMovSex | ~value << kArmImmOff | reg << kArmRegOff;
  } else {
    i = 0;
    op = kArmMovZex;
    while (value && !(value & 0xffff)) {
      value >>= 16;
      ++i;
    }
    do {
      op |= (value & 0xffff) << kArmImmOff;
      op |= reg << kArmRegOff;
      op |= i++ << kArmIdxOff;
      buf[n++] = op;
      op = kArmMovNex;
    } while ((value >>= 16));
  }
  n *= 4;
#endif
  return AppendJit(jp, buf, n);
}

#else
// clang-format off
#define STUB(RETURN, NAME, PARAMS, RESULT) \
  RETURN NAME PARAMS {                     \
    return RESULT;                         \
  }
STUB(int, InitJit, (struct Jit *jit), 0)
STUB(int, DestroyJit, (struct Jit *jit), 0)
STUB(int, DisableJit, (struct Jit *jit), 0)
STUB(bool, IsJitDisabled, (struct Jit *jit), 1)
STUB(struct JitPage *, AcquireJit, (struct Jit *jit, long reserve), 0)
STUB(long, GetJitRemaining, (struct JitPage *jp), 0)
STUB(intptr_t, GetJitPc, (struct JitPage *jp), 0)
STUB(bool, AppendJit, (struct JitPage *jp, const void *data, long size), 0)
STUB(intptr_t, ReleaseJit, (struct Jit *jit, struct JitPage *jp, hook_t *hook, intptr_t staging), 0)
STUB(int, AbandonJit, (struct Jit *jit, struct JitPage *jp), 0);
STUB(int, FlushJit, (struct Jit *jit), 0);
STUB(struct JitPage *, StartJit, (struct Jit *jit), 0)
STUB(intptr_t, FinishJit, (struct Jit *jit, struct JitPage *jp, hook_t *hook, intptr_t staging), 0)
STUB(bool, AppendJitJmp, (struct JitPage *jp, void *code), 0)
STUB(bool, AppendJitCall, (struct JitPage *jp, void *func), 0)
STUB(bool, AppendJitSetReg, (struct JitPage *jp, int reg, u64 value), 0)
STUB(bool, AppendJitSetArg, (struct JitPage *jp, int param, u64 value), 0)
STUB(intptr_t, SpliceJit, (struct Jit *jit, struct JitPage *jp, hook_t *hook, intptr_t staging, intptr_t chunk), 0);
#endif