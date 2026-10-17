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
 * Windows implementation of sysdep/cpu
 */

#include "precompiled.h"
#include "lib/sysdep/os_cpu.h"

#include "lib/bits.h"
#include "lib/alignment.h"
#include "lib/module_init.h"
#include "lib/sysdep/os/win/wutil.h"
#include "lib/sysdep/arch/x86_x64/x86_x64.h"

#include <bit>


uintptr_t os_cpu_ProcessorMask()
{
	static uintptr_t processorMask;

	if(!processorMask)
	{
		const HANDLE hProcess = GetCurrentProcess();
		DWORD_PTR processAffinity, systemAffinity;
		const BOOL ok = GetProcessAffinityMask(hProcess, &processAffinity, &systemAffinity);
		ENSURE(ok);
		ENSURE(processAffinity != 0);
		processorMask = processAffinity;
	}

	return processorMask;
}


size_t os_cpu_NumProcessors()
{
	static size_t numProcessors;

	if(!numProcessors)
	{
		numProcessors = std::popcount(os_cpu_ProcessorMask());

		// sanity check
		SYSTEM_INFO si;
		GetSystemInfo(&si);	// guaranteed to succeed
		ENSURE(numProcessors <= (size_t)si.dwNumberOfProcessors);
		ENSURE(numProcessors >= 1);
	}

	return numProcessors;
}


size_t os_cpu_PageSize()
{
	static size_t systemPageSize;

	if(!systemPageSize)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);	// guaranteed to succeed
		systemPageSize = (size_t)si.dwPageSize;
	}

	return systemPageSize;
}


size_t os_cpu_LargePageSize()
{
	static size_t largePageSize = ~(size_t)0;	// "0" has special significance

	if(largePageSize == ~(size_t)0)
	{
		largePageSize = GetLargePageMinimum();
		// Note: checks disabled due to failing on Vista SP2 with old Xeon CPU
		//	see https://gitea.wildfiregames.com/0ad/0ad/issues/2346
		//ENSURE(largePageSize != 0);	// IA-32 and AMD64 definitely support large pages
		//ENSURE(largePageSize > os_cpu_PageSize());
	}

	return largePageSize;
}


static void GetMemoryStatus(MEMORYSTATUSEX& mse)
{
	// note: we no longer bother dynamically importing GlobalMemoryStatusEx -
	// it's available on Win2k and above. this function safely handles
	// systems with > 4 GB of memory.
	mse.dwLength = sizeof(mse);
	const BOOL ok = GlobalMemoryStatusEx(&mse);
	WARN_IF_FALSE(ok);
}

size_t os_cpu_MemoryAvailable()
{
	MEMORYSTATUSEX mse;
	GetMemoryStatus(mse);
	const size_t memoryAvailableMiB = size_t(mse.ullAvailPhys / MiB);
	return memoryAvailableMiB;
}
