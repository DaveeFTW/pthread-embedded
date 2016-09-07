/*
 * psp_osal.c
 *
 * Description:
 *
 * --------------------------------------------------------------------------
 *
 *      Pthreads-embedded (PTE) - POSIX Threads Library for embedded systems
 *      Copyright(C) 2008 Jason Schmidlapp
 *
 *      Contact Email: jschmidlapp@users.sourceforge.net
 *
 *      This library is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU Lesser General Public
 *      License as published by the Free Software Foundation; either
 *      version 2 of the License, or (at your option) any later version.
 *
 *      This library is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *      Lesser General Public License for more details.
 *
 *      You should have received a copy of the GNU Lesser General Public
 *      License along with this library in the file COPYING.LIB;
 *      if not, write to the Free Software Foundation, Inc.,
 *      59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "pte_osal.h"
#include "pthread.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>

#include <vitasdk/utils.h>

/* For ftime */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/timeb.h>

// debug
#include <stdarg.h>

#define MAX_PSP_UID 2048 // SWAG

#define DEFAULT_STACK_SIZE_BYTES 0x1000

#define PTHREAD_EVID_CANCEL 0x1

#if 1
#define PSP_DEBUG(x) printf(x)
#else
#define PSP_DEBUG(x)
#endif

typedef struct tlsKeyEntry
{
	struct tlsKeyEntry *next;
	unsigned int key;
	void *value;
} tlsKeyEntry;

/*
 * Data stored on a per-thread basis - allocated in pte_osThreadCreate
 * and freed in pte_osThreadDelete.
 */
typedef struct pspThreadData
  {
    /* Entry point and parameters to thread's main function */
    pte_osThreadEntryPoint entryPoint;
    void * argv;

    /* Semaphore used for cancellation.  Posted to by pte_osThreadCancel, 
       polled in pte_osSemaphoreCancellablePend */
	SceUID evid;

  } pspThreadData;

static inline int invert_priority(int priority)
{
	return (pte_osThreadGetMinPriority() - priority) + pte_osThreadGetMaxPriority();
}

/* A new thread's stub entry point.  It retrieves the real entry point from the per thread control
 * data as well as any parameters to this function, and then calls the entry point.
 */
int pspStubThreadEntry (unsigned int argc, void *argv)
{
	pspThreadData *pThreadData = *(pspThreadData **)vitasdk_get_pthread_data(0);
	return (*(pThreadData->entryPoint))(pThreadData->argv);
}

/****************************************************************************
 *
 * Initialization
 *
 ***************************************************************************/

pte_osResult pte_osInit(void)
{
	/* Allocate some memory for our per-thread control data.  We use this for:
	 * 1. Entry point and parameters for the user thread's main function.
	 * 2. Semaphore used for thread cancellation.
	 */
	pspThreadData *pThreadData = (pspThreadData *)malloc(sizeof(pspThreadData));

	if (pThreadData == NULL)
	{
		PSP_DEBUG("malloc(pspThreadData): PTE_OS_NO_RESOURCES\n");
		return PTE_OS_NO_RESOURCES;
	}

	pspThreadData **addr = (pspThreadData **)vitasdk_get_pthread_data(0);

	pThreadData->evid = sceKernelCreateEventFlag("", 0, 0, NULL);

	*addr = pThreadData;
	return PTE_OS_OK;
}

/****************************************************************************
 *
 * Threads
 *
 ***************************************************************************/

