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

#ifndef RAMCLOUD_SERVER_H
#define RAMCLOUD_SERVER_H

#include <Common.h>

#include <Object.h>
#include <Log.h>
#include <BackupClient.h>

#include <Net.h>
#include <Hashtable.h>

namespace RAMCloud {

class Table {
  public:
    static const int TABLE_NAME_MAX_LEN = 64;
    explicit Table() : next_key(0), next_version(1), object_map(HASH_NLINES) {
    }
    const char *GetName() { return &name[0]; }
    void SetName(const char *new_name) {
        strncpy(&name[0], new_name, TABLE_NAME_MAX_LEN);
        name[TABLE_NAME_MAX_LEN - 1] = '\0';
    }
    uint64_t AllocateKey() {
        while (Get(next_key))
            ++next_key;
        return next_key;
    }
    uint64_t AllocateVersion() {
        return next_version++;
    }
    const Object *Get(uint64_t key) {
        void *val = object_map.Lookup(key);
        const Object *o = static_cast<const Object *>(val);
        return o;
    }
    void Put(uint64_t key, const Object *o) {
        object_map.Delete(key);
        object_map.Insert(key, const_cast<Object *>(o));
    }
    void Delete(uint64_t key) {
        object_map.Delete(key);
    }

  private:
    char name[64];
    uint64_t next_key;
    uint64_t next_version;
    Hashtable object_map;
    DISALLOW_COPY_AND_ASSIGN(Table);
};

struct ServerConfig {
    // Restore from backups before resuming operation
    bool restore;
  ServerConfig() : restore(false) {}
};

class Server {
  public:
    void Ping(const rcrpc_ping_request *req,
              rcrpc_ping_response *resp);
    void Read(const rcrpc_read_request *req,
              rcrpc_read_response *resp);
    void Write(const rcrpc_write_request *req,
               rcrpc_write_response *resp);
    void InsertKey(const rcrpc_insert_request *req,
                   rcrpc_insert_response *resp);
    void DeleteKey(const rcrpc_delete_request *req,
                   rcrpc_delete_response *resp);
    void CreateTable(const rcrpc_create_table_request *req,
                     rcrpc_create_table_response *resp);
    void OpenTable(const rcrpc_open_table_request *req,
                   rcrpc_open_table_response *resp);
    void DropTable(const rcrpc_drop_table_request *req,
                   rcrpc_drop_table_response *resp);

    explicit Server(const ServerConfig *sconfig, Net *net_impl);
    Server(const Server& server);
    Server& operator=(const Server& server);
    ~Server();
    void Run();

  private:
    static bool RejectOperation(const rcrpc_reject_rules *reject_rules,
                                uint64_t version);
    void Restore();
    void HandleRPC();
    bool StoreData(uint64_t table,
                   uint64_t key,
                   const rcrpc_reject_rules *reject_rules,
                   const char *buf,
                   uint64_t buf_len,
                   uint64_t *new_version);
    explicit Server();

    const ServerConfig *config;
    Log *log;
    Net *net;
    BackupClient backup;
    Table tables[RC_NUM_TABLES];
    friend void ObjectEvictionCallback(log_entry_type_t type,
                                    const void *p,
                                    uint64_t len,
                                    void *cookie);
    friend void TombstoneEvictionCallback(log_entry_type_t type,
                                    const void *p,
                                    uint64_t len,
                                    void *cookie);
    friend void SegmentReplayCallback(Segment *seg, void *cookie);
    friend void ObjectReplayCallback(log_entry_type_t type,
                                     const void *p,
                                     uint64_t len,
                                     void *cookie);
};

} // namespace RAMCloud

#endif