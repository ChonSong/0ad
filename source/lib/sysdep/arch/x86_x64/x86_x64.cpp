/* Copyright (C) 2026 Wildfire Games.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * CPU-specific routines common to 32 and 64-bit x86
 */

#include "precompiled.h"

#include "x86_x64.h"

#include "lib/bits.h"
#include "lib/code_annotation.h"
#include "lib/debug.h"
#include "lib/lib.h"
#include "lib/module_init.h"
#include "lib/posix/posix_pthread.h"
#include "lib/secure_crt.h"
#include "lib/status.h"
#include "lib/sysdep/cpu.h"
#include "lib/timer.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if MSC_VERSION
# include <intrin.h>	// __rdtsc
#endif

namespace x86_x64 {

#if defined(_MSC_FULL_VER) && _MSC_FULL_VER >= 150030729
// VC10+ and VC9 SP1: __cpuidex is already available
#elif GCC_VERSION
#  define __cpuidex(regsArray, level, index)\
	__asm__ __volatile__ ("cpuid"\
		: "=a" ((regsArray)[0]), "=b" ((regsArray)[1]), "=c" ((regsArray)[2]), "=d" ((regsArray)[3])\
		: "0" (level), "2" (index));
#else
# error "compiler not supported"
#endif


// some of this module's functions are frequently called but require
// non-trivial initialization, so caching is helpful. isInitialized
// flags aren't thread-safe, so we use ModuleInit. calling it from
// every function is a bit wasteful, but it is convenient to avoid
// requiring users to pass around a global state object.
// one big Init() would be prone to deadlock if its subroutines also
// call a public function (that re-enters ModuleInit), so each
// function gets its own initState.

//-----------------------------------------------------------------------------
// CPUID

static void Invoke_cpuid(CpuidRegs* regs)
{
	cassert(sizeof(regs->eax) == sizeof(int));
	cassert(sizeof(*regs) == 4*sizeof(int));
	__cpuidex((int*)regs, regs->eax, regs->ecx);
}

static std::uint32_t cpuid_maxFunction;
static std::uint32_t cpuid_maxExtendedFunction;

static Status InitCpuid()
{
	CpuidRegs regs = { 0 };

	regs.eax = 0;
	Invoke_cpuid(&regs);
	cpuid_maxFunction = regs.eax;

	regs.eax = 0x80000000;
	Invoke_cpuid(&regs);
	cpuid_maxExtendedFunction = regs.eax;

	return INFO::OK;
}

bool cpuid(CpuidRegs* regs)
{
	static ModuleInitState initState{ 0 };
	ModuleInit(&initState, InitCpuid);

	const std::uint32_t function = regs->eax;
	if(function > cpuid_maxExtendedFunction)
		return false;
	if(function < 0x80000000 && function > cpuid_maxFunction)
		return false;

	Invoke_cpuid(regs);
	return true;
}


//-----------------------------------------------------------------------------
// capability bits

// treated as 128 bit field; order: std ecx, std edx, ext ecx, ext edx
// keep in sync with enum Cap!
static std::uint32_t caps[4];

static ModuleInitState capsInitState{ 0 };

static Status InitCaps()
{
	CpuidRegs regs = { 0 };
	regs.eax = 1;
	if(cpuid(&regs))
	{
		caps[0] = regs.ecx;
		caps[1] = regs.edx;
	}
	regs.eax = 0x80000001;
	if(cpuid(&regs))
	{
		caps[2] = regs.ecx;
		caps[3] = regs.edx;
	}

	return INFO::OK;
}

bool Cap(Caps cap)
{
	ModuleInit(&capsInitState, InitCaps);

	const size_t index = cap >> 5;
	const size_t bit = cap & 0x1F;
	if(index >= ARRAY_SIZE(caps))
	{
		DEBUG_WARN_ERR(ERR::INVALID_PARAM);
		return false;
	}
	return IsBitSet(caps[index], bit);
}

void GetCapBits(std::uint32_t* d0, std::uint32_t* d1, std::uint32_t* d2, std::uint32_t* d3)
{
	ModuleInit(&capsInitState, InitCaps);

	*d0 = caps[0];
	*d1 = caps[1];
	*d2 = caps[2];
	*d3 = caps[3];
}


//-----------------------------------------------------------------------------
// vendor

static Vendors vendor;

static Status InitVendor()
{
	CpuidRegs regs = { 0 };
	regs.eax = 0;
	if(!cpuid(&regs))
		DEBUG_WARN_ERR(ERR::CPU_FEATURE_MISSING);

	// copy regs to string
	// note: 'strange' ebx,edx,ecx reg order is due to ModR/M encoding order.
	char vendorString[13];
	memcpy(&vendorString[0], &regs.ebx, 4);
	memcpy(&vendorString[4], &regs.edx, 4);
	memcpy(&vendorString[8], &regs.ecx, 4);
	vendorString[12] = '\0';	// 0-terminate

	if(!strcmp(vendorString, "AuthenticAMD"))
		vendor = x86_x64::VENDOR_AMD;
	else if(!strcmp(vendorString, "GenuineIntel"))
		vendor = x86_x64::VENDOR_INTEL;
	else
	{
		DEBUG_WARN_ERR(ERR::CPU_UNKNOWN_VENDOR);
		vendor = x86_x64::VENDOR_UNKNOWN;
	}

	return INFO::OK;
}

Vendors Vendor()
{
	static ModuleInitState initState{ 0 };
	ModuleInit(&initState, InitVendor);
	return vendor;
}


//-----------------------------------------------------------------------------
// signature

static size_t m_Model;
static size_t m_Family;
static ModuleInitState signatureInitState{ 0 };

static Status InitSignature()
{
	CpuidRegs regs = { 0 };
	regs.eax = 1;
	if(!cpuid(&regs))
		DEBUG_WARN_ERR(ERR::CPU_FEATURE_MISSING);
	m_Model = bits(regs.eax, 4, 7);
	m_Family = bits(regs.eax, 8, 11);
	const size_t extendedModel = bits(regs.eax, 16, 19);
	const size_t extendedFamily = bits(regs.eax, 20, 27);
	if(m_Family == 0xF)
		m_Family += extendedFamily;
	if(m_Family == 0xF || (Vendor() == x86_x64::VENDOR_INTEL && m_Family == 6))
		m_Model += extendedModel << 4;
	return INFO::OK;
}

size_t Model()
{
	ModuleInit(&signatureInitState, InitSignature);
	return m_Model;
}

size_t Family()
{
	ModuleInit(&signatureInitState, InitSignature);
	return m_Family;
}




//-----------------------------------------------------------------------------
// identifier string

/// functor to remove substrings from the CPU identifier string
class StringStripper
{
public:
	StringStripper(char* string, size_t max_chars)
		: m_string(string), m_max_chars(max_chars)
	{
	}

