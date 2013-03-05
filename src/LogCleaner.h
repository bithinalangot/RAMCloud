/* Copyright (c) 2009, 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef RAMCLOUD_LOGCLEANER_H
#define RAMCLOUD_LOGCLEANER_H

#include <thread>
#include <vector>

#include "Common.h"
#include "HashTable.h"
#include "Segment.h"
#include "LogCleanerMetrics.h"
#include "LogEntryHandlers.h"
#include "LogEntryRelocator.h"
#include "LogSegment.h"
#include "SegmentManager.h"
#include "ReplicaManager.h"

#include "LogMetrics.pb.h"

namespace RAMCloud {

class ServerConfig;

/**
 * The LogCleaner defragments a Log's closed segments, writing out any live
 * data to new "survivor" segments and reclaiming space used by dead log
 * entries. The cleaner runs in parallel with regular log operations in its
 * own thread.
 *
 * The cleaner employs some heuristics to aid efficiency. For instance, it
 * tries to minimise the cost of cleaning by choosing segments that have a
 * good 'cost-benefit' ratio. That is, it looks for segments that have lots
 * of free space, but also for segments that have less free space but a lot
 * of old data (the assumption being that old data is unlikely to die and
 * cleaning old data will reduce fragmentation and not soon require another
 * cleaning).
 *
 * In addition, the LogCleaner attempts to segregate entries by age in the
 * hopes of packing old data and new data into different segments. This has
 * two main benefits. First, old data is less likely to fragment (be freed)
 * so those segments will maintain high utilization and therefore require
 * less cleaning. Second, new data is more likely to fragment, so segments
 * containing newer data will hopefully be cheaper to clean in the future.
 */
class LogCleaner {
  public:
    LogCleaner(Context* context,
               const ServerConfig* config,
               SegmentManager& segmentManager,
               ReplicaManager& replicaManager,
               LogEntryHandlers& entryHandlers);
    ~LogCleaner();
    void start();
    void stop();
    void getMetrics(ProtoBuf::LogMetrics_CleanerMetrics& m);

  PRIVATE:
    typedef LogCleanerMetrics::MetricCycleCounter MetricCycleCounter;
    typedef std::lock_guard<SpinLock> Lock;

    /// If no cleaning work had to be done the last time we checked, sleep for
    /// this many microseconds before checking again.
    enum { POLL_USEC = 10000 };

    /// The maximum in-memory segment utilization we will clean at. This upper
    /// limit, in conjunction with the number of seglets per segment, ensures
    /// that we can never consume more seglets in cleaning than we free.
    enum { MAX_CLEANABLE_MEMORY_UTILIZATION = 98 };

    /// The maximum amount of live data we'll process in any single disk
    /// cleaning pass. The units are full segments. The cleaner will multiply
    /// this value by the number of bytes in a full segment and extract live
    /// entries from candidate segments until it exceeds that product.
    enum { MAX_LIVE_SEGMENTS_PER_DISK_PASS = 10 };

    /// The number of full survivor segments to reserve with the SegmentManager.
    /// Must be large enough to ensure that if we get the worst possible
    /// fragmentation during cleaning, we'll still have enough space to fit in
    /// MAX_LIVE_SEGMENTS_PER_DISK_PASS of live data before freeing unused
    /// seglets at the ends of survivor segments.
    ///
    /// TODO(Steve): This should probably just be dynamically computed using the
    /// segment size, maximum entry size, and MAX_LIVE_SEGMENTS_PER_DISK_PASS.
    enum { SURVIVOR_SEGMENTS_TO_RESERVE = 15 };

    /// The minimum amount of memory utilization we will begin cleaning at using
    /// the in-memory cleaner.
    enum { MIN_MEMORY_UTILIZATION = 90 };

    /// The minimum amount of backup disk utilization we will begin cleaning at
    /// using the disk cleaner. Note that the disk cleaner may also run if the
    /// in-memory cleaner is not working efficiently (there are tombstones that
    /// need to be made freeable by cleaning on disk).
    enum { MIN_DISK_UTILIZATION = 95 };

