/*
 * Copyright (c) 1997, 2016, Oracle and/or its affiliates. All rights reserved.
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

#ifndef CPU_AARCH64_VM_VM_VERSION_AARCH64_HPP
#define CPU_AARCH64_VM_VM_VERSION_AARCH64_HPP

#include "runtime/globals_extension.hpp"
#include "runtime/vm_version.hpp"
#include "utilities/sizes.hpp"

class VM_Version : public Abstract_VM_Version {
  friend class JVMCIVMStructs;

protected:
  static int _cpu;
  static int _model;
  static int _model2;
  static int _variant;
  static int _revision;
  static int _stepping;

  struct PsrInfo {
    uint32_t dczid_el0;
    uint32_t ctr_el0;
  };
  static PsrInfo _psr_info;
  static void get_processor_features();
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__APPLE__)
  static unsigned long os_get_processor_features();
#endif

public:
  // Initialization
  static void initialize();

  // Asserts
  static void assert_is_initialized() {
  }

  static bool expensive_load(int ld_size, int scale) {
    if (cpu_family() == CPU_ARM) {
      // Half-word load with index shift by 1 (aka scale is 2) has
      // extra cycle latency, e.g. ldrsh w0, [x1,w2,sxtw #1].
      if (ld_size == 2 && scale == 2) {
        return true;
      }
    }
    return false;
  }

  enum Family {
    CPU_ARM       = 'A',
    CPU_BROADCOM  = 'B',
    CPU_CAVIUM    = 'C',
    CPU_DEC       = 'D',
    CPU_INFINEON  = 'I',
    CPU_MOTOROLA  = 'M',
    CPU_NVIDIA    = 'N',
    CPU_AMCC      = 'P',
    CPU_QUALCOM   = 'Q',
    CPU_MARVELL   = 'V',
    CPU_INTEL     = 'i',
  };

  enum Feature_Flag {
    CPU_FP           = (1<<0),
    CPU_ASIMD        = (1<<1),
    CPU_EVTSTRM      = (1<<2),
    CPU_AES          = (1<<3),
    CPU_PMULL        = (1<<4),
    CPU_SHA1         = (1<<5),
    CPU_SHA2         = (1<<6),
    CPU_CRC32        = (1<<7),
    CPU_LSE          = (1<<8),
    CPU_STXR_PREFETCH= (1 << 29),
    CPU_A53MAC       = (1 << 30),
    CPU_DMB_ATOMICS  = (1 << 31),
  };

  static int cpu_family()                     { return _cpu; }
  static int cpu_model()                      { return _model; }
  static int cpu_model2()                     { return _model2; }
  static int cpu_variant()                    { return _variant; }
  static int cpu_revision()                   { return _revision; }
  static ByteSize dczid_el0_offset() { return byte_offset_of(PsrInfo, dczid_el0); }
  static ByteSize ctr_el0_offset()   { return byte_offset_of(PsrInfo, ctr_el0); }
#ifndef __APPLE__
  static bool is_zva_enabled() {
    // Check the DZP bit (bit 4) of dczid_el0 is zero
    // and block size (bit 0~3) is not zero.
    return ((_psr_info.dczid_el0 & 0x10) == 0 &&
            (_psr_info.dczid_el0 & 0xf) != 0);
  }
  static int zva_length() {
    assert(is_zva_enabled(), "ZVA not available");
    return 4 << (_psr_info.dczid_el0 & 0xf);
  }
#else
  static bool is_zva_enabled() { return true; }
  static int zva_length() { return 16; } 
  /* MCApollo:
     Assuming 16, this number gets 4*(x).
     4 << (1 & 0xf) == 8 => therefore this should be a number of 8^N.
     FIXME: zva
  */
#endif
  static int icache_line_size() {
#ifdef __APPLE__
    // FIXME: icache crash.
    return 64;
#else
    return (1 << (_psr_info.ctr_el0 & 0x0f)) * 4;
#endif
  }
  static int dcache_line_size() {
#ifdef __APPLE__
    /* MCApollo:
       Crash here => vm_version_aarch64.cpp:130
       Assume 64 (sysctl hw.cachelinesize)
       FIXME: dcache crash.
    */
    return 64; 
#else
    return (1 << ((_psr_info.ctr_el0 >> 16) & 0x0f)) * 4;
#endif
  }
};

#endif // CPU_AARCH64_VM_VM_VERSION_AARCH64_HPP