	// remove all instances of substring from m_string
	void operator()(const char* substring)
	{
		const size_t substring_length = strlen(substring);
		for(;;)
		{
			char* substring_pos = strstr(m_string, substring);
			if(!substring_pos)
				break;
			const size_t substring_ofs = substring_pos - m_string;
			const size_t num_chars = m_max_chars - substring_ofs - substring_length;
			memmove(substring_pos, substring_pos+substring_length, num_chars);
		}
	}

private:
	char* m_string;
	size_t m_max_chars;
};

// 3 calls x 4 registers x 4 bytes = 48 + 0-terminator
static char identifierString[48+1];

static Status InitIdentifierString()
{
	// get brand string (if available)
	char* pos = identifierString;
	bool gotBrandString = true;
	for(std::uint32_t function = 0x80000002; function <= 0x80000004; function++)
	{
		CpuidRegs regs = { 0 };
		regs.eax = function;
		gotBrandString &= cpuid(&regs);
		memcpy(pos, &regs, 16);
		pos += 16;
	}

	// fall back to manual detect of CPU type because either:
	// - CPU doesn't support brand string (we use a flag to indicate this
	//   rather than comparing against a default value because it is safer);
	// - the brand string is useless, e.g. "Unknown". this happens on
	//   some older boards whose BIOS reprograms the string for CPUs it
	//   doesn't recognize.
	if(!gotBrandString || strncmp(identifierString, "Unknow", 6) == 0)
	{
		const size_t family = Family();
		const size_t model = Model();
		switch(Vendor())
		{
		case x86_x64::VENDOR_AMD:
			// everything else is either too old, or should have a brand string.
			if(family == 6)
			{
				if(model == 3 || model == 7)
					strcpy_s(identifierString, ARRAY_SIZE(identifierString), "AMD Duron");
				else if(model <= 5)
					strcpy_s(identifierString, ARRAY_SIZE(identifierString), "AMD Athlon");
				else
				{
					if(Cap(x86_x64::CAP_AMD_MP))
						strcpy_s(identifierString, ARRAY_SIZE(identifierString), "AMD Athlon MP");
					else
						strcpy_s(identifierString, ARRAY_SIZE(identifierString), "AMD Athlon XP");
				}
			}
			break;

		case x86_x64::VENDOR_INTEL:
			// everything else is either too old, or should have a brand string.
			if(family == 6)
			{
				if(model == 1)
					strcpy_s(identifierString, ARRAY_SIZE(identifierString), "Intel Pentium Pro");
				else if(model == 3 || model == 5)
					strcpy_s(identifierString, ARRAY_SIZE(identifierString), "Intel Pentium II");
				else if(model == 6)
					strcpy_s(identifierString, ARRAY_SIZE(identifierString), "Intel Celeron");
				else
					strcpy_s(identifierString, ARRAY_SIZE(identifierString), "Intel Pentium III");
			}
			break;

		default:
			strcpy_s(identifierString, ARRAY_SIZE(identifierString), "Unknown, non-Intel/AMD");
			break;
		}
	}
	// identifierString already holds a valid brand string; pretty it up.
	else
	{
		const char* const undesiredStrings[] = { "(tm)", "(TM)", "(R)", "CPU ", "          " };
		std::for_each(undesiredStrings, undesiredStrings+ARRAY_SIZE(undesiredStrings),
			StringStripper(identifierString, strlen(identifierString)+1));

		// note: Intel brand strings include a frequency, but we can't rely
		// on it because the CPU may be overclocked. we'll leave it in the
		// string to show measurement accuracy and if SpeedStep is active.
	}

	return INFO::OK;
}

static const char* IdentifierString()
{
	static ModuleInitState initState{ 0 };
	ModuleInit(&initState, InitIdentifierString);
	return identifierString;
}


//-----------------------------------------------------------------------------
// miscellaneous stateless functions

#if !MSC_VERSION	// ensure not already defined in header
std::uint64_t rdtsc()
{
#if GCC_VERSION
	// GCC supports "portable" assembly for both x86 and x64
	volatile std::uint32_t lo, hi;
	__asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
	return u64_from_u32(hi, lo);
#endif
}
#endif


void DebugBreak()
{
#if MSC_VERSION
	__debugbreak();
#elif GCC_VERSION
	// note: this probably isn't necessary, since unix_debug_break
	// (SIGTRAP) is most probably available if GCC_VERSION.
	// we include it for completeness, though.
	__asm__ __volatile__ ("int $3");
#endif
}

}	// namespace x86_x64


const char* cpu_IdentifierString()
{
	return x86_x64::IdentifierString();
}
