/***********************IMPORTANT NPCAP LICENSE TERMS***********************
 *                                                                         *
 * Npcap is a Windows packet sniffing driver and library and is copyright  *
 * (c) 2013-2020 by Insecure.Com LLC ("The Nmap Project").  All rights     *
 * reserved.                                                               *
 *                                                                         *
 * Even though Npcap source code is publicly available for review, it is   *
 * not open source software and may not be redistributed or incorporated   *
 * into other software without special permission from the Nmap Project.   *
 * We fund the Npcap project by selling a commercial license which allows  *
 * companies to redistribute Npcap with their products and also provides   *
 * for support, warranty, and indemnification rights.  For details on      *
 * obtaining such a license, please contact:                               *
 *                                                                         *
 * sales@nmap.com                                                          *
 *                                                                         *
 * Free and open source software producers are also welcome to contact us  *
 * for redistribution requests.  However, we normally recommend that such  *
 * authors instead ask your users to download and install Npcap            *
 * themselves.                                                             *
 *                                                                         *
 * Since the Npcap source code is available for download and review,       *
 * users sometimes contribute code patches to fix bugs or add new          *
 * features.  By sending these changes to the Nmap Project (including      *
 * through direct email or our mailing lists or submitting pull requests   *
 * through our source code repository), it is understood unless you        *
 * specify otherwise that you are offering the Nmap Project the            *
 * unlimited, non-exclusive right to reuse, modify, and relicence your     *
 * code contribution so that we may (but are not obligated to)             *
 * incorporate it into Npcap.  If you wish to specify special license      *
 * conditions or restrictions on your contributions, just say so when you  *
 * send them.                                                              *
 *                                                                         *
 * This software is distributed in the hope that it will be useful, but    *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                    *
 *                                                                         *
 * Other copyright notices and attribution may appear below this license   *
 * header. We have kept those for attribution purposes, but any license    *
 * terms granted by those notices apply only to their original work, and   *
 * not to any changes made by the Nmap Project or to this entire file.     *
 *                                                                         *
 * This header summarizes a few important aspects of the Npcap license,    *
 * but is not a substitute for the full Npcap license agreement, which is  *
 * in the LICENSE file included with Npcap and also available at           *
 * https://github.com/nmap/npcap/blob/master/LICENSE.                      *
 *                                                                         *
 ***************************************************************************/
#include "Packet.h"
#include "ObjPool.h"
#include <limits.h>

typedef struct _NPF_OBJ_SHELF
{
	SINGLE_LIST_ENTRY ShelfEntry;
	PNPF_OBJ_POOL pPool;
	ULONG ulUsed;
	SINGLE_LIST_ENTRY UnusedHead;
	UCHAR pBuffer[];
} NPF_OBJ_SHELF, *PNPF_OBJ_SHELF;

/* Objects in the pool are retrieved and returned using this struct.
 * pObject is an uninitialized array of ulObjectSize bytes.
 */
typedef struct _NPF_OBJ_POOL_ELEM
{
	PNPF_OBJ_SHELF pShelf;
	SINGLE_LIST_ENTRY UnusedEntry;
	ULONG Refcount;
	UCHAR pObject[];
} NPF_OBJ_POOL_ELEM, *PNPF_OBJ_POOL_ELEM;

typedef struct _NPF_OBJ_POOL
{
	SINGLE_LIST_ENTRY EmptyShelfHead;
	SINGLE_LIST_ENTRY PartialShelfHead;
	NDIS_SPIN_LOCK ShelfLock;
	NDIS_HANDLE NdisHandle;
	ULONG ulObjectSize;
	ULONG ulIncrement;
	PNPF_OBJ_INIT InitFunc;
	PNPF_OBJ_INIT CleanupFunc;
} NPF_OBJ_POOL;

#define NPF_OBJECT_POOL_TAG 'TPON'

#define NPF_OBJ_ELEM_ALLOC_SIZE(POOL) (sizeof(NPF_OBJ_POOL_ELEM) + (POOL)->ulObjectSize)
#define NPF_OBJ_SHELF_ALLOC_SIZE(POOL) ( sizeof(NPF_OBJ_SHELF) + NPF_OBJ_ELEM_ALLOC_SIZE(POOL) * (POOL)->ulIncrement)
#define NPF_OBJ_ELEM_MAX_IDX(POOL) (NPF_OBJ_SHELF_ALLOC_SIZE(pPool) - sizeof(NPF_OBJ_SHELF))