pte_osResult pte_osThreadCreate(pte_osThreadEntryPoint entryPoint,
                                int stackSize,
                                int initialPriority,
                                void *argv,
                                pte_osThreadHandle* ppte_osThreadHandle)
{
	SceUID thid;

	if (stackSize < DEFAULT_STACK_SIZE_BYTES)
		stackSize = DEFAULT_STACK_SIZE_BYTES;

	/* Allocate some memory for our per-thread control data.  We use this for:
	 * 1. Entry point and parameters for the user thread's main function.
	 * 2. Semaphore used for thread cancellation.
	 */
	pspThreadData *pThreadData = (pspThreadData *)malloc(sizeof(pspThreadData));

	if (pThreadData == NULL)
	{
		PSP_DEBUG("malloc(pspThreadData): PTE_OS_NO_RESOURCES\n");
		return PTE_OS_NO_RESOURCES;
	}

	thid = sceKernelCreateThread("",
								 pspStubThreadEntry,
								 invert_priority(initialPriority),
								 stackSize,
								 0,
								 0,
								 NULL);

	if (thid < 0)
	{
		// TODO: expand this further
		if (thid == SCE_KERNEL_ERROR_NO_MEMORY)
		{
			PSP_DEBUG("sceKernelCreateThread: PTE_OS_NO_RESOURCES\n");
			return PTE_OS_NO_RESOURCES;
		}
		else
		{
			PSP_DEBUG("sceKernelCreateThread: PTE_OS_GENERAL_FAILURE\n");
			return PTE_OS_GENERAL_FAILURE;
		}
	}

	pspThreadData **addr = (pspThreadData **)vitasdk_get_pthread_data(thid);

	*ppte_osThreadHandle = thid;
	pThreadData->entryPoint = entryPoint;
	pThreadData->argv = argv;
	pThreadData->evid = sceKernelCreateEventFlag("", 0, 0, NULL);

	*addr = pThreadData;
	return PTE_OS_OK;
}

pte_osResult pte_osThreadStart(pte_osThreadHandle osThreadHandle)
{
	sceKernelStartThread(osThreadHandle, 0, NULL);
	return PTE_OS_OK;
}

pte_osResult pte_osThreadDelete(pte_osThreadHandle handle)
{
	pspThreadData *pThreadData = *(pspThreadData **)vitasdk_get_pthread_data(handle);
	sceKernelDeleteEventFlag(pThreadData->evid);
	free(pThreadData);
	tlsKeyEntry *entry = *(tlsKeyEntry **)vitasdk_get_tls_data(handle);

	while (entry)
	{
		tlsKeyEntry *ent2 = entry;
		entry = entry->next;
		free(ent2);
	}

	*(tlsKeyEntry **)vitasdk_get_tls_data(handle) = NULL;
	vitasdk_delete_thread_reent(handle);
	sceKernelDeleteThread(handle);
	return PTE_OS_OK;
}

pte_osResult pte_osThreadExitAndDelete(pte_osThreadHandle handle)
{
	pte_osThreadDelete(handle);
	sceKernelExitDeleteThread(0);
	return PTE_OS_OK;
}

void pte_osThreadExit()
{
	sceKernelExitThread(0);
}

/*
 * This has to be cancellable, so we can't just call sceKernelWaitThreadEnd.
 * Instead, poll on this in a loop, like we do for a cancellable semaphore.
 */
