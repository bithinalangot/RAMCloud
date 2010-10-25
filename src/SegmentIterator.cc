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

// RAMCloud pragma [GCCWARN=5]
// RAMCloud pragma [CPPLINT=0]

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Segment.h>
#include <SegmentIterator.h>
#include <LogTypes.h>

namespace RAMCloud {

/**
 * Construct a new SegmentIterator for the given Segment object.
 * \param[in] segment
 *      The Segment object to be iterated over.
 * \return
 *      The newly constructed SegmentIterator object.
 */
SegmentIterator::SegmentIterator(const Segment *segment)
    : baseAddress(segment->getBaseAddress()),
      segmentCapacity(segment->getCapacity()),
      id(segment->getId()),
      type(LOG_ENTRY_TYPE_INVALID),
      length(0),
      blobPtr(NULL),
      sawFooter(false),
      firstEntry(NULL),
      currentEntry(NULL)
{
    CommonConstructor();
}

/**
 * Construct a new SegmentIterator for a piece of memory that was or is used
 * as the backing for a Segment object.
 * \param[in] buffer
 *      A pointer to the first byte of the Segment backing memory.
 * \param[in] length
 *      The length of the segment in bytes.
 */
SegmentIterator::SegmentIterator(const void *buffer, uint64_t length)
    : baseAddress(buffer),
      segmentCapacity(length),
      id(-1),
      type(LOG_ENTRY_TYPE_INVALID),
      length(0),
      blobPtr(NULL),
      sawFooter(false),
      firstEntry(NULL),
      currentEntry(NULL)
{
    CommonConstructor();
}

/**
 * Perform initialisation operations common to all constructors. This
 * includes sanity checking and setting up the first iteration's state.
 */
void
SegmentIterator::CommonConstructor()
{
    if (segmentCapacity < (sizeof(SegmentEntry) + sizeof(SegmentHeader)))
        throw 0;

    const SegmentEntry *entry = (const SegmentEntry *)baseAddress;
    if (entry->type   != LOG_ENTRY_TYPE_SEGHEADER ||
        entry->length != sizeof(SegmentHeader) ||
        !isEntryValid(entry)) {
        throw 0;
    }

    const SegmentHeader *header = (const SegmentHeader *)((char *)baseAddress +
        sizeof(SegmentEntry));
    if (header->segmentCapacity != segmentCapacity)
        throw 0;

    type    = entry->type;
    length  = entry->length;
    blobPtr = (char *)baseAddress + sizeof(*entry);

    currentEntry = firstEntry = entry;
}

/**
 * Determine if the SegmentEntry provided is valid, i.e. that the SegmentEntry
 * does not overrun or underrun the buffer.
 * \param[in] entry
 *      The entry to validate.
 * \return
 *      true if the entry is valid, false otherwise.
 */
bool
SegmentIterator::isEntryValid(const SegmentEntry *entry) const
{
    uintptr_t lastByte      = (uintptr_t)baseAddress + segmentCapacity - 1;
    uintptr_t entryStart    = (uintptr_t)entry;
    uintptr_t entryLastByte = entryStart + entry->length + sizeof(*entry) - 1;

    // this is an internal error
    if (entryStart < (uintptr_t)baseAddress)
        throw 0;

    if (entryLastByte > lastByte)
        return false;

    return true;
}

/**
 * Test if the SegmentIterator has exhausted all entries.
 * \return
 *      true if there are no more entries left to iterate, else false.
 */
bool
SegmentIterator::isDone() const
{
    return (sawFooter || !isEntryValid(currentEntry));
}

/**
 * Progress the iterator to the next entry in the Segment, if there is one.
 * Future calls to #getType, #getLength, #getPointer, and #getOffset will
 * reflect the next SegmentEntry's parameters.
 */
void
SegmentIterator::next()
{
    type = LOG_ENTRY_TYPE_INVALID;
    length = 0;
    blobPtr = NULL;

    if (currentEntry == NULL)
        return;

    if (currentEntry->type == LOG_ENTRY_TYPE_SEGFOOTER) {
        sawFooter = true;
        return;
    }

    uintptr_t nextEntry = (uintptr_t)currentEntry + sizeof(*currentEntry) +
        currentEntry->length;
    const SegmentEntry *entry = (const SegmentEntry *)nextEntry;

    if (!isEntryValid(entry)) {
        currentEntry = NULL;
        return;
    }

    type    = entry->type;
    length  = entry->length;
    blobPtr = (const void *)((uintptr_t)entry + sizeof(*entry));
    currentEntry = entry;
}

/**
 * Obtain the type of the SegmentEntry currently being iterated over.
 * \return
 *      The type of the current entry.
 * \throw 0
 *      An exception is thrown if the iterator has no more entries.
 */
LogEntryType
SegmentIterator::getType() const
{
    if (currentEntry == NULL)
        throw 0;
    return type;
}

/**
 * Obtain the length of the SegmentEntry currently being iterated over.
 * \return
 *      The length of the current entry in bytes.
 * \throw 0
 *      An exception is thrown if the iterator has no more entries.
 */
uint64_t
SegmentIterator::getLength() const
{
    if (currentEntry == NULL)
        throw 0;
    return length;
}

/**
 * Obtain a const void* to the data associated with the current SegmentEntry. 
 * \return
 *      A const void* to the current data.
 * \throw 0
 *      An exception is thrown if the iterator has no more entries.
 */
const void *
SegmentIterator::getPointer() const
{
    if (currentEntry == NULL)
        throw 0;
    return blobPtr;
}

/**
 * Obtain the byte offset of the current SegmentEntry's data within the Segment
 * being iterated over. Note that the data offset is not the SegmentEntry
 * structure, but the typed data immediately following it.
 * \return
 *      The byte offset of the current SegmentEntry's data.
 * \throw 0
 *      An exception is thrown if the iterator has no more entries.
 */
uint64_t
SegmentIterator::getOffset() const
{
    if (currentEntry == NULL)
        throw 0;
    return (uintptr_t)blobPtr - (uintptr_t)baseAddress;
}

} // namespace
