/*
 * Copyright (c) 1999, 2018, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, Red Hat Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

// no precompiled headers
#include "jvm.h"
#include "asm/macroAssembler.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "code/nativeInst.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/allocation.inline.hpp"
#include "os_share_bsd.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/extendedPC.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/osThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/timer.hpp"
#include "utilities/debug.hpp"
#include "utilities/events.hpp"
#include "utilities/vmError.hpp"

// put OS-includes here
# include <sys/types.h>
# include <sys/mman.h>
# include <pthread.h>
# include <signal.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdlib.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/utsname.h>
# include <sys/socket.h>
# include <sys/wait.h>
# include <pwd.h>
# include <poll.h>
#ifdef __FreeBSD__
# include <ucontext.h>
# include <sys/sysctl.h>
# include <sys/procctl.h>
# ifndef PROC_STACKGAP_STATUS
#  define PROC_STACKGAP_STATUS	18
# endif
# ifndef PROC_STACKGAP_DISABLE
#  define PROC_STACKGAP_DISABLE	0x0002
# endif
#endif /* __FreeBSD__ */
#ifdef __APPLE__
# include <machine/_mcontext.h>
# include <sys/_types/_ucontext64.h>
#endif

#define REG_FP 29

NOINLINE address os::current_stack_pointer() {
  return (address)__builtin_frame_address(0);
}

char* os::non_memory_address_word() {
  // Must never look like an address returned by reserve_memory,
  // even in its subfields (as defined by the CPU immediate fields,
  // if the CPU splits constants across multiple instructions).

  return (char*) 0xffffffffffff;
}

address os::Bsd::ucontext_get_pc(const ucontext_t * uc) {
#if defined(__FreeBSD__)
  return (address)uc->uc_mcontext.mc_gpregs.gp_elr;
#elif defined(__OpenBSD__)
  return (address)uc->sc_elr;
#elif defined(__APPLE__)
  return (address)uc->uc_mcontext->__ss.__pc;
#endif
}

void os::Bsd::ucontext_set_pc(ucontext_t * uc, address pc) {
#if defined(__FreeBSD__)
  uc->uc_mcontext.mc_gpregs.gp_elr = (intptr_t)pc;
#elif defined(__OpenBSD__)
  uc->sc_elr = (unsigned long)pc;
#elif defined(__APPLE__)
  uc->uc_mcontext->__ss.__pc = (intptr_t)pc;
#endif
}

intptr_t* os::Bsd::ucontext_get_sp(const ucontext_t * uc) {
#if defined(__FreeBSD__)
  return (intptr_t*)uc->uc_mcontext.mc_gpregs.gp_sp;
#elif defined(__OpenBSD__)
  return (intptr_t*)uc->sc_sp;
#elif defined(__APPLE__)
  return (intptr_t*)uc->uc_mcontext->__ss.__sp;
#endif
}

intptr_t* os::Bsd::ucontext_get_fp(const ucontext_t * uc) {
#if defined(__FreeBSD__)
  return (intptr_t*)uc->uc_mcontext.mc_gpregs.gp_x[REG_FP];
#elif defined(__OpenBSD__)
  return (intptr_t*)uc->sc_x[REG_FP];
#elif defined(__APPLE__)
  return (intptr_t*)uc->uc_mcontext->__ss.__x[REG_FP];
#endif
}

// For Forte Analyzer AsyncGetCallTrace profiling support - thread
// is currently interrupted by SIGPROF.
// os::Solaris::fetch_frame_from_ucontext() tries to skip nested signal
// frames. Currently we don't do that on Bsd, so it's the same as
// os::fetch_frame_from_context().
ExtendedPC os::Bsd::fetch_frame_from_ucontext(Thread* thread,
  const ucontext_t* uc, intptr_t** ret_sp, intptr_t** ret_fp) {

  assert(thread != NULL, "just checking");
  assert(ret_sp != NULL, "just checking");
  assert(ret_fp != NULL, "just checking");

  return os::fetch_frame_from_context(uc, ret_sp, ret_fp);
}

