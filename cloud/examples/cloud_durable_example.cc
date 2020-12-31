// Copyright (c) 2017-present, Rockset, Inc.  All rights reserved.
#include <cstdio>
#include <iostream>
#include <string>

#include "rocksdb/cloud/db_cloud.h"
#include "rocksdb/options.h"

using namespace rocksdb;

// This is the local directory where the db is stored.
std::string kDBPath = "/tmp/rocksdb_cloud_durable";

// This is the name of the cloud storage bucket where the db
// is made durable. if you are using AWS, you have to manually
// ensure that this bucket name is unique to you and does not
// conflict with any other S3 users who might have already created
// this bucket name.
std::string kBucketSuffix = "cloud.durable.example.";
std::string kRegion = "us-west-2";

static const bool flushAtEnd = true;
static const bool disableWAL = false;

int main() {
  // cloud environment config options here
  CloudEnvOptions cloud_env_options;

  // Store a reference to a cloud env. A new cloud env object should be
  // associated
  // with every new cloud-db.
  std::unique_ptr<CloudEnv> cloud_env;

  // Retrieve aws access keys from env
  char* keyid = getenv("AWS_ACCESS_KEY_ID");
  char* secret = getenv("AWS_SECRET_ACCESS_KEY");
  if (keyid == nullptr || secret == nullptr) {
    fprintf(
        stderr,
        "Please set env variables "
        "AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY with cloud credentials");
    return -1;
  }
  cloud_env_options.credentials.access_key_id.assign(keyid);
  cloud_env_options.credentials.secret_key.assign(secret);

  // Append the user name to the bucket name in an attempt to make it
  // globally unique. S3 bucket-names need to be globally unique.
  // If you want to rerun this example, then unique user-name suffix here.
  char* user = getenv("USER");
  kBucketSuffix.append(user);

  // create a bucket name for debugging purposes
  const std::string bucketName = "rockset." + kBucketSuffix;

  // Create a new AWS cloud env Status
  CloudEnv* cenv;
  Status s =
      CloudEnv::NewAwsEnv(Env::Default(),
                          kBucketSuffix, kDBPath, kRegion,
                          kBucketSuffix, kDBPath, kRegion,
                          cloud_env_options, nullptr, &cenv);
  if (!s.ok()) {
    fprintf(stderr, "Unable to create cloud env in bucket %s. %s\n",
            bucketName.c_str(), s.ToString().c_str());
    return -1;
  }
  cloud_env.reset(cenv);

  // Create options and use the AWS env that we created earlier
  Options options;
  options.env = cloud_env.get();
  options.create_if_missing = true;

  // No persistent read-cache
  std::string persistent_cache = "";

  // options for each write
  WriteOptions wopt;
  wopt.disableWAL = disableWAL;

  // open DB
  DBCloud* db;
  s = DBCloud::Open(options, kDBPath, persistent_cache, 0, &db);
  if (!s.ok()) {
    fprintf(stderr, "Unable to open db at path %s with bucket %s. %s\n",
            kDBPath.c_str(), bucketName.c_str(), s.ToString().c_str());
    return -1;
  }

  // Put key-value
  s = db->Put(wopt, "key1", "value");
  assert(s.ok());
  std::string value;
  // get value
  s = db->Get(ReadOptions(), "key1", &value);
  assert(s.ok());
  assert(value == "value");

  // atomically apply a set of updates
  {
    WriteBatch batch;
    batch.Delete("key1");
    batch.Put("key2", value);
    s = db->Write(wopt, &batch);
  }

  s = db->Get(ReadOptions(), "key1", &value);
  assert(s.IsNotFound());

  db->Get(ReadOptions(), "key2", &value);
  assert(value == "value");

  // print all values in the database
  rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::cout << it->key().ToString() << ": " << it->value().ToString()
              << std::endl;
  }
  delete it;

  // Flush all data from main db to sst files. Release db.
  if (flushAtEnd) {
    db->Flush(FlushOptions());
  }
  delete db;

  fprintf(stdout, "Successfully used db at path %s in bucket %s.\n",
          kDBPath.c_str(), bucketName.c_str());
  return 0;
}
