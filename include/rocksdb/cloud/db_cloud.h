//  Copyright (c) 2017-present, Rockset

#pragma once
#ifndef ROCKSDB_LITE

#include <string>
#include <vector>

#include "rocksdb/cloud/cloud_env_options.h"
#include "rocksdb/db.h"
#include "rocksdb/utilities/stackable_db.h"

namespace rocksdb {

//
// Database with Cloud support.
//
// Important: The caller is responsible for ensuring that only one database at
// a time is running with the same cloud destination bucket and path. Running
// two databases concurrently with the same destination path will lead to
// corruption if it lasts for more than couple of minutes.
class DBCloud : public StackableDB {
 public:
  // This API is to open a DB when key-values are to be made durable by
  // backing up database state into a cloud-storage system like S3.
  // All kv updates are persisted in cloud-storage.
  // options.env is an object of type rocksdb::CloudEnv and the cloud
  // buckets are specified there.
  static Status Open(const Options& options, const std::string& name,
                     const std::string& persistent_cache_path,
                     const uint64_t persistent_cache_size_gb, DBCloud** dbptr,
                     bool read_only = false);

  // This is for advanced users who can comprehend column families.
  // If you want sst files from S3 to be cached in local SSD/disk, then
  // persistent_cache_path should be the pathname of the local
  // cache storage.
  // TODO(igor/dhruba) The first argument here should be DBOptions, just like in
  // DB class.
  static Status Open(const Options& options, const std::string& dbname,
                     const std::vector<ColumnFamilyDescriptor>& column_families,
                     const std::string& persistent_cache_path,
                     const uint64_t persistent_cache_size_gb,
                     std::vector<ColumnFamilyHandle*>* handles, DBCloud** dbptr,
                     bool read_only = false);

  // Synchronously copy all relevant files (if any) from source cloud storage to
  // destination cloud storage.
  virtual Status Savepoint() = 0;

  static Status PreloadCloudManifest(CloudEnv* cenv, const Options& options,
                                     const std::string& dbname);

  virtual ~DBCloud() {}

 protected:
  explicit DBCloud(DB* db) : StackableDB(db) {}
};

}  // namespace rocksdb
#endif  // ROCKSDB_LITE