ExtendedPC os::fetch_frame_from_context(const void* ucVoid,
                    intptr_t** ret_sp, intptr_t** ret_fp) {

  ExtendedPC  epc;
  const ucontext_t* uc = (const ucontext_t*)ucVoid;

  if (uc != NULL) {
    epc = ExtendedPC(os::Bsd::ucontext_get_pc(uc));
    if (ret_sp) *ret_sp = os::Bsd::ucontext_get_sp(uc);
    if (ret_fp) *ret_fp = os::Bsd::ucontext_get_fp(uc);
  } else {
    // construct empty ExtendedPC for return value checking
    epc = ExtendedPC(NULL);
    if (ret_sp) *ret_sp = (intptr_t *)NULL;
    if (ret_fp) *ret_fp = (intptr_t *)NULL;
  }

  return epc;
}

frame os::fetch_frame_from_context(const void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  ExtendedPC epc = fetch_frame_from_context(ucVoid, &sp, &fp);
  return frame(sp, fp, epc.pc());
}

bool os::Bsd::get_frame_at_stack_banging_point(JavaThread* thread, ucontext_t* uc, frame* fr) {
  address pc = (address) os::Bsd::ucontext_get_pc(uc);
  if (Interpreter::contains(pc)) {
    // interpreter performs stack banging after the fixed frame header has
    // been generated while the compilers perform it before. To maintain
    // semantic consistency between interpreted and compiled frames, the
    // method returns the Java sender of the current frame.
    *fr = os::fetch_frame_from_context(uc);
    if (!fr->is_first_java_frame()) {
      assert(fr->safe_for_sender(thread), "Safety check");
      *fr = fr->java_sender();
    }
  } else {
    // more complex code with compiled code
    assert(!Interpreter::contains(pc), "Interpreted methods should have been handled above");
    CodeBlob* cb = CodeCache::find_blob(pc);
    if (cb == NULL || !cb->is_nmethod() || cb->is_frame_complete_at(pc)) {
      // Not sure where the pc points to, fallback to default
      // stack overflow handling
      return false;
    } else {
      // In compiled code, the stack banging is performed before LR
      // has been saved in the frame.  LR is live, and SP and FP
      // belong to the caller.
      intptr_t* fp = os::Bsd::ucontext_get_fp(uc);
      intptr_t* sp = os::Bsd::ucontext_get_sp(uc);
#if defined(__FreeBSD__)
      address pc = (address)(uc->uc_mcontext.mc_gpregs.gp_lr
                         - NativeInstruction::instruction_size);
#elif defined(__OpenBSD__)
      address pc = (address)(uc->sc_lr
                         - NativeInstruction::instruction_size);
#elif defined(__APPLE__)
      address pc = (address)(uc->uc_mcontext->__ss.__lr
                         - NativeInstruction::instruction_size);
#endif
      *fr = frame(sp, fp, pc);
      if (!fr->is_java_frame()) {
        assert(fr->safe_for_sender(thread), "Safety check");
        assert(!fr->is_first_frame(), "Safety check");
        *fr = fr->java_sender();
      }
    }
  }
  assert(fr->is_java_frame(), "Safety check");
  return true;
}

// By default, gcc always saves frame pointer rfp on this stack. This
// may get turned off by -fomit-frame-pointer.
frame os::get_sender_for_C_frame(frame* fr) {
  return frame(fr->link(), fr->link(), fr->sender_pc());
}

NOINLINE frame os::current_frame() {
  intptr_t *fp = *(intptr_t **)__builtin_frame_address(0);
  frame myframe((intptr_t*)os::current_stack_pointer(),
                (intptr_t*)fp,
                CAST_FROM_FN_PTR(address, os::current_frame));
  if (os::is_first_C_frame(&myframe)) {
    // stack is not walkable
    return frame();
  } else {
    return os::get_sender_for_C_frame(&myframe);
  }
}