_Ret_maybenull_
PNPF_OBJ_SHELF
NPF_NewObjectShelf(
		_In_ PNPF_OBJ_POOL pPool)
{
	PNPF_OBJ_SHELF pShelf = NULL;
	PNPF_OBJ_POOL_ELEM pElem = NULL;
	ULONG i;

       	pShelf = (PNPF_OBJ_SHELF) NdisAllocateMemoryWithTagPriority(pPool->NdisHandle,
			NPF_OBJ_SHELF_ALLOC_SIZE(pPool),
		       	NPF_OBJECT_POOL_TAG,
			NormalPoolPriority);
	if (pShelf == NULL)
	{
		return NULL;
	}
	RtlZeroMemory(pShelf, NPF_OBJ_SHELF_ALLOC_SIZE(pPool));
	pShelf->pPool = pPool;

	// Buffer starts after the shelf itself
	for (i=0; i < pPool->ulIncrement; i++)
	{
		pElem = (PNPF_OBJ_POOL_ELEM) (pShelf->pBuffer + i * NPF_OBJ_ELEM_ALLOC_SIZE(pPool));
		pElem->pShelf= pShelf;
		PushEntryList(&pShelf->UnusedHead, &pElem->UnusedEntry);
	}

	return pShelf;
}

_Use_decl_annotations_
PNPF_OBJ_POOL NPF_AllocateObjectPool(
		NDIS_HANDLE NdisHandle,
		ULONG ulObjectSize,
		USHORT ulIncrement,
		PNPF_OBJ_INIT InitFunc,
		PNPF_OBJ_CLEANUP CleanupFunc)
{
	PNPF_OBJ_POOL pPool = NULL;

	pPool = NdisAllocateMemoryWithTagPriority(NdisHandle,
			sizeof(NPF_OBJ_POOL),
		       	NPF_OBJECT_POOL_TAG,
			NormalPoolPriority);
	if (pPool == NULL)
	{
		return NULL;
	}
	RtlZeroMemory(pPool, sizeof(NPF_OBJ_POOL));

	NdisAllocateSpinLock(&pPool->ShelfLock);

	pPool->NdisHandle = NdisHandle;
	pPool->ulObjectSize = ulObjectSize;
	pPool->ulIncrement = ulIncrement;
	pPool->InitFunc = InitFunc;
	pPool->CleanupFunc = CleanupFunc;

	return pPool;
}

_Use_decl_annotations_
PVOID NPF_ObjectPoolGet(PNPF_OBJ_POOL pPool,
		PNPF_OBJ_POOL_CTX Context)
{
	PNPF_OBJ_POOL_ELEM pElem = NULL;
	PSINGLE_LIST_ENTRY pEntry = NULL;
	PNPF_OBJ_SHELF pShelf = NULL;
	ASSERT(Context);
	if (Context == NULL)
	{
		return NULL;
	}

	FILTER_ACQUIRE_LOCK(&pPool->ShelfLock, Context->bAtDispatchLevel);
	// Get the first partial shelf
	pEntry = pPool->PartialShelfHead.Next;

	// If there are no partial shelves, get an empty one
	if (pEntry == NULL)
	{
		pEntry = PopEntryList(&pPool->EmptyShelfHead);
		// If there are no empty shelves, allocate a new one
		if (pEntry == NULL)
		{
			pShelf = NPF_NewObjectShelf(pPool);
			// If we couldn't allocate one, bail.
			if (pShelf == NULL)
			{
				FILTER_RELEASE_LOCK(&pPool->ShelfLock, Context->bAtDispatchLevel);
				return NULL;
			}
			pEntry = &pShelf->ShelfEntry;
		}
		// Now pEntry is an empty shelf. Move it to partials.
		PushEntryList(&pPool->PartialShelfHead, pEntry);
	}

	pShelf = CONTAINING_RECORD(pEntry, NPF_OBJ_SHELF, ShelfEntry);
	InterlockedIncrement(&pShelf->ulUsed);
	pEntry = PopEntryList(&pShelf->UnusedHead);
	if (pEntry == NULL)
	{
		// Should be impossible since all tracked shelves are partial or empty
		ASSERT(pEntry != NULL);
		FILTER_RELEASE_LOCK(&pPool->ShelfLock, Context->bAtDispatchLevel);
		return NULL;
	}
	pElem = CONTAINING_RECORD(pEntry, NPF_OBJ_POOL_ELEM, UnusedEntry);

	// If there aren't any more unused slots on this shelf, unlink it
	if (pShelf->UnusedHead.Next == NULL)
	{
		// We always use the first partial shelf on the stack, so pop it off.
		// No need to keep track of it; Return operation will re-link
		// it into partials.
		PopEntryList(&pPool->PartialShelfHead);
	}

	FILTER_RELEASE_LOCK(&pPool->ShelfLock, Context->bAtDispatchLevel);

	// We zero the memory when we first allocate it, and when an object is returned.
	// RtlZeroMemory(pElem->pObject, pPool->ulObjectSize);
#if DBG
	// Let's check that condition and make sure nothing is messing with returned objects
	// (SLOW! debug only)
	for (ULONG i=0; i < pPool->ulObjectSize; i++)
	{
		ASSERT(((PUCHAR)pElem->pObject)[i] == 0);
	}
#endif
	if (pPool->InitFunc)
	{
		pPool->InitFunc(pElem->pObject, Context);
	}

	ASSERT(pElem->Refcount == 0);
	pElem->Refcount = 1;
	return pElem->pObject;
}

