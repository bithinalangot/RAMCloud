/* Copyright (c) 2009 Stanford University
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

#ifndef RAMCLOUD_KEYUTIL_H
#define RAMCLOUD_KEYUTIL_H

namespace RAMCloud {

class MakeKey {
  public:
    explicit MakeKey(uint64_t value)
    : val(value) {}

    explicit MakeKey(int value)
    : val(static_cast<uint64_t>(value)) {}

    const char*
    get() const
    {
        return (const char*)&val;
    }

    uint16_t
    length() const
    {
        return downCast<uint16_t>(sizeof(val));
    }

    uint64_t val;
};

} // end RAMCloud

#endif // RAMCLOUD_KEYUTIL_H