// Utility functions
extern "C" JNIEXPORT int
JVM_handle_bsd_signal(int sig,
                        siginfo_t* info,
                        void* ucVoid,
                        int abort_if_unrecognized) {
  ucontext_t* uc = (ucontext_t*) ucVoid;

  Thread* t = Thread::current_or_null_safe();

  // Must do this before SignalHandlerMark, if crash protection installed we will longjmp away
  // (no destructors can be run)
  os::ThreadCrashProtection::check_crash_protection(sig, t);

  SignalHandlerMark shm(t);

  // Note: it's not uncommon that JNI code uses signal/sigset to install
  // then restore certain signal handler (e.g. to temporarily block SIGPIPE,
  // or have a SIGILL handler when detecting CPU type). When that happens,
  // JVM_handle_bsd_signal() might be invoked with junk info/ucVoid. To
  // avoid unnecessary crash when libjsig is not preloaded, try handle signals
  // that do not require siginfo/ucontext first.

  if (sig == SIGPIPE || sig == SIGXFSZ) {
    // allow chained handler to go first
    if (os::Bsd::chained_handler(sig, info, ucVoid)) {
      return true;
    } else {
      // Ignoring SIGPIPE/SIGXFSZ - see bugs 4229104 or 6499219
      return true;
    }
  }

#ifdef CAN_SHOW_REGISTERS_ON_ASSERT
  if ((sig == SIGSEGV || sig == SIGBUS) && info != NULL && info->si_addr == g_assert_poison) {
    handle_assert_poison_fault(ucVoid, info->si_addr);
    return 1;
  }
#endif

  JavaThread* thread = NULL;
  VMThread* vmthread = NULL;
  if (os::Bsd::signal_handlers_are_installed) {
    if (t != NULL ){
      if(t->is_Java_thread()) {
        thread = (JavaThread*)t;
      }
      else if(t->is_VM_thread()){
        vmthread = (VMThread *)t;
      }
    }
  }
/*
  NOTE: does not seem to work on bsd.
  if (info == NULL || info->si_code <= 0 || info->si_code == SI_NOINFO) {
    // can't decode this kind of signal
    info = NULL;
  } else {
    assert(sig == info->si_signo, "bad siginfo");
  }
*/
  // decide if this trap can be handled by a stub
  address stub = NULL;

  address pc          = NULL;

  //%note os_trap_1
  if (info != NULL && uc != NULL && thread != NULL) {
    pc = (address) os::Bsd::ucontext_get_pc(uc);

    if (StubRoutines::is_safefetch_fault(pc)) {
      os::Bsd::ucontext_set_pc(uc, StubRoutines::continuation_for_safefetch_fault(pc));
      return 1;
    }

    // Handle ALL stack overflow variations here
    if (sig == SIGSEGV) {
      address addr = (address) info->si_addr;
#ifdef __FreeBSD__
      /*
       * Determine whether the kernel stack guard pages have been disabled
       */
      int status = 0;
      int ret = procctl(P_PID, getpid(), PROC_STACKGAP_STATUS, &status);

      /*
       * Check if the call to procctl(2) failed or the stack guard is not
       * disabled.  Either way, we'll then attempt a workaround.
       */
      if (ret == -1 || !(status & PROC_STACKGAP_DISABLE)) {
          /*
           * Try to work around the problems caused on FreeBSD where the kernel
           * may place guard pages above JVM guard pages and prevent the Java
           * thread stacks growing into the JVM guard pages.  The work around
           * is to determine how many such pages there may be and round down the
           * fault address so that tests of whether it is in the JVM guard zone
           * succeed.
           *
           * Note that this is a partial workaround at best since the normally
           * the JVM could then unprotect the reserved area to allow a critical
           * section to complete.  This is not possible if the kernel has
           * placed guard pages below the reserved area.
           *
           * This also suffers from the problem that the
           * security.bsd.stack_guard_page sysctl is dynamic and may have
           * changed since the stack was allocated.  This is likely to be rare
           * in practice though.
           *
           * What this does do is prevent the JVM crashing on FreeBSD and
           * instead throwing a StackOverflowError when infinite recursion
           * is attempted, which is the expected behaviour.  Due to it's
           * limitations though, objects may be in unexpected states when
           * this occurs.
           *
           * A better way to avoid these problems is either to be on a new
           * enough version of FreeBSD (one that has PROC_STACKGAP_CTL) or set
           * security.bsd.stack_guard_page to zero.
           */
          int guard_pages = 0;
          size_t size = sizeof(guard_pages);
          if (sysctlbyname("security.bsd.stack_guard_page",
                           &guard_pages, &size, NULL, 0) == 0 &&
              guard_pages > 0) {
            addr -= guard_pages * os::vm_page_size();
          }
      }
#endif

      // check if fault address is within thread stack
      if (thread->on_local_stack(addr)) {
        // stack overflow
        if (thread->in_stack_yellow_reserved_zone(addr)) {
          if (thread->thread_state() == _thread_in_Java) {
            if (thread->in_stack_reserved_zone(addr)) {
              frame fr;
              if (os::Bsd::get_frame_at_stack_banging_point(thread, uc, &fr)) {
                assert(fr.is_java_frame(), "Must be a Java frame");
                frame activation =
                  SharedRuntime::look_for_reserved_stack_annotated_method(thread, fr);
                if (activation.sp() != NULL) {
                  thread->disable_stack_reserved_zone();
                  if (activation.is_interpreted_frame()) {
                    thread->set_reserved_stack_activation((address)(
                      activation.fp() + frame::interpreter_frame_initial_sp_offset));
                  } else {
                    thread->set_reserved_stack_activation((address)activation.unextended_sp());
                  }
                  return 1;
                }
              }
            }
            // Throw a stack overflow exception.  Guard pages will be reenabled
            // while unwinding the stack.
            thread->disable_stack_yellow_reserved_zone();
            stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::STACK_OVERFLOW);
          } else {
            // Thread was in the vm or native code.  Return and try to finish.
            thread->disable_stack_yellow_reserved_zone();
            return 1;
          }
        } else if (thread->in_stack_red_zone(addr)) {
          // Fatal red zone violation.  Disable the guard pages and fall through
          // to handle_unexpected_exception way down below.
          thread->disable_stack_red_zone();
          tty->print_raw_cr("An irrecoverable stack overflow has occurred.");

          // This is a likely cause, but hard to verify. Let's just print
          // it as a hint.
          tty->print_raw_cr("Please check if any of your loaded .so files has "
                            "enabled executable stack (see man page execstack(8))");
        }
      }
    }

    if (thread->thread_state() == _thread_in_Java) {
      // Java thread running in Java code => find exception handler if any
      // a fault inside compiled code, the interpreter, or a stub

      // Handle signal from NativeJump::patch_verified_entry().
      if ((sig == SIGILL || sig == SIGTRAP)
          && nativeInstruction_at(pc)->is_sigill_zombie_not_entrant()) {
        if (TraceTraps) {
          tty->print_cr("trap: zombie_not_entrant (%s)", (sig == SIGTRAP) ? "SIGTRAP" : "SIGILL");
        }
        stub = SharedRuntime::get_handle_wrong_method_stub();
      } else if (sig == SIGSEGV && os::is_poll_address((address)info->si_addr)) {
        stub = SharedRuntime::get_poll_stub(pc);
      } else if (sig == SIGBUS /* && info->si_code == BUS_OBJERR */) {
        // BugId 4454115: A read from a MappedByteBuffer can fault
        // here if the underlying file has been truncated.
        // Do not crash the VM in such a case.
        CodeBlob* cb = CodeCache::find_blob_unsafe(pc);
        CompiledMethod* nm = (cb != NULL) ? cb->as_compiled_method_or_null() : NULL;
        if (nm != NULL && nm->has_unsafe_access()) {
          address next_pc = pc + NativeCall::instruction_size;
          stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
        }
      }
      else

      if (sig == SIGFPE  &&
          (info->si_code == FPE_INTDIV || info->si_code == FPE_FLTDIV)) {
        stub =
          SharedRuntime::
          continuation_for_implicit_exception(thread,
                                              pc,
                                              SharedRuntime::
                                              IMPLICIT_DIVIDE_BY_ZERO);
      } else if (sig == SIGSEGV &&
               !MacroAssembler::needs_explicit_null_check((intptr_t)info->si_addr)) {
          // Determination of interpreter/vtable stub/compiled code null exception
          stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
      }
    } else if (thread->thread_state() == _thread_in_vm &&
               sig == SIGBUS && /* info->si_code == BUS_OBJERR && */
               thread->doing_unsafe_access()) {
      address next_pc = pc + NativeCall::instruction_size;
      stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
    }

    // jni_fast_Get<Primitive>Field can trap at certain pc's if a GC kicks in
    // and the heap gets shrunk before the field access.
    if ((sig == SIGSEGV) || (sig == SIGBUS)) {
      address addr = JNI_FastGetField::find_slowcase_pc(pc);
      if (addr != (address)-1) {
        stub = addr;
      }
    }

    // Check to see if we caught the safepoint code in the
    // process of write protecting the memory serialization page.
    // It write enables the page immediately after protecting it
    // so we can just return to retry the write.
    if ((sig == SIGSEGV) &&
        os::is_memory_serialize_page(thread, (address) info->si_addr)) {
      // Block current thread until the memory serialize page permission restored.
      os::block_on_serialize_page_trap();
      return true;
    }
  }

  if (stub != NULL) {
    // save all thread context in case we need to restore it
    if (thread != NULL) thread->set_saved_exception_pc(pc);

    os::Bsd::ucontext_set_pc(uc, stub);
    return true;
  }

  // signal-chaining
  if (os::Bsd::chained_handler(sig, info, ucVoid)) {
     return true;
  }

  if (!abort_if_unrecognized) {
    // caller wants another chance, so give it to him
    return false;
  }

  if (pc == NULL && uc != NULL) {
    pc = os::Bsd::ucontext_get_pc(uc);
  }

  // unmask current signal
  sigset_t newset;
  sigemptyset(&newset);
  sigaddset(&newset, sig);
  sigprocmask(SIG_UNBLOCK, &newset, NULL);

  VMError::report_and_die(t, sig, pc, info, ucVoid);

  ShouldNotReachHere();
  return true; // Mute compiler
}