_Use_decl_annotations_
VOID NPF_FreeObjectPool(PNPF_OBJ_POOL pPool)
{
	PSINGLE_LIST_ENTRY pShelfEntry = NULL;

	FILTER_ACQUIRE_LOCK(&pPool->ShelfLock, NPF_IRQL_UNKNOWN);

	while ((pShelfEntry = PopEntryList(&pPool->PartialShelfHead)) != NULL)
	{
		NdisFreeMemory(
				CONTAINING_RECORD(pShelfEntry, NPF_OBJ_SHELF, ShelfEntry),
				NPF_OBJ_SHELF_ALLOC_SIZE(pPool),
				0);
	}
	while ((pShelfEntry = PopEntryList(&pPool->EmptyShelfHead)) != NULL)
	{
		NdisFreeMemory(
				CONTAINING_RECORD(pShelfEntry, NPF_OBJ_SHELF, ShelfEntry),
				NPF_OBJ_SHELF_ALLOC_SIZE(pPool),
				0);
	}
	FILTER_RELEASE_LOCK(&pPool->ShelfLock, NPF_IRQL_UNKNOWN);

	NdisFreeSpinLock(&pPool->ShelfLock);
	NdisFreeMemory(pPool, sizeof(NPF_OBJ_POOL), 0);
}

_Use_decl_annotations_
VOID NPF_ShrinkObjectPool(PNPF_OBJ_POOL pPool)
{
	PSINGLE_LIST_ENTRY pShelfEntry = NULL;
	PSINGLE_LIST_ENTRY pEmptyNext = NULL;
	ULONG TotalUnused = 0;
	BOOLEAN bKeepOne = TRUE;

	if (pPool->EmptyShelfHead.Next == NULL)
	{
		// No empty shelves to free
		return;
	}

	FILTER_ACQUIRE_LOCK(&pPool->ShelfLock, NPF_IRQL_UNKNOWN);

	for (pShelfEntry = pPool->PartialShelfHead.Next; pShelfEntry != NULL; pShelfEntry = pShelfEntry->Next)
	{
		TotalUnused += pPool->ulIncrement - CONTAINING_RECORD(pShelfEntry, NPF_OBJ_SHELF, ShelfEntry)->ulUsed;
		if (TotalUnused >= pPool->ulIncrement)
		{
			// There's at least 1 shelf's worth of unused space
			bKeepOne = FALSE;
			break;
		}
	}

	// While there are empty shelves available
	while (pPool->EmptyShelfHead.Next != NULL
			// and we either don't need to keep one or there's one more after this one,
			&& (!bKeepOne || pPool->EmptyShelfHead.Next->Next != NULL))
	{
		// Pop one off and free it.
		pShelfEntry = PopEntryList(&pPool->EmptyShelfHead);
		ASSERT(pShelfEntry);
		if (!pShelfEntry) {
			// Shouldn't happen because of the loop condition, but Code Analysis complains.
			break;
		}
		NdisFreeMemory(
				CONTAINING_RECORD(pShelfEntry, NPF_OBJ_SHELF, ShelfEntry),
				NPF_OBJ_SHELF_ALLOC_SIZE(pPool),
				0);
	}

	FILTER_RELEASE_LOCK(&pPool->ShelfLock, NPF_IRQL_UNKNOWN);
}