    /**
     * Tuple containing a reference to a live entry being cleaned, as well as a
     * cache of its timestamp. The purpose of this is to make sorting entries
     * by age much faster by caching the timestamp when we first examine the
     * entry in getSortedEntries(), rather than extracting it on each sort
     * comparison.
     */
    class LiveEntry {
      public:
        LiveEntry(LogSegment* segment, uint32_t offset, uint32_t timestamp)
            : segment(segment),
              offset(offset),
              timestamp(timestamp)
        {
            static_assert(sizeof(LiveEntry) == 16,
                "LiveEntry isn't the expected size!");
        }

        /// Pointer to the segment this entry is in.
        LogSegment* segment;

        /// Offset of the entry within the segment.
        uint32_t offset;

        /// Timestamp of the entry (see WallTime).
        uint32_t timestamp;
    } __attribute__((packed));
    typedef std::vector<LiveEntry> LiveEntryVector;

    /**
     * Comparison functor for sorting entries extracted from segments by their
     * timestamp. In sorting objects by age we can hopefully segregate objects
     * that will quickly decay and those that will last long into different
     * segments, which in turn makes cleaning more efficient.
     */
    class TimestampComparer {
      public:
        bool
        operator()(const LiveEntry& a, const LiveEntry& b)
        {
            return a.timestamp < b.timestamp;
        }
    };

    /**
     * Comparison functor used when sorting segments by best cost-benefit ratio.
     * This is used when choosing disk segments to clean. All candidates are
     * sorted by cost-benefit and then the list is walked in order.
     */
    class CostBenefitComparer {
      public:
        CostBenefitComparer();
        uint64_t costBenefit(LogSegment* s);
        bool operator()(LogSegment* a, LogSegment* b);

      PRIVATE:
        /// WallTime timestamp when this object was constructed.
        uint64_t now;

        /// Unique identifier for this comparer instance. The cost-benefit for a
        /// particular LogSegment must not change within a comparer's lifetime,
        /// otherwise weird things can happen, for example A < B, B < C, C < A).
        uint64_t version;
    };

    static void cleanerThreadEntry(LogCleaner* logCleaner, Context* context);
    void doWork();
    double doMemoryCleaning();
    void doDiskCleaning();
    LogSegment* getSegmentToCompact(uint32_t& outFreeableSeglets);
    void sortSegmentsByCostBenefit(LogSegmentVector& segments);
    void getSegmentsToClean(LogSegmentVector& outSegmentsToClean);
    void sortEntriesByTimestamp(LiveEntryVector& entries);
    void getSortedEntries(LogSegmentVector& segmentsToClean,
                          LiveEntryVector& outLiveEntries);
    uint64_t relocateLiveEntries(LiveEntryVector& liveEntries,
                                 LogSegmentVector& outSurvivors);
    void closeSurvivor(LogSegment* survivor);
    void waitForAvailableSurvivors(size_t count, uint64_t& outTicks);

    /**
     * Helper method that relocates a log entry and updates various metrics
     * to track relocation performance. It is templated so that it may be
     * used to track both relocations due to disk cleaning and in-memory
     * cleaning (compaction). 
     *
     * During cleaning, this is invoked on every entry in the segments being
     * cleaned. It is up to the LogEntryHandlers callee to decide whether or not
     * they want the entry anymore and to use the LogEntryRelocator this method
     * provides them to perpetuate the entry if they need it.
     *
     * \param type
     *      The type of entry that may need relocation.
     * \param buffer
     *      Buffer containing the segment entry that may need relocation.
     * \param survivor
     *      Survivor segment into which this entry may be relocated. This may
     *      be NULL, in which case the method will return false if relocation
     *      is attempted, or true if the entry was no longer needed and no
     *      relocation was tried.
     * \param metrics
     *      The appropriate metrics to update with relocation performance
     *      statistics. This is an instance of LogCleanerMetrics::InMemory or
     *      LogCleanerMetrics::OnDisk.
     * \return
     *      Returns true if the operation succeeded (the entry was successfully
     *      relocated or was not needed and no relocation was performed).
     *      Returns false if relocation was needed, but failed because the
     *      survivor segment was either NULL or had insufficient space. In this
     *      case, the caller will simply allocate a new survivor and retry.
     */
    template<typename T>
    bool
    relocateEntry(LogEntryType type,
                  Buffer& buffer,
                  LogSegment* survivor,
                  T& metrics)
    {
        LogEntryRelocator relocator(survivor, buffer.getTotalLength());

        {
            metrics.totalRelocationCallbacks++;
            MetricCycleCounter _(&metrics.relocationCallbackTicks);
            entryHandlers.relocate(type, buffer, relocator);
        }

        if (relocator.failed())
            return false;

        metrics.totalRelocationAppends++;
        metrics.relocationAppendTicks += relocator.getAppendTicks();
        return true;
    }