void os::Bsd::init_thread_fpu_state(void) {
}

bool os::supports_sse() {
  return true;
}

bool os::is_allocatable(size_t bytes) {
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// thread stack

// Minimum usable stack sizes required to get to user code. Space for
// HotSpot guard pages is added later.
size_t os::Posix::_compiler_thread_min_stack_allowed = 72 * K;
size_t os::Posix::_java_thread_min_stack_allowed = 72 * K;
size_t os::Posix::_vm_internal_thread_min_stack_allowed = 72 * K;

// return default stack size for thr_type
size_t os::Posix::default_stack_size(os::ThreadType thr_type) {
  // default stack size (compiler thread needs larger stack)
  size_t s = (thr_type == os::compiler_thread ? 4 * M : 1 * M);
  return s;
}

/////////////////////////////////////////////////////////////////////////////
// helper functions for fatal error handler

void os::print_context(outputStream *st, const void *context) {
  if (context == NULL) return;

  const ucontext_t *uc = (const ucontext_t*)context;
  st->print_cr("Registers:");
  for (int r = 0; r < 30; r++) {
    st->print("R%-2d=", r);
#if defined(__FreeBSD__)
    print_location(st, uc->uc_mcontext.mc_gpregs.gp_x[r]);
#elif defined(__OpenBSD__)
    print_location(st, uc->sc_x[r]);
#elif defined(__APPLE__)
    print_location(st, uc->uc_mcontext->__ss.__x[r]);
#endif
  }
  st->cr();

  intptr_t *sp = (intptr_t *)os::Bsd::ucontext_get_sp(uc);
  st->print_cr("Top of Stack: (sp=" PTR_FORMAT ")", p2i(sp));
  print_hex_dump(st, (address)sp, (address)(sp + 8*sizeof(intptr_t)), sizeof(intptr_t));
  st->cr();

  // Note: it may be unsafe to inspect memory near pc. For example, pc may
  // point to garbage if entry point in an nmethod is corrupted. Leave
  // this at the end, and hope for the best.
  address pc = os::Bsd::ucontext_get_pc(uc);
  print_instructions(st, pc, sizeof(char));
  st->cr();
}

void os::print_register_info(outputStream *st, const void *context) {
  if (context == NULL) return;
  const ucontext_t *uc = (const ucontext_t*)context;

  st->print_cr("Register to memory mapping:");
  st->cr();

  // this is horrendously verbose but the layout of the registers in the
  // context does not match how we defined our abstract Register set, so
  // we can't just iterate through the gregs area

  // this is only for the "general purpose" registers

  for (int r = 0; r < 30; r++)
#if defined(__FreeBSD__)
    st->print_cr(  "R%d=" INTPTR_FORMAT, r, (uintptr_t)uc->uc_mcontext.mc_gpregs.gp_x[r]);
#elif defined(__OpenBSD__)
    st->print_cr(  "R%d=" INTPTR_FORMAT, r, (uintptr_t)uc->sc_x[r]);
#elif defined(__APPLE__)
    st->print_cr(  "R%d=" INTPTR_FORMAT, r, (uintptr_t)uc->uc_mcontext->__ss.__x[r]);
#endif
  st->cr();
}

void os::setup_fpu() {
}

#ifndef PRODUCT
void os::verify_stack_alignment() {
  assert(((intptr_t)os::current_stack_pointer() & (StackAlignmentInBytes-1)) == 0, "incorrect stack alignment");
}
#endif

int os::extra_bang_size_in_bytes() {
  // AArch64 does not require the additional stack bang.
  return 0;
}

extern "C" {
  int SpinPause() {
    return 0;
  }

  void _Copy_conjoint_jshorts_atomic(const jshort* from, jshort* to, size_t count) {
    if (from > to) {
      const jshort *end = from + count;
      while (from < end)
        *(to++) = *(from++);
    }
    else if (from < to) {
      const jshort *end = from;
      from += count - 1;
      to   += count - 1;
      while (from >= end)
        *(to--) = *(from--);
    }
  }
  void _Copy_conjoint_jints_atomic(const jint* from, jint* to, size_t count) {
    if (from > to) {
      const jint *end = from + count;
      while (from < end)
        *(to++) = *(from++);
    }
    else if (from < to) {
      const jint *end = from;
      from += count - 1;
      to   += count - 1;
      while (from >= end)
        *(to--) = *(from--);
    }
  }
  void _Copy_conjoint_jlongs_atomic(const jlong* from, jlong* to, size_t count) {
    if (from > to) {
      const jlong *end = from + count;
      while (from < end)
        os::atomic_copy64(from++, to++);
    }
    else if (from < to) {
      const jlong *end = from;
      from += count - 1;
      to   += count - 1;
      while (from >= end)
        os::atomic_copy64(from--, to--);
    }
  }

  void _Copy_arrayof_conjoint_bytes(const HeapWord* from,
                                    HeapWord* to,
                                    size_t    count) {
    memmove(to, from, count);
  }
  void _Copy_arrayof_conjoint_jshorts(const HeapWord* from,
                                      HeapWord* to,
                                      size_t    count) {
    memmove(to, from, count * 2);
  }
  void _Copy_arrayof_conjoint_jints(const HeapWord* from,
                                    HeapWord* to,
                                    size_t    count) {
    memmove(to, from, count * 4);
  }
  void _Copy_arrayof_conjoint_jlongs(const HeapWord* from,
                                     HeapWord* to,
                                     size_t    count) {
    memmove(to, from, count * 8);
  }
};
