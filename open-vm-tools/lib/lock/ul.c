/*********************************************************
 * Copyright (C) 2009-2019 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#include "vmware.h"
#include "str.h"
#include "util.h"
#include "userlock.h"
#include "ulInt.h"
#include "ulIntShared.h"
#include "hashTable.h"
#include "random.h"

static Bool mxInPanic = FALSE;  // track when involved in a panic
static Bool mxUserCollectLockingTree = FALSE;

Bool (*MXUserTryAcquireForceFail)() = NULL;

static MX_Rank (*MXUserMxCheckRank)(void) = NULL;
static void (*MXUserMxLockLister)(void) = NULL;
void (*MXUserMX_LockRec)(struct MX_MutexRec *lock) = NULL;
void (*MXUserMX_UnlockRec)(struct MX_MutexRec *lock) = NULL;
Bool (*MXUserMX_TryLockRec)(struct MX_MutexRec *lock) = NULL;
Bool (*MXUserMX_IsLockedByCurThreadRec)(const struct MX_MutexRec *lock) = NULL;
char *(*MXUserMX_NameRec)(const struct MX_MutexRec *lock) = NULL;
static void (*MXUserMX_SetInPanic)(void) = NULL;
static Bool (*MXUserMX_InPanic)(void) = NULL;

#define	MXUSER_MAX_LOOP 5


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_IsLockingTreeAvailable
 *
 *      Is the lock tracking tree available for reporting?
 *
 * Results:
 *      TRUE  MXuser lock tree tracking is enabled
 *      FALSE MXUser lock tree tracking is disabled
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
MXUser_IsLockingTreeAvailable(void)
{
   return mxUserCollectLockingTree;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_LockingTreeCollection
 *
 *      Enable or disable locking tree data collection.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_LockingTreeCollection(Bool enabled)  // IN:
{
   mxUserCollectLockingTree = vmx86_devel && vmx86_debug && enabled;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserInternalSingleton --
 *
 *      A "singleton" function for the MXUser internal recursive lock.
 *
 *      Internal MXUser recursive locks have no statistics gathering or
 *      tracking abilities. They need to used with care and rarely.
 *
 * Results:
 *      NULL    Failure
 *      !NULL   A pointer to an initialized MXRecLock
 *
 * Side effects:
 *      Manifold.
 *
 *-----------------------------------------------------------------------------
 */