    /// Shared RAMCloud information.
    Context* context;

    /// The SegmentManager instance that we use to allocate survivor segments,
    /// report cleaned segments to, etc. This class owns all of the segments
    /// and seglets in the system.
    SegmentManager& segmentManager;

    /// The ReplicaManager instance that we use to store copies of log segments
    /// on remote backups. The cleaner needs this in order to replicate survivor
    /// segments generated during cleaning.
    ReplicaManager& replicaManager;

    /// EntryHandlers used to query information about entries we are cleaning
    /// (such as liveness), and to notify when an entry has been relocated.
    LogEntryHandlers& entryHandlers;

    /// Threshold defining how much work the in-memory cleaner should do before
    /// forcing a disk cleaning pass. Necessary because in-memory cleaning does
    /// not free up tombstones and can become very expensive before we run out
    /// of disk space and fire up the disk cleaner.
    double writeCostThreshold;

    /// If true, the in-memory cleaner will never be run. Instead, the disk
    /// cleaner will run in its place.
    bool disableInMemoryCleaning;

    /// The number of cleaner threads to run concurrently. More threads will
    /// allow the system to perform more cleaning and compaction in parallel to
    /// keep up with higher write rates and memory utilizations.
    const int numThreads;

    /// Closed log segments that are candidates for cleaning. Before each
    /// cleaning pass this list will be updated from the SegmentManager with
    /// newly closed segments. The most appropriate segments will then be
    /// cleaned. This list is shared across all cleaning threads and must only
    /// be accessed with the candidatesLock held.
    LogSegmentVector candidates;

    /// SpinLock protecting access to candidates. Needed because multiple
    /// cleaning threads may need to access it simultaneously.
    SpinLock candidatesLock;

    /// Size of each seglet in bytes. Used to calculate the best segment for in-
    /// memory cleaning.
    uint32_t segletSize;

    /// Size of each full segment in bytes. Used to calculate the amount of
    /// space freed on backup disks.
    uint32_t segmentSize;

    /// Number of cpu cycles spent in the doWork() routine.
    LogCleanerMetrics::Metric64BitType doWorkTicks;

    /// Number of cpu cycles spent sleeping in the doWork() routine because
    /// memory was not low.
    LogCleanerMetrics::Metric64BitType doWorkSleepTicks;

    /// Metrics kept for measuring in-memory cleaning (compaction) performance.
    LogCleanerMetrics::InMemory inMemoryMetrics;

    /// Metrics kept for measuring on-disk cleaning performance.
    LogCleanerMetrics::OnDisk onDiskMetrics;

    /// Metrics kept for measuring how many threads the cleaner is using.
    LogCleanerMetrics::Threads threadMetrics;

    /// Set by halt() to indicate that the cleaning thread(s) should exit.
    bool threadsShouldExit;

    /// The cleaner spins one or more threads to perform its work (#numThreads).
    /// This vector contains pointers to these threads. When the cleaner is
    /// started, these threads are created. When stopped, they are deleted and
    /// the pointers in this vector are set to NULL.
    vector<std::thread*> threads;

    DISALLOW_COPY_AND_ASSIGN(LogCleaner);
};

} // namespace

#endif // !RAMCLOUD_LOGCLEANER_H