pte_osResult pte_osThreadWaitForEnd(pte_osThreadHandle threadHandle)
{
	int status = 0;
	pspThreadData *pThreadData = *(pspThreadData **)vitasdk_get_pthread_data(0);

	while (1)
	{
		unsigned int bits = 0;
		sceKernelPollEventFlag(pThreadData->evid, PTHREAD_EVID_CANCEL, SCE_EVENT_WAITAND, &bits);

		if (bits & PTHREAD_EVID_CANCEL)
		{
			return PTE_OS_INTERRUPTED;
		}

		SceUInt timeout = POLLING_DELAY_IN_us;
		int res = sceKernelWaitThreadEndCB(threadHandle, &status, &timeout);

		if (res < 0)
		{
			// TODO: associate error codes better
			if (res == SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			{
				continue;
			}
			else
			{
				return PTE_OS_GENERAL_FAILURE;
			}
		}

		break;
	}

	return PTE_OS_OK;
}

pte_osThreadHandle pte_osThreadGetHandle(void)
{
	return sceKernelGetThreadId();
}

int pte_osThreadGetPriority(pte_osThreadHandle threadHandle)
{
	SceKernelThreadInfo thinfo;
	thinfo.size = sizeof(SceKernelThreadInfo);
	sceKernelGetThreadInfo(threadHandle, &thinfo);
	return invert_priority(thinfo.currentPriority);
}

pte_osResult pte_osThreadSetPriority(pte_osThreadHandle threadHandle, int newPriority)
{
	sceKernelChangeThreadPriority(threadHandle, invert_priority(newPriority));
	return PTE_OS_OK;
}

pte_osResult pte_osThreadCancel(pte_osThreadHandle threadHandle)
{
	pspThreadData *pThreadData = *(pspThreadData **)vitasdk_get_pthread_data(threadHandle);

	int res = sceKernelSetEventFlag(pThreadData->evid, PTHREAD_EVID_CANCEL);

	if (res < 0)
		return PTE_OS_GENERAL_FAILURE;

	return PTE_OS_OK;
}


pte_osResult pte_osThreadCheckCancel(pte_osThreadHandle threadHandle)
{
	pspThreadData *pThreadData = *(pspThreadData **)vitasdk_get_pthread_data(threadHandle);

	unsigned int bits = 0;
	sceKernelPollEventFlag(pThreadData->evid, PTHREAD_EVID_CANCEL, SCE_EVENT_WAITAND, &bits);

	if (bits & PTHREAD_EVID_CANCEL)
		return PTE_OS_INTERRUPTED;

	return PTE_OS_OK;
}

void pte_osThreadSleep(unsigned int msecs)
{
	sceKernelDelayThread(msecs*1000);
}

int pte_osThreadGetMinPriority()
{
	return pte_osThreadGetDefaultPriority()-32;
}

int pte_osThreadGetMaxPriority()
{
	return pte_osThreadGetDefaultPriority()+31;
}

int pte_osThreadGetDefaultPriority()
{
	return 160;
}

/****************************************************************************
 *
 * Mutexes
 *
 ****************************************************************************/

pte_osResult pte_osMutexCreate(pte_osMutexHandle *pHandle)
{
	SceUID muid = sceKernelCreateMutex("", 0, 0, NULL);

	if (muid < 0)
		return PTE_OS_GENERAL_FAILURE;

	*pHandle = muid;
	return PTE_OS_OK;
}

pte_osResult pte_osMutexDelete(pte_osMutexHandle handle)
{
	sceKernelDeleteMutex(handle);
	return PTE_OS_OK;
}


pte_osResult pte_osMutexLock(pte_osMutexHandle handle)
{
	sceKernelLockMutex(handle, 1, NULL);
	return PTE_OS_OK;
}

pte_osResult pte_osMutexTimedLock(pte_osMutexHandle handle, unsigned int timeoutMsecs)
{
	unsigned int timeoutUsecs = timeoutMsecs*1000;
	int status = sceKernelLockMutex(handle, 1, &timeoutUsecs);

	if (status < 0)
	{
		if (status == SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			return PTE_OS_TIMEOUT;

		return PTE_OS_GENERAL_FAILURE;
	}

	return PTE_OS_OK;
}


pte_osResult pte_osMutexUnlock(pte_osMutexHandle handle)
{
	sceKernelUnlockMutex(handle, 1);
	return PTE_OS_OK;
}

/****************************************************************************
 *
 * Semaphores
 *
 ***************************************************************************/

pte_osResult pte_osSemaphoreCreate(int initialValue, pte_osSemaphoreHandle *pHandle)
{
	SceUID handle = sceKernelCreateSema("",
									   0,              /* attributes (default) */
									   initialValue,   /* initial value        */
									   SEM_VALUE_MAX,  /* maximum value        */
									   0);             /* options (default)    */

	if (handle < 0)
		return PTE_OS_GENERAL_FAILURE;

	*pHandle = handle;
	return PTE_OS_OK;
}

pte_osResult pte_osSemaphoreDelete(pte_osSemaphoreHandle handle)
{
	sceKernelDeleteSema(handle);
	return PTE_OS_OK;
}

pte_osResult pte_osSemaphorePost(pte_osSemaphoreHandle handle, int count)
{
	sceKernelSignalSema(handle, count);
	return PTE_OS_OK;
}

pte_osResult pte_osSemaphorePend(pte_osSemaphoreHandle handle, unsigned int *pTimeoutMsecs)
{
	SceUInt timeoutus = 0;
	SceUInt *timeout = NULL;

	if (pTimeoutMsecs)
	{
		timeoutus = *pTimeoutMsecs * 1000;
		timeout = &timeoutus;
	}

	int result = sceKernelWaitSema(handle, 1, timeout);

	if (result < 0)
	{
		if (result == SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			return PTE_OS_TIMEOUT;

		return PTE_OS_GENERAL_FAILURE;
	}

	return PTE_OS_OK;
}

/*
 * Pend on a semaphore- and allow the pend to be cancelled.
 *
 * PSP OS provides no functionality to asynchronously interrupt a blocked call.  We simulte
 * this by polling on the main semaphore and the cancellation semaphore and sleeping in a loop.
 */
pte_osResult pte_osSemaphoreCancellablePend(pte_osSemaphoreHandle semHandle, unsigned int *pTimeout)
{
	pspThreadData *pThreadData = *(pspThreadData **)vitasdk_get_pthread_data(0);
	SceUInt32 start = sceKernelGetProcessTimeLow();

	while (1)
	{
		unsigned int bits = 0;
		sceKernelPollEventFlag(pThreadData->evid, PTHREAD_EVID_CANCEL, SCE_EVENT_WAITAND, &bits);

		if (bits & PTHREAD_EVID_CANCEL)
			return PTE_OS_INTERRUPTED;


		// only poll the semaphore
		SceUInt semTimeout = 5*POLLING_DELAY_IN_us;
		int res = sceKernelWaitSema(semHandle, 1, &semTimeout);

		if (res < 0)
		{
			// TODO: associate error codes better
			if (res != SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			{
				return PTE_OS_GENERAL_FAILURE;
			}
		}
		else
		{
			break;
		}

		if (pTimeout)
		{
			if (sceKernelGetProcessTimeLow() - start > (*pTimeout) * 1000)
			{
				return PTE_OS_TIMEOUT;
			}
		}
	}

	return PTE_OS_OK;
}


/****************************************************************************
 *
 * Atomic Operations
 *
 ***************************************************************************/

int pte_osAtomicExchange(int *ptarg, int val)
{
	return atomic_exchange(ptarg, val);
}

int pte_osAtomicCompareExchange(int *pdest, int exchange, int comp)
{
	return __extension__ ({
		(void)(memory_order_seq_cst); (void)(memory_order_seq_cst);
		(__sync_val_compare_and_swap(pdest,
			comp, exchange));
	});
}

int pte_osAtomicExchangeAdd(int volatile* pAddend, int value)
{
	return atomic_fetch_add(pAddend, value);
}

int pte_osAtomicDecrement(int *pdest)
{
	return __sync_sub_and_fetch(pdest, 1);
}

int pte_osAtomicIncrement(int *pdest)
{
	return __sync_add_and_fetch(pdest, 1);
}

/****************************************************************************
 *
 * Thread Local Storage
 *
 ***************************************************************************/

pte_osResult pte_osTlsSetValue(unsigned int key, void * value)
{
	tlsKeyEntry *entry = *(tlsKeyEntry **)vitasdk_get_tls_data(0);
	tlsKeyEntry *prev = NULL;

	while (entry)
	{
		if (entry->key == key)
			break;

		prev = entry;
		entry = entry->next;
	}

	// no matching entry...
	if (!entry)
	{
		entry = (tlsKeyEntry *)malloc(sizeof(tlsKeyEntry));
		entry->key = key;
		entry->next = NULL;

		if (prev)
			prev->next = entry;
		else
			*(tlsKeyEntry **)vitasdk_get_tls_data(0) = entry;
	}

	entry->value = value;
	return PTE_OS_OK;
}

void * pte_osTlsGetValue(unsigned int index)
{
	tlsKeyEntry *entry = *(tlsKeyEntry **)vitasdk_get_tls_data(0);

	while (entry)
	{
		if (entry->key == index)
			break;

		entry = entry->next;
	}

	// no matching entry...
	if (!entry)
		return NULL;

	return entry->value;
}

pte_osResult pte_osTlsAlloc(unsigned int *pKey)
{
	unsigned int key = 0;

	// generate random key
	sceKernelGetRandomNumber(&key, 4);

	*pKey = key;
	return PTE_OS_OK;
}

pte_osResult pte_osTlsFree(unsigned int index)
{
	return PTE_OS_OK;
}

/****************************************************************************
 *
 * Miscellaneous
 *
 ***************************************************************************/

int ftime(struct timeb *tb)
{
  struct timeval tv;
  struct timezone tz;

  gettimeofday(&tv, &tz);

  tb->time = tv.tv_sec;
  tb->millitm = tv.tv_usec / 1000;
  tb->timezone = tz.tz_minuteswest;
  tb->dstflag = tz.tz_dsttime;

  return 0;
}