MXRecLock *
MXUserInternalSingleton(Atomic_Ptr *storage)  // IN:
{
   MXRecLock *lock = Atomic_ReadPtr(storage);

   if (UNLIKELY(lock == NULL)) {
      MXRecLock *newLock = Util_SafeMalloc(sizeof *newLock);

      if (MXRecLockInit(newLock)) {
         lock = Atomic_ReadIfEqualWritePtr(storage, NULL, (void *) newLock);

         if (lock) {
            MXRecLockDestroy(newLock);
            free(newLock);
         } else {
            lock = Atomic_ReadPtr(storage);
         }
      } else {
         free(newLock);
         lock = Atomic_ReadPtr(storage);  // maybe another thread succeeded
      }
   }

   return lock;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserSydrome --
 *
 *      Generate the syndrome bits for this MXUser library.
 *
 *      Each MXUser library has unique syndrome bits enabling the run time
 *      detection of locks created with one copy of the MXUser library and
 *      passed to another copy of the MXUser library.
 *
 *      The syndrome bits provide a detection mechanism for locks created and
 *      maintained by one copy of the lock library being used by another copy
 *      of the lock library. Undetected, strange crashes can occur due to locks
 *      that are aliased or are incompatible with the lock library that is
 *      manipulating them.
 *
 *      The syndrome bits are generated by using a source of bits that is
 *      external to a program and its libraries. This way no code or data
 *      based scheme can be spoofed or aliased.
 *
 * Results:
 *      As above
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static uint32
MXUserSyndrome(void)
{
   uint32 syndrome;
   static Atomic_uint32 syndromeMem;  // implicitly zero -- mbellon

   syndrome = Atomic_Read(&syndromeMem);

   if (syndrome == 0) {
#if defined(_WIN32)
#if defined(VM_WIN_UWP)
      syndrome = (uint32)GetTickCount64();
#else
      syndrome = GetTickCount();
#endif
#else
      syndrome = time(NULL) & 0xFFFFFFFF;
#endif

      /*
       * Protect against a total failure.
       */

      if (syndrome == 0) {
         syndrome++;
      }

      /* blind write; if racing one thread or the other will do */
      Atomic_ReadIfEqualWrite(&syndromeMem, 0, syndrome);

      syndrome = Atomic_Read(&syndromeMem);
   }

   ASSERT(syndrome);

   return syndrome;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserGetSignature --
 *
 *      Return a signature appropriate for the specified object type.
 *
 * Results:
 *      As above
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

uint32
MXUserGetSignature(MXUserObjectType objectType)  // IN:
{
   uint32 signature;

   ASSERT((objectType >= 0) && (objectType < 16) &&
          (objectType != MXUSER_TYPE_NEVER_USE));

   /*
    * Create a unique signature by combining the unique syndrome of this
    * instance of lib/lock with a mapping of objectType. The unique syndome
    * within the signature is used to catch multiple copies of lib/lock that
    * are "leaking" locks between them (e.g. locks may be aliased (which means
    * no protection) or internal implementation details may have changed).
    */

   signature = (MXUserSyndrome() & 0x0FFFFFFF) | (objectType << 28);

   ASSERT(signature);

   return signature;
}


/*
 *---------------------------------------------------------------------
 *
 *  MXUser_SetInPanic --
 *
 *     Notify the locking system that a panic is occurring.
 *
 *  Results:
 *     Set the "in a panic" state in userland locks and, when possible,
 *     MX locks.
 *
 *  Side effects:
 *     None
 *
 *---------------------------------------------------------------------
 */

void
MXUser_SetInPanic(void)
{
   mxInPanic = TRUE;

   if (MXUserMX_SetInPanic != NULL) {
      MXUserMX_SetInPanic();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserDumpAndPanic --
 *
 *      Dump a lock, print a message and die
 *
 * Results:
 *      A panic.
 *
 * Side effects:
 *      Manifold.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserDumpAndPanic(MXUserHeader *header,  // IN:
                   const char *fmt,       // IN:
                   ...)                   // IN:
{
   char *msg;
   va_list ap;
   static uint32 loopCounter = 0;  // Is panic looping through there?

   ASSERT((header != NULL) && (header->dumpFunc != NULL));

   if (++loopCounter > MXUSER_MAX_LOOP) {
      /*
       * Panic is looping through MXUser to here - no progress is being made.
       * Switch to panic mode in the hopes that this will allow some progress.
       */

      MXUser_SetInPanic();
   }

   (*header->dumpFunc)(header);

   va_start(ap, fmt);
   msg = Str_SafeVasprintf(NULL, fmt, ap);
   va_end(ap);

   Panic("%s", msg);
}


/*
 *---------------------------------------------------------------------
 *
 *  MXUser_InPanic --
 *
 *     Is the caller in the midst of a panic?
 *
 *  Results:
 *     TRUE   Yes
 *     FALSE  No
 *
 *  Side effects:
 *     None
 *
 *---------------------------------------------------------------------
 */

Bool
MXUser_InPanic(void)
{
   return mxInPanic || (MXUserMX_InPanic != NULL && MXUserMX_InPanic());
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserInstallMxHooks --
 *
 *      The MX facility may notify the MXUser facility that it is place and
 *      that MXUser should check with it. This function should be called from
 *      MX_Init.
 *
 * Results:
 *      As Above.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserInstallMxHooks(void (*theLockListFunc)(void),
                     MX_Rank (*theRankFunc)(void),
                     void  (*theLockFunc)(struct MX_MutexRec *lock),
                     void  (*theUnlockFunc)(struct MX_MutexRec *lock),
                     Bool  (*theTryLockFunc)(struct MX_MutexRec *lock),
                     Bool  (*theIsLockedFunc)(const struct MX_MutexRec *lock),
                     char *(*theNameFunc)(const struct MX_MutexRec *lock),
                     void  (*theSetInPanicFunc)(void),
                     Bool  (*theInPanicFunc)(void))
{
   /*
    * This function can be called more than once but the second and later
    * invocations must be attempting to install the same hook functions as
    * the first invocation.
    */

   if ((MXUserMxLockLister == NULL) &&
       (MXUserMxCheckRank == NULL) &&
       (MXUserMX_LockRec == NULL) &&
       (MXUserMX_UnlockRec == NULL) &&
       (MXUserMX_TryLockRec == NULL) &&
       (MXUserMX_IsLockedByCurThreadRec == NULL) &&
       (MXUserMX_NameRec == NULL) &&
       (MXUserMX_SetInPanic == NULL) &&
       (MXUserMX_InPanic == NULL)
       ) {
      MXUserMxLockLister = theLockListFunc;
      MXUserMxCheckRank = theRankFunc;
      MXUserMX_LockRec = theLockFunc;
      MXUserMX_UnlockRec = theUnlockFunc;
      MXUserMX_TryLockRec = theTryLockFunc;
      MXUserMX_IsLockedByCurThreadRec = theIsLockedFunc;
      MXUserMX_NameRec = theNameFunc;
      MXUserMX_SetInPanic = theSetInPanicFunc;
      MXUserMX_InPanic = theInPanicFunc;
   } else {
      ASSERT((MXUserMxLockLister == theLockListFunc) &&
             (MXUserMxCheckRank == theRankFunc) &&
             (MXUserMX_LockRec == theLockFunc) &&
             (MXUserMX_UnlockRec == theUnlockFunc) &&
             (MXUserMX_TryLockRec == theTryLockFunc) &&
             (MXUserMX_IsLockedByCurThreadRec == theIsLockedFunc) &&
             (MXUserMX_NameRec == theNameFunc) &&
             (MXUserMX_SetInPanic == theSetInPanicFunc) &&
             (MXUserMX_InPanic == theInPanicFunc)
            );
   }
}

#if defined(MXUSER_DEBUG)
#define MXUSER_MAX_LOCKS_PER_THREAD (2 * MXUSER_MAX_REC_DEPTH)

typedef struct MXUserPerThread {
   struct MXUserPerThread  *next;
   uint32                   locksHeld;
   MXUserHeader            *lockArray[MXUSER_MAX_LOCKS_PER_THREAD];
} MXUserPerThread;

static Atomic_Ptr perThreadLockMem;
static MXUserPerThread *perThreadFreeList = NULL;

static Atomic_Ptr hashTableMem;

/*
 *-----------------------------------------------------------------------------
 *
 * MXUserAllocPerThread --
 *
 *     Allocate a perThread structure.
 *
 *     Memory is allocated for the specified thread as necessary. Use a
 *     victim cache in front of malloc to provide a slight performance
 *     advantage. The lock here is equivalent to the lock buried inside
 *     malloc but no complex calculations are necessary to perform an
 *     allocation most of the time.
 *
 *     The maximum size of the list will be roughly the maximum number of
 *     threads having taken locks at the same time - a bounded number less
 *     than or equal to the maximum of threads created.
 *
 * Results:
 *     As above.
 *
 * Side effects:
 *      Memory may be allocated.
 *
 *-----------------------------------------------------------------------------
 */

static MXUserPerThread *
MXUserAllocPerThread(void)
{
   MXUserPerThread *perThread;
   MXRecLock *perThreadLock = MXUserInternalSingleton(&perThreadLockMem);

   ASSERT(perThreadLock);

   MXRecLockAcquire(perThreadLock,
                    NULL);          // non-stats

   if (perThreadFreeList == NULL) {
      perThread = Util_SafeMalloc(sizeof *perThread);
   } else {
      perThread = perThreadFreeList;
      perThreadFreeList = perThread->next;
   }

   MXRecLockRelease(perThreadLock);

   ASSERT(perThread);

   memset(perThread, 0, sizeof *perThread);  // ensure all zeros

   return perThread;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserFreePerThread --
 *
 *     Free a perThread structure.
 *
 *     The structure is placed on the free list -- for "later".
 *
 * Results:
 *     As above.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
MXUserFreePerThread(MXUserPerThread *perThread)  // IN:
{
   MXRecLock *perThreadLock;

   ASSERT(perThread);
   ASSERT(perThread->next == NULL);

   perThreadLock = MXUserInternalSingleton(&perThreadLockMem);
   ASSERT(perThreadLock);

   MXRecLockAcquire(perThreadLock,
                    NULL);          // non-stats

   perThread->next = perThreadFreeList;
   perThreadFreeList = perThread;

   MXRecLockRelease(perThreadLock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserGetPerThread --
 *
 *      Return a pointer to the per thread data for the specified thread.
 *
 *      Memory is allocated for the specified thread as necessary. This memory
 *      is never released since it it is highly likely a thread will use a
 *      lock and need to record data in the perThread.
 *
 * Results:
 *      NULL   mayAlloc was FALSE and the thread doesn't have a perThread
 *     !NULL   the perThread of the specified thread
 *
 * Side effects:
 *      Memory may be allocated.
 *
 *-----------------------------------------------------------------------------
 */

static MXUserPerThread *
MXUserGetPerThread(Bool mayAlloc)  // IN: alloc perThread if not present?
{
   HashTable *hash;
   MXUserPerThread *perThread = NULL;
   void *tid = MXUserCastedThreadID();

   hash = HashTable_AllocOnce(&hashTableMem, 1024,
                              HASH_INT_KEY | HASH_FLAG_ATOMIC, NULL);

   if (!HashTable_Lookup(hash, tid, (void **) &perThread)) {
      /* No entry for this tid was found, allocate one? */

      if (mayAlloc) {
         MXUserPerThread *newEntry = MXUserAllocPerThread();

         /*
          * Attempt to (racey) insert a perThread on behalf of the specified
          * thread. If yet another thread takes care of this first, clean up
          * the mess.
          */

         perThread = HashTable_LookupOrInsert(hash, tid, newEntry);
         ASSERT(perThread);

         if (perThread != newEntry) {
            MXUserFreePerThread(newEntry);
         }
      } else {
         perThread = NULL;
      }
   }

   return perThread;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserListLocks
 *
 *      Allow a caller to list, via warnings, the list of locks the caller
 *      has acquired. Ensure that no memory for lock tracking is allocated
 *      if no locks have been taken.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The list is printed.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserListLocks(void)
{
   MXUserPerThread *perThread = MXUserGetPerThread(FALSE);

   if (perThread != NULL) {
      uint32 i;

      for (i = 0; i < perThread->locksHeld; i++) {
         MXUserHeader *hdr = perThread->lockArray[i];

         Warning("\tMXUser lock %s (@0x%p) rank 0x%x\n", hdr->name, hdr,
                 hdr->rank);
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_IsCurThreadHoldingLocks --
 *
 *      Are any MXUser locks held by the calling thread?
 *
 * Results:
 *      TRUE   Yes
 *      FALSE  No
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
MXUser_IsCurThreadHoldingLocks(void)
{
   MXUserPerThread *perThread = MXUserGetPerThread(FALSE);

   return (perThread == NULL) ? FALSE : (perThread->locksHeld != 0);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserThreadRank --
 *
 *      Return the highest rank held by the specified thread via MXUser locks.
 *
 * Results:
 *      As above
 *
 * Side effects:
 *      Can optionally determine if a lock has been locked before.
 *
 *-----------------------------------------------------------------------------
 */

static MX_Rank
MXUserThreadRank(MXUserPerThread *perThread,  // IN:
                 MXUserHeader *header,        // IN:
                 Bool *firstUse)              // OUT:
{
   uint32 i;
   Bool foundOnce = TRUE;
   MX_Rank maxRank = RANK_UNRANKED;

   ASSERT(perThread);

   /*
    * Determine the maximum rank held. Note if the lock being acquired
    * was previously entered into the tracking system.
    */

   for (i = 0; i < perThread->locksHeld; i++) {
      MXUserHeader *chkHdr = perThread->lockArray[i];

      maxRank = MAX(chkHdr->rank, maxRank);

      if (chkHdr == header) {
         foundOnce = FALSE;
      }
   }

   if (firstUse) {
      *firstUse = foundOnce;
   }

   return maxRank;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserCurrentRank --
 *
 *      Return the highest rank held by the current thread via MXUser locks.
 *
 * Results:
 *      As above
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

MX_Rank
MXUserCurrentRank(void)
{
   MX_Rank maxRank;
   MXUserPerThread *perThread = MXUserGetPerThread(FALSE);

   if (perThread == NULL) {
      maxRank = RANK_UNRANKED;
   } else {
      maxRank = MXUserThreadRank(perThread, NULL, NULL);
   }

   return maxRank;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserAcquisitionTracking --
 *
 *      Perform the appropriate tracking for lock acquisition.
 *
 * Results:
 *      Panic when a rank violation is detected (checkRank is TRUE).
 *      Add a lock instance to perThread lock list.
 *
 * Side effects:
 *      Manifold.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserAcquisitionTracking(MXUserHeader *header,  // IN:
                          Bool checkRank)        // IN:
{
   MXUserPerThread *perThread = MXUserGetPerThread(TRUE);

   VERIFY(perThread->locksHeld < MXUSER_MAX_LOCKS_PER_THREAD);

   /*
    * Rank checking anyone?
    *
    * Rank checking is abandoned once we're in a panic situation. This will
    * improve the chances of obtaining a good log and/or coredump.
    */

   if (checkRank && (header->rank != RANK_UNRANKED) && !MXUser_InPanic()) {
      MX_Rank maxRank;
      Bool firstInstance = TRUE;

      /*
       * Determine the highest rank held by the calling thread. Check for
       * MX locks if they are present.
       */

      maxRank = MXUserThreadRank(perThread, header, &firstInstance);

      if (MXUserMxCheckRank) {
         maxRank = MAX(maxRank, (*MXUserMxCheckRank)());
      }

      /*
       * Perform rank checking when a lock is entered into the tracking
       * system for the first time. This works out well because:
       *
       * Recursive locks are rank checked only upon their first acquisition...
       * just like MX locks.
       *
       * Exclusive locks will have a second entry added into the tracking
       * system but will immediately panic due to the run time checking - no
       * (real) harm done.
       */

      if (firstInstance && (header->rank <= maxRank)) {
         Warning("%s: lock rank violation by thread %s\n", __FUNCTION__,
                 VThread_CurName());
         Warning("%s: locks held:\n", __FUNCTION__);

         if (MXUserMxLockLister) {
            (*MXUserMxLockLister)();
         }

         MXUserListLocks();

         MXUserDumpAndPanic(header, "%s: rank violation maxRank=0x%x\n",
                            __FUNCTION__, maxRank);
      }
   }

   /* Add a lock instance to the calling threads perThread information */
   perThread->lockArray[perThread->locksHeld++] = header;

   /*
    * Maintain the lock tracking tree when approporiate.
    */

   if (vmx86_devel && vmx86_debug && mxUserCollectLockingTree) {
      uint32 i;
      MXUserLockTreeNode *node = NULL;

      MXUserLockTreeAcquire();

      for (i = 0; i < perThread->locksHeld; i++) {
         header = perThread->lockArray[i];

         node = MXUserLockTreeAdd(node, header->name,
                                  header->serialNumber, header->rank);
      }

      MXUserLockTreeRelease();
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserReleaseTracking --
 *
 *      Perform the appropriate tracking for lock release.
 *
 * Results:
 *      A panic.
 *
 * Side effects:
 *      Manifold.
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserReleaseTracking(MXUserHeader *header)  // IN: lock, via its header
{
   uint32 i;
   uint32 lastEntry;
   MXUserPerThread *perThread = MXUserGetPerThread(FALSE);

   /* MXUserAcquisitionTracking should have already created a perThread */
   if (UNLIKELY(perThread == NULL)) {
      MXUserDumpAndPanic(header, "%s: perThread not found! (thread 0x%p)\n",
                         __FUNCTION__, MXUserCastedThreadID());
   }

   /* Search the perThread for the argument lock */
   for (i = 0; i < perThread->locksHeld; i++) {
      if (perThread->lockArray[i] == header) {
         break;
      }
   }

   /* The argument lock had better be in the perThread */
   if (UNLIKELY(i >= perThread->locksHeld)) {
      MXUserDumpAndPanic(header,
                         "%s: lock not found! (thread 0x%p; count %u)\n",
                         __FUNCTION__, MXUserCastedThreadID(),
                         perThread->locksHeld);
   }

   /* Remove the argument lock from the perThread */
   lastEntry = perThread->locksHeld - 1;

   if (i < lastEntry) {
      uint32 j;

      for (j = i + 1; j < perThread->locksHeld; j++) {
         perThread->lockArray[i++] = perThread->lockArray[j];
      }
   }

   perThread->lockArray[lastEntry] = NULL;  // tidy up memory
   perThread->locksHeld--;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUser_TryAcquireFailureControl --
 *
 *      Should a TryAcquire operation fail, no matter "what", sometimes?
 *
 *      Failures occur statistically in debug builds to force our code down
 *      all of its paths.
 *
 * Results:
 *      Unknown
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUser_TryAcquireFailureControl(Bool (*func)(const char *name))  // IN:
{
   MXUserTryAcquireForceFail = func;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MXUserValidateHeader --
 *
 *      Validate an MXUser object header.
 *
 * Results:
 *      Return  All is well
 *      Panic   All is NOT well
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
MXUserValidateHeader(MXUserHeader *header,         // IN:
                     MXUserObjectType objectType)  // IN:
{
   uint32 expected = MXUserGetSignature(objectType);

   if (header->badHeader) {
      return; // No need to panic on a bad header repeatedly...
   }

   if (header->signature != expected) {
      header->badHeader = TRUE;

      MXUserDumpAndPanic(header,
                        "%s: signature failure! expected 0x%X observed 0x%X\n",
                         __FUNCTION__, expected, header->signature);

   }

   if (header->serialNumber == 0) {
      header->badHeader = TRUE;

      MXUserDumpAndPanic(header, "%s: Invalid serial number!\n",
                         __FUNCTION__);
   }
}
#endif