_Use_decl_annotations_
VOID NPF_ObjectPoolReturn(
		PVOID pObject,
		PNPF_OBJ_POOL_CTX Context)
{
	NPF_OBJ_CALLBACK_STATUS Status = NPF_OBJ_STATUS_SUCCESS;
	PNPF_OBJ_SHELF pShelf = NULL;
	PNPF_OBJ_POOL pPool = NULL;
	PNPF_OBJ_POOL_ELEM pElem = CONTAINING_RECORD(pObject, NPF_OBJ_POOL_ELEM, pObject);
	ULONG refcount = InterlockedDecrement(&pElem->Refcount);
	NPF_OBJ_POOL_CTX unknown_context;
	ASSERT(Context);
	if (Context == NULL)
	{
		unknown_context.bAtDispatchLevel = FALSE;
		unknown_context.pContext = NULL;
		Context = &unknown_context;
	}
	ASSERT(refcount < ULONG_MAX);

	if (refcount == 0)
	{
		pShelf = pElem->pShelf;
		pPool = pShelf->pPool;
		if (pPool->CleanupFunc)
		{
			Status = pPool->CleanupFunc(pElem->pObject, Context);
			switch (Status)
			{
				case NPF_OBJ_STATUS_SAVED:
					// Callback refcounted the object and put it elsewhere.
					// Can't assert anything because it could be
					// being returned again somewhere.
					return;
				case NPF_OBJ_STATUS_RESOURCES:
					// TODO: Safely recover from this condition.
					// 1. Have NPF_ObjectPoolReturn tell its caller that something went wrong so it can bail.
					// 2. Introduce a way to safely shut down the driver if we end up in an unrecoverable state.
					ASSERT(NULL);
				default:
					break;
			}
		}

		// Zero this now to ensure unused objects are zeroed when retrieved
		// Doing it this way helps spot bugs by invalidating pointers in the old object
		RtlZeroMemory(pElem->pObject, pPool->ulObjectSize);

		FILTER_ACQUIRE_LOCK(&pPool->ShelfLock, Context->bAtDispatchLevel);

		refcount = InterlockedDecrement(&pShelf->ulUsed);
		ASSERT(refcount < pPool->ulIncrement);
		if (refcount == 0)
		{
			// Empty shelf. Move it to the other list.
			PSINGLE_LIST_ENTRY pEntry = pPool->PartialShelfHead.Next;
			PSINGLE_LIST_ENTRY pPrev = &pPool->PartialShelfHead;
			while (pEntry)
			{
				if (pEntry == &pShelf->ShelfEntry)
				{
					// Found it. Unlink and stop looking.
					pPrev->Next = pEntry->Next;
					pEntry->Next = NULL;
					break;
				}
				pPrev = pEntry;
				pEntry = pPrev->Next;
			}
			if (pEntry == NULL)
			{
				ASSERT(pEntry != NULL);
				FILTER_RELEASE_LOCK(&pPool->ShelfLock, Context->bAtDispatchLevel);
				return;
			}

			PushEntryList(&pPool->EmptyShelfHead, &pShelf->ShelfEntry);
		}
		else if (refcount == pPool->ulIncrement - 1)
		{
			// This shelf was full and now it's partial. Link it in.
			PushEntryList(&pPool->PartialShelfHead, &pShelf->ShelfEntry);
		}

		PushEntryList(&pShelf->UnusedHead, &pElem->UnusedEntry);

		FILTER_RELEASE_LOCK(&pPool->ShelfLock, Context->bAtDispatchLevel);
	}
}

_Use_decl_annotations_
VOID NPF_ReferenceObject(PVOID pObject)
{
	PNPF_OBJ_POOL_ELEM pElem = CONTAINING_RECORD(pObject, NPF_OBJ_POOL_ELEM, pObject);

	InterlockedIncrement(&pElem->Refcount);
	// If we get this many, we have an obvious bug.
	ASSERT(pElem->Refcount < ULONG_MAX);
}
