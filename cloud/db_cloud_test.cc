// Copyright (c) 2017 Rockset

#ifndef ROCKSDB_LITE

#ifdef USE_AWS

#include "rocksdb/cloud/db_cloud.h"
#include <algorithm>
#include <chrono>
#include "cloud/aws/aws_env.h"
#include "cloud/aws/aws_file.h"
#include "cloud/db_cloud_impl.h"
#include "cloud/filename.h"
#include "cloud/manifest_reader.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "file/filename.h"
#include "logging/logging.h"
#include "util/string_util.h"
#include "test_util/testharness.h"
#ifndef OS_WIN
#include <unistd.h>
#endif

namespace rocksdb {

class CloudTest : public testing::Test {
 public:
  CloudTest() {
    base_env_ = Env::Default();
    dbname_ = test::TmpDir() + "/db_cloud";
    clone_dir_ = test::TmpDir() + "/ctest";
    src_bucket_prefix_ = "dbcloud." + AwsEnv::GetTestBucketSuffix();
    src_object_prefix_ = dbname_;
    // Set dest = src by default
    dest_bucket_prefix_ = src_bucket_prefix_;
    dest_object_prefix_ = src_object_prefix_;
    options_.create_if_missing = true;
    persistent_cache_path_ = "";
    persistent_cache_size_gb_ = 0;
    db_ = nullptr;

    DestroyDir(dbname_);
    base_env_->CreateDirIfMissing(dbname_);
    base_env_->NewLogger(test::TmpDir(base_env_) + "/rocksdb-cloud.log",
                         &options_.info_log);
    options_.info_log->SetInfoLogLevel(InfoLogLevel::DEBUG_LEVEL);

    // Get cloud credentials
    AwsEnv::GetTestCredentials(&cloud_env_options_.credentials.access_key_id,
                               &cloud_env_options_.credentials.secret_key,
                               &region_);
    Cleanup();
  }

  void Cleanup() {
    ASSERT_TRUE(!aenv_);

    CloudEnv* aenv;
    // create a dummy aws env
    ASSERT_OK(CloudEnv::NewAwsEnv(
        base_env_, src_bucket_prefix_, src_object_prefix_, region_,
        dest_bucket_prefix_, dest_object_prefix_, region_, cloud_env_options_,
        options_.info_log, &aenv));
    aenv_.reset(aenv);
    // delete all pre-existing contents from the bucket
    Status st = aenv_->EmptyBucket(src_bucket_prefix_, "");
    ASSERT_TRUE(st.ok() || st.IsNotFound());
    aenv_.reset();

    // delete and create directory where clones reside
    DestroyDir(clone_dir_);
    ASSERT_OK(base_env_->CreateDir(clone_dir_));
  }

  std::set<std::string> GetSSTFiles(std::string name) {
    std::vector<std::string> files;
    aenv_->GetBaseEnv()->GetChildren(name, &files);
    std::set<std::string> sst_files;
    for (auto& f : files) {
      if (IsSstFile(RemoveEpoch(f))) {
        sst_files.insert(f);
      }
    }
    return sst_files;
  }

  std::set<std::string> GetSSTFilesClone(std::string name) {
    std::string cname = clone_dir_ + "/" + name;
    return GetSSTFiles(cname);
  }

  void DestroyDir(const std::string& dir) {
    std::string cmd = "rm -rf " + dir;
    int rc = system(cmd.c_str());
    ASSERT_EQ(rc, 0);
  }

  virtual ~CloudTest() { CloseDB(); }

  void CreateAwsEnv() {
    CloudEnv* aenv;
    ASSERT_OK(CloudEnv::NewAwsEnv(
        base_env_, src_bucket_prefix_, src_object_prefix_, region_,
        dest_bucket_prefix_, dest_object_prefix_, region_, cloud_env_options_,
        options_.info_log, &aenv));
    // To catch any possible file deletion bugs, we set file deletion delay to
    // smallest possible
    if (!src_bucket_prefix_.empty() || !dest_bucket_prefix_.empty()) {
      ((AwsEnv*)aenv)->TEST_SetFileDeletionDelay(std::chrono::seconds(0));
    }
    aenv_.reset(aenv);
  }

  // Open database via the cloud interface
  void OpenDB() {
    ASSERT_NE(cloud_env_options_.credentials.access_key_id.size(), 0);
    ASSERT_NE(cloud_env_options_.credentials.secret_key.size(), 0);

    // Create new AWS env
    CreateAwsEnv();
    options_.env = aenv_.get();

    // default column family
    ColumnFamilyOptions cfopt = options_;
    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(
        ColumnFamilyDescriptor(kDefaultColumnFamilyName, cfopt));
    std::vector<ColumnFamilyHandle*> handles;

    ASSERT_TRUE(db_ == nullptr);
    ASSERT_OK(DBCloud::Open(options_, dbname_, column_families,
                            persistent_cache_path_, persistent_cache_size_gb_,
                            &handles, &db_));
    ASSERT_OK(db_->GetDbIdentity(dbid_));

    // Delete the handle for the default column family because the DBImpl
    // always holds a reference to it.
    ASSERT_TRUE(handles.size() > 0);
    delete handles[0];
  }

  // Creates and Opens a clone
  void CloneDB(const std::string& clone_name, const std::string& src_bucket,
               const std::string& src_object_path,
               const std::string& dest_bucket,
               const std::string& dest_object_path,
               std::unique_ptr<DBCloud>* cloud_db,
               std::unique_ptr<CloudEnv>* cloud_env) {
    // The local directory where the clone resides
    std::string cname = clone_dir_ + "/" + clone_name;

    CloudEnv* cenv;
    DBCloud* clone_db;

    // If there is no destination bucket, then the clone needs to copy
    // all sst fies from source bucket to local dir
    CloudEnvOptions copt = cloud_env_options_;
    if (dest_bucket.empty()) {
      copt.keep_local_sst_files = true;
    }

    // Create new AWS env
    ASSERT_OK(CloudEnv::NewAwsEnv(base_env_, src_bucket, src_object_path,
                                  region_, dest_bucket, dest_object_path,
                                  region_, copt, options_.info_log, &cenv));
    // To catch any possible file deletion bugs, we set file deletion delay to
    // smallest possible
    ((AwsEnv*)cenv)->TEST_SetFileDeletionDelay(std::chrono::seconds(0));

    // sets the cloud env to be used by the env wrapper
    options_.env = cenv;

    // Returns the cloud env that was created
    cloud_env->reset(cenv);

    // default column family
    ColumnFamilyOptions cfopt = options_;

    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(
        ColumnFamilyDescriptor(kDefaultColumnFamilyName, cfopt));
    std::vector<ColumnFamilyHandle*> handles;

    ASSERT_OK(DBCloud::Open(options_, cname, column_families,
                            persistent_cache_path_, persistent_cache_size_gb_,
                            &handles, &clone_db));
    cloud_db->reset(clone_db);

    // Delete the handle for the default column family because the DBImpl
    // always holds a reference to it.
    ASSERT_TRUE(handles.size() > 0);
    delete handles[0];
  }

  void CloseDB() {
    if (db_) {
      db_->Flush(FlushOptions());  // convert pending writes to sst files
      delete db_;
      db_ = nullptr;
    }
  }

  void SetPersistentCache(const std::string& path, uint64_t size_gb) {
    persistent_cache_path_ = path;
    persistent_cache_size_gb_ = size_gb;
  }

  Status GetCloudLiveFilesSrc(std::set<uint64_t>* list) {
    std::unique_ptr<ManifestReader> manifest(
        new ManifestReader(options_.info_log, aenv_.get(), src_bucket_prefix_));
    return manifest->GetLiveFiles(src_object_prefix_, list);
  }

  // Verify that local files are the same as cloud files in src bucket path
  void ValidateCloudLiveFilesSrcSize() {

    // Loop though all the files in the cloud manifest
    std::set<uint64_t> cloud_files;
    ASSERT_OK(GetCloudLiveFilesSrc(&cloud_files));
    for (uint64_t num: cloud_files) {
      std::string pathname = MakeTableFileName(dbname_, num);
      Log(options_.info_log, "cloud file list  %s\n", pathname.c_str());
    }

    std::set<std::string> localFiles = GetSSTFiles(dbname_);
    uint64_t cloudSize = 0;
    uint64_t localSize = 0;

    // loop through all the local files and validate
    for (std::string path: localFiles) {
      std::string cpath = src_object_prefix_ + "/" + path;
      ASSERT_OK(aenv_->GetObjectSize(src_bucket_prefix_, cpath, &cloudSize));

      // find the size of the file on local storage
      std::string lpath = dbname_ + "/" + path;
      ASSERT_OK(aenv_->GetBaseEnv()->GetFileSize(lpath, &localSize));
      ASSERT_TRUE(localSize == cloudSize);
      Log(options_.info_log, "local file %s size %ld\n", lpath.c_str(), localSize);
      Log(options_.info_log, "cloud file %s size %ld\n", cpath.c_str(), cloudSize);
      printf("local file %s size %ld\n", lpath.c_str(), localSize);
      printf("cloud file %s size %ld\n", cpath.c_str(), cloudSize);
    }
  }

 protected:
  Env* base_env_;
  Options options_;
  std::string dbname_;
  std::string clone_dir_;
  std::string src_bucket_prefix_;
  std::string src_object_prefix_;
  std::string dest_bucket_prefix_;
  std::string dest_object_prefix_;
  CloudEnvOptions cloud_env_options_;
  std::string region_;
  std::string dbid_;
  std::string persistent_cache_path_;
  uint64_t persistent_cache_size_gb_;
  DBCloud* db_;
  std::unique_ptr<CloudEnv> aenv_;
};

//
// Most basic test. Create DB, write one key, close it and then check to see
// that the key exists.
//
TEST_F(CloudTest, BasicTest) {
  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();

  // Reopen and validate
  OpenDB();
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");

  std::set<uint64_t> live_files;
  ASSERT_OK(GetCloudLiveFilesSrc(&live_files));
  ASSERT_GT(live_files.size(), 0);
  CloseDB();
}

TEST_F(CloudTest, GetChildrenTest) {
  // Create some objects in S3
  OpenDB();
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Flush(FlushOptions()));

  CloseDB();
  DestroyDir(dbname_);
  OpenDB();

  std::vector<std::string> children;
  ASSERT_OK(aenv_->GetChildren(dbname_, &children));
  int sst_files = 0;
  for (auto c : children) {
    if (IsSstFile(c)) {
      sst_files++;
    }
  }
  // This verifies that GetChildren() works on S3. We deleted the S3 file
  // locally, so the only way to actually get it through GetChildren() if
  // listing S3 buckets works.
  EXPECT_EQ(sst_files, 1);
}

//
// Create and read from a clone.
//
TEST_F(CloudTest, Newdb) {
  std::string master_dbid;
  std::string newdb1_dbid;
  std::string newdb2_dbid;

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  ASSERT_OK(db_->GetDbIdentity(master_dbid));
  CloseDB();
  value.clear();

  {
    // Create and Open a new ephemeral instance
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("newdb1", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of the first reopen
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // This is an ephemeral clone. Its dbid is a prefix of the master's.
    ASSERT_NE(newdb1_dbid, master_dbid);
    auto res = std::mismatch(master_dbid.begin(), master_dbid.end(),
                             newdb1_dbid.begin());
    ASSERT_TRUE(res.first == master_dbid.end());

    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // Open master and write one more kv to it. This is written to
    // src bucket as well.
    OpenDB();
    ASSERT_OK(db_->Put(WriteOptions(), "Dhruba", "Borthakur"));

    // check that the newly written kv exists
    value.clear();
    ASSERT_OK(db_->Get(ReadOptions(), "Dhruba", &value));
    ASSERT_TRUE(value.compare("Borthakur") == 0);

    // check that the earlier kv exists too
    value.clear();
    ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    CloseDB();

    // Assert  that newdb1 cannot see the second kv because the second kv
    // was written to local dir only of the ephemeral clone.
    ASSERT_TRUE(cloud_db->Get(ReadOptions(), "Dhruba", &value).IsNotFound());
  }
  {
    // Create another ephemeral instance using a different local dir but the same two
    // buckets as newdb1. This should be identical in contents with newdb1.
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("newdb2", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of the second clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));

    // Since we use two different local directories for the two ephemeral
    // clones, their dbids should be different from one another
    ASSERT_NE(newdb1_dbid, newdb2_dbid);

    // check that both the kvs appear in the new ephemeral clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Dhruba", &value));
    ASSERT_TRUE(value.compare("Borthakur") == 0);
  }

  CloseDB();
}

//
// Create and read from a clone.
//
TEST_F(CloudTest, TrueClone) {
  std::string master_dbid;
  std::string newdb1_dbid;
  std::string newdb2_dbid;
  std::string newdb3_dbid;

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  ASSERT_OK(db_->GetDbIdentity(master_dbid));
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDB();
  value.clear();
  {
    // Create a new instance with different src and destination paths.
    // This is true clone and should have all the contents of the masterdb
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath1", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, "clone1_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // Since we used the different src and destination paths for both
    // the master and clone1, the clone should have its own identity.
    ASSERT_NE(master_dbid, newdb1_dbid);

    // check that the original kv appears in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // write a new value to the clone
    ASSERT_OK(cloud_db->Put(WriteOptions(), "Hello", "Clone1"));
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
    ASSERT_OK(cloud_db->Flush(FlushOptions()));
  }
  {
    // Reopen clone1 with a different local path
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath2", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, "clone1_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));
    ASSERT_EQ(newdb1_dbid, newdb2_dbid);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
    ASSERT_OK(cloud_db->Flush(FlushOptions()));
  }
  {
    // Reopen clone1 with the same local path as above.
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath2", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, "clone1_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));
    ASSERT_EQ(newdb1_dbid, newdb2_dbid);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
    ASSERT_OK(cloud_db->Flush(FlushOptions()));
  }
  {
    // Create clone2
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath3",  // xxx try with localpath2
            src_bucket_prefix_, src_object_prefix_, src_bucket_prefix_,
            "clone2_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb3_dbid));
    ASSERT_NE(newdb2_dbid, newdb3_dbid);

    // verify that data is still as it was in the original db.
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // Assert that there are no redundant sst files
    CloudEnvImpl* env = static_cast<CloudEnvImpl*>(cloud_env.get());
    std::vector<std::string> to_be_deleted;
    ASSERT_OK(env->FindObsoleteFiles(src_bucket_prefix_, &to_be_deleted));
    // TODO(igor): Re-enable once purger code is fixed
    // ASSERT_EQ(to_be_deleted.size(), 0);

    // Assert that there are no redundant dbid
    ASSERT_OK(env->FindObsoleteDbid(src_bucket_prefix_, &to_be_deleted));
    // TODO(igor): Re-enable once purger code is fixed
    // ASSERT_EQ(to_be_deleted.size(), 0);
  }
}

//
// verify that dbid registry is appropriately handled
//
TEST_F(CloudTest, DbidRegistry) {
  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);

  // Assert that there is one db in the registry
  DbidList dbs;
  ASSERT_OK(aenv_->GetDbidList(src_bucket_prefix_, &dbs));
  ASSERT_EQ(dbs.size(), 1);

  CloseDB();
}

TEST_F(CloudTest, KeepLocalFiles) {
  cloud_env_options_.keep_local_sst_files = true;
  // Create two files
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Hello2", "World2"));
  ASSERT_OK(db_->Flush(FlushOptions()));

  CloseDB();
  DestroyDir(dbname_);
  OpenDB();

  std::vector<std::string> files;
  ASSERT_OK(Env::Default()->GetChildren(dbname_, &files));
  long sst_files =
      std::count_if(files.begin(), files.end(), [](const std::string& file) {
        return file.find("sst") != std::string::npos;
      });
  ASSERT_EQ(sst_files, 2);

  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
  ASSERT_OK(db_->Get(ReadOptions(), "Hello2", &value));
  ASSERT_EQ(value, "World2");

  CloseDB();
  ValidateCloudLiveFilesSrcSize();
}


TEST_F(CloudTest, CopyToFromS3) {
  std::string fname = dbname_ + "/100000.sst";

  // Create aws env
  cloud_env_options_.keep_local_sst_files = true;
  CreateAwsEnv();
  ((CloudEnvImpl*)aenv_.get())->TEST_InitEmptyCloudManifest();
  char buffer[1 * 1024 * 1024];

  // create a 10 MB file and upload it to cloud
  {
    std::unique_ptr<WritableFile> writer;
    ASSERT_OK(aenv_->NewWritableFile(fname, &writer, EnvOptions()));

    for (int i = 0; i < 10; i++) {
      ASSERT_OK(writer->Append(Slice(buffer, sizeof(buffer))));
    }
    // sync and close file
  }

  // delete the file manually.
  ASSERT_OK(base_env_->DeleteFile(fname));

  // reopen file for reading. It should be refetched from cloud storage.
  {
    std::unique_ptr<RandomAccessFile> reader;
    ASSERT_OK(aenv_->NewRandomAccessFile(fname, &reader, EnvOptions()));

    uint64_t offset = 0;
    for (int i = 0; i < 10; i++) {
      Slice result;
      char* scratch = &buffer[0];
      ASSERT_OK(reader->Read(offset, sizeof(buffer), &result, scratch));
      ASSERT_EQ(result.size(), sizeof(buffer));
      offset += sizeof(buffer);
    }
  }
}

TEST_F(CloudTest, DelayFileDeletion) {
  std::string fname = dbname_ + "/000010.sst";

  // Create aws env
  cloud_env_options_.keep_local_sst_files = true;
  CreateAwsEnv();
  ((CloudEnvImpl*)aenv_.get())->TEST_InitEmptyCloudManifest();
  ((AwsEnv*)aenv_.get())->TEST_SetFileDeletionDelay(std::chrono::seconds(2));

  auto createFile = [&]() {
    std::unique_ptr<WritableFile> writer;
    ASSERT_OK(aenv_->NewWritableFile(fname, &writer, EnvOptions()));

    for (int i = 0; i < 10; i++) {
      ASSERT_OK(writer->Append("igor"));
    }
    // sync and close file
  };

  for (int iter = 0; iter <= 1; ++iter) {
    createFile();
    // delete the file
    ASSERT_OK(aenv_->DeleteFile(fname));
    // file should still be there
    ASSERT_OK(aenv_->FileExists(fname));

    if (iter == 1) {
      // should prevent the deletion
      createFile();
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    auto st = aenv_->FileExists(fname);
    if (iter == 0) {
      // in iter==0 file should be deleted after 2 seconds
      ASSERT_TRUE(st.IsNotFound());
    } else {
      // in iter==1 file should not be deleted because we wrote the new file
      ASSERT_OK(st);
    }
  }
}

// Verify that a savepoint copies all src files to destination
TEST_F(CloudTest, Savepoint) {
  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();
  std::string dest_path = "/clone2_path";
  {
    // Create a new instance with different src and destination paths.
    // This is true clone and should have all the contents of the masterdb
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath1", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, dest_path, &cloud_db, &cloud_env);

    // check that the original kv appears in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // there should be only one sst file
    std::vector<LiveFileMetaData> flist;
    cloud_db->GetLiveFilesMetaData(&flist);
    ASSERT_TRUE(flist.size() == 1);

    auto remapped_fname =
        ((CloudEnvImpl*)cloud_env.get())->RemapFilename(flist[0].name);
    // source path
    std::string spath = src_object_prefix_ + "/" + remapped_fname;
    ASSERT_OK(cloud_env->ExistsObject(src_bucket_prefix_, spath));

    // Verify that the destination path does not have any sst files
    std::string dpath = dest_path + "/" + remapped_fname;
    ASSERT_TRUE(
        cloud_env->ExistsObject(src_bucket_prefix_, dpath).IsNotFound());

    // write a new value to the clone
    ASSERT_OK(cloud_db->Put(WriteOptions(), "Hell", "Done"));
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hell", &value));
    ASSERT_TRUE(value.compare("Done") == 0);

    // Invoke savepoint to populate destination path from source path
    ASSERT_OK(cloud_db->Savepoint());

    // check that the sst file is copied to dest path
    ASSERT_OK(cloud_env->ExistsObject(src_bucket_prefix_, dpath));
    ASSERT_OK(cloud_db->Flush(FlushOptions()));
  }
  {
    // Reopen the clone
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath2", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, dest_path, &cloud_db, &cloud_env);

    // check that the both kvs appears in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hell", &value));
    ASSERT_TRUE(value.compare("Done") == 0);
  }
}

TEST_F(CloudTest, Encryption) {
  // Create aws env
  cloud_env_options_.server_side_encryption = true;
  char* key_id = getenv("AWS_KMS_KEY_ID");
  if (key_id != nullptr) {
    cloud_env_options_.encryption_key_id = std::string(key_id);
    Log(options_.info_log, "Found encryption key id in env variable %s",
        key_id);
  }

  OpenDB();

  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  // create a file
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDB();

  OpenDB();
  std::string value;
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
  CloseDB();
}

#ifdef USE_KAFKA
TEST_F(CloudTest, KeepLocalLogKafka) {
  cloud_env_options_.keep_local_log_files = false;
  cloud_env_options_.log_type = LogType::kLogKafka;
  cloud_env_options_.kafka_log_options.client_config_params["metadata.broker.list"]
      = "localhost:9092";

  OpenDB();

  ASSERT_OK(db_->Put(WriteOptions(), "Franz", "Kafka"));

  // Destroy DB in memory and on local file system.
  delete db_;
  db_ = nullptr;
  aenv_.reset();
  DestroyDir(dbname_);
  DestroyDir("/tmp/ROCKSET");

  // Create new env.
  CreateAwsEnv();

  // Give env enough time to consume WALs
  std::this_thread::sleep_for(std::chrono::seconds(3));

  // Open DB.
  cloud_env_options_.keep_local_log_files = true;
  options_.wal_dir = static_cast<AwsEnv*>(aenv_.get())->GetWALCacheDir();
  OpenDB();

  // Test read.
  std::string value;
  ASSERT_OK(db_->Get(ReadOptions(), "Franz", &value));
  ASSERT_EQ(value, "Kafka");

  CloseDB();
}
#endif /* USE_KAFKA */

// TODO(igor): determine why this fails,
// https://github.com/rockset/rocksdb-cloud/issues/35
TEST_F(CloudTest, DISABLED_KeepLocalLogKinesis) {
  cloud_env_options_.keep_local_log_files = false;
  cloud_env_options_.log_type = LogType::kLogKinesis;

  OpenDB();

  // Test write.
  ASSERT_OK(db_->Put(WriteOptions(), "Tele", "Kinesis"));

  // Destroy DB in memory and on local file system.
  delete db_;
  db_ = nullptr;
  aenv_.reset();
  DestroyDir(dbname_);
  DestroyDir("/tmp/ROCKSET");

  // Create new env.
  CreateAwsEnv();

  // Give env enough time to consume WALs
  std::this_thread::sleep_for(std::chrono::seconds(3));

  // Open DB.
  cloud_env_options_.keep_local_log_files = true;
  options_.wal_dir = static_cast<AwsEnv*>(aenv_.get())->GetWALCacheDir();
  OpenDB();

  // Test read.
  std::string value;
  ASSERT_OK(db_->Get(ReadOptions(), "Tele", &value));
  ASSERT_EQ(value, "Kinesis");

  CloseDB();
}

// Test whether we are able to recover nicely from two different writers to the
// same S3 bucket. (The feature that was enabled by CLOUDMANIFEST)
TEST_F(CloudTest, TwoDBsOneBucket) {
  auto firstDB = dbname_;
  auto secondDB = dbname_ + "-1";
  cloud_env_options_.keep_local_sst_files = true;
  std::string value;

  OpenDB();
  auto firstManifestFile =
      dest_object_prefix_ + "/" +
      ((CloudEnvImpl*)aenv_.get())->RemapFilename("MANIFEST-1");
  EXPECT_OK(aenv_->ExistsObject(dest_bucket_prefix_, firstManifestFile));
  // Create two files
  ASSERT_OK(db_->Put(WriteOptions(), "First", "File"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Second", "File"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  auto files = GetSSTFiles(dbname_);
  EXPECT_EQ(files.size(), 2);
  CloseDB();

  // Open again, with no destination bucket
  dest_bucket_prefix_.clear();
  dest_object_prefix_.clear();
  OpenDB();
  ASSERT_OK(db_->Put(WriteOptions(), "Third", "File"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  auto newFiles = GetSSTFiles(dbname_);
  EXPECT_EQ(newFiles.size(), 3);
  // Remember the third file we created
  std::vector<std::string> diff;
  std::set_difference(newFiles.begin(), newFiles.end(), files.begin(),
                      files.end(), std::inserter(diff, diff.begin()));
  ASSERT_EQ(diff.size(), 1);
  auto thirdFile = diff[0];
  CloseDB();

  // Open in a different directory with destination bucket set
  dbname_ = secondDB;
  dest_bucket_prefix_ = src_bucket_prefix_;
  dest_object_prefix_ = src_object_prefix_;
  OpenDB();
  ASSERT_OK(db_->Put(WriteOptions(), "Third", "DifferentFile"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDB();

  // Open back in the first directory with no destination
  dbname_ = firstDB;
  dest_bucket_prefix_.clear();
  dest_object_prefix_.clear();
  OpenDB();
  // Changes to the cloud database should make no difference for us. This is an
  // important check because we should not reinitialize from the cloud if we
  // have a valid local directory!
  ASSERT_OK(db_->Get(ReadOptions(), "Third", &value));
  EXPECT_EQ(value, "File");
  CloseDB();

  // Reopen in the first directory, this time with destination path
  dbname_ = firstDB;
  dest_bucket_prefix_ = src_bucket_prefix_;
  dest_object_prefix_ = src_object_prefix_;
  OpenDB();
  // Changes to the cloud database should be pulled down now.
  ASSERT_OK(db_->Get(ReadOptions(), "Third", &value));
  EXPECT_EQ(value, "DifferentFile");
  files = GetSSTFiles(dbname_);
  // Should no longer be in my directory because it's not part of the new
  // MANIFEST.
  EXPECT_TRUE(files.find(thirdFile) == files.end());

  // We need to sleep a bit because file deletion happens in a different thread,
  // so it might not be immediately deleted.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(
      aenv_->ExistsObject(dest_bucket_prefix_, firstManifestFile).IsNotFound());
  CloseDB();
}

// This test is similar to TwoDBsOneBucket, but is much more chaotic and illegal
// -- it runs two databases on exact same S3 bucket. The work on CLOUDMANIFEST
// enables us to run in that configuration for extended amount of time (1 hour
// by default) without any issues -- the last CLOUDMANIFEST writer wins.
TEST_F(CloudTest, TwoConcurrentWriters) {
  auto firstDB = dbname_;
  auto secondDB = dbname_ + "-1";

  DBCloud *db1, *db2;
  CloudEnv *aenv1, *aenv2;

  auto openDB1 = [&] {
    dbname_ = firstDB;
    OpenDB();
    db1 = db_;
    db_ = nullptr;
    aenv1 = aenv_.release();
  };
  auto openDB2 = [&] {
    dbname_ = secondDB;
    OpenDB();
    db2 = db_;
    db_ = nullptr;
    aenv2 = aenv_.release();
  };
  auto closeDB1 = [&] {
    db_ = db1;
    aenv_.reset(aenv1);
    CloseDB();
  };
  auto closeDB2 = [&] {
    db_ = db2;
    aenv_.reset(aenv2);
    CloseDB();
  };

  openDB1();
  openDB2();

  // Create bunch of files, reopening the databases during
  for (int i = 0; i < 5; ++i) {
    closeDB1();
    if (i == 2) {
      DestroyDir(firstDB);
    }
    // opening the database makes me a master (i.e. CLOUDMANIFEST points to my
    // manifest), my writes are applied to the shared space!
    openDB1();
    for (int j = 0; j < 5; ++j) {
      auto key = ToString(i) + ToString(j) + "1";
      ASSERT_OK(db1->Put(WriteOptions(), key, "FirstDB"));
      ASSERT_OK(db1->Flush(FlushOptions()));
    }
    closeDB2();
    if (i == 2) {
      DestroyDir(secondDB);
    }
    // opening the database makes me a master (i.e. CLOUDMANIFEST points to my
    // manifest), my writes are applied to the shared space!
    openDB2();
    for (int j = 0; j < 5; ++j) {
      auto key = ToString(i) + ToString(j) + "2";
      ASSERT_OK(db2->Put(WriteOptions(), key, "SecondDB"));
      ASSERT_OK(db2->Flush(FlushOptions()));
    }
  }

  dbname_ = firstDB;
  // This write should not be applied, because DB2 is currently the owner of the
  // S3 bucket
  ASSERT_OK(db1->Put(WriteOptions(), "ShouldNotBeApplied", ""));
  ASSERT_OK(db1->Flush(FlushOptions()));

  closeDB1();
  closeDB2();

  openDB1();
  for (int i = 0; i < 5; ++i) {
    for (int j = 0; j < 5; ++j) {
      std::string val;
      auto key = ToString(i) + ToString(j);
      ASSERT_OK(db1->Get(ReadOptions(), key + "1", &val));
      EXPECT_EQ(val, "FirstDB");
      ASSERT_OK(db1->Get(ReadOptions(), key + "2", &val));
      EXPECT_EQ(val, "SecondDB");
    }
  }

  std::string v;
  ASSERT_TRUE(db1->Get(ReadOptions(), "ShouldNotBeApplied", &v).IsNotFound());
}

// Creates a pure RocksDB database and makes sure we can migrate to RocksDB
// Cloud
TEST_F(CloudTest, MigrateFromPureRocksDB) {
  {  // Create local RocksDB
    Options options;
    options.create_if_missing = true;
    DB* dbptr;
    std::unique_ptr<DB> db;
    ASSERT_OK(DB::Open(options, dbname_, &dbptr));
    db.reset(dbptr);
    // create 5 files
    for (int i = 0; i < 5; ++i) {
      auto key = "key" + ToString(i);
      ASSERT_OK(db->Put(WriteOptions(), key, key));
      ASSERT_OK(db->Flush(FlushOptions()));
    }
  }
  // Now open RocksDB cloud
  // TODO(dhruba) Figure out how to make this work without skipping dbid
  // verification
  cloud_env_options_.skip_dbid_verification = true;
  OpenDB();
  for (int i = 5; i < 10; ++i) {
    auto key = "key" + ToString(i);
    ASSERT_OK(db_->Put(WriteOptions(), key, key));
    ASSERT_OK(db_->Flush(FlushOptions()));
  }

  for (int i = 0; i < 10; ++i) {
    auto key = "key" + ToString(i);
    std::string value;
    ASSERT_OK(db_->Get(ReadOptions(), key, &value));
    ASSERT_EQ(value, key);
  }
  CloseDB();
}

// Tests that we can open cloud DB without destination and source bucket set.
// This is useful for tests.
TEST_F(CloudTest, NoDestOrSrc) {
  DestroyDir(dbname_);
  // cloud_env_options_.keep_local_sst_files = true;
  dest_bucket_prefix_ = dest_object_prefix_ = src_bucket_prefix_ =
      src_object_prefix_ = "";
  OpenDB();
  ASSERT_OK(db_->Put(WriteOptions(), "key", "value"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  std::string value;
  ASSERT_OK(db_->Get(ReadOptions(), "key", &value));
  ASSERT_EQ(value, "value");
  CloseDB();
  OpenDB();
  ASSERT_OK(db_->Get(ReadOptions(), "key", &value));
  ASSERT_EQ(value, "value");
  CloseDB();
}

TEST_F(CloudTest, PreloadCloudManifest) {
  DestroyDir(dbname_);
  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();

  // Reopen and validate, preload cloud manifest
  DBCloud::PreloadCloudManifest(aenv_.get(), options_, dbname_);

  OpenDB();
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
}

//
// Test Ephemeral mode. In this mode, the database is cloned
// from a cloud bucket but new writes are not propagated
// back to any cloud bucket. Once cloned, all updates are local.
//
TEST_F(CloudTest, Ephemeral) {
  cloud_env_options_.keep_local_sst_files = true;
  options_.level0_file_num_compaction_trigger = 100; // never compact

  // Create a primary DB with two files
  OpenDB();
  std::string value;
  std::string newdb1_dbid;
  std::set<uint64_t> cloud_files;
  ASSERT_OK(db_->Put(WriteOptions(), "Name", "dhruba"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Hello2", "borthakur"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDB();
  ASSERT_EQ(2, GetSSTFiles(dbname_).size());

  // Reopen the same database in ephemeral mode by cloning the original.
  // Do not destroy the local dir. Writes to this db does not make it back
  // to any cloud storage.
  {
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("db_ephemeral", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of the first reopen
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // verify that we still have two sst files
    ASSERT_EQ(2, GetSSTFilesClone("db_ephemeral").size());

    ASSERT_OK(cloud_db->Get(ReadOptions(), "Name", &value));
    ASSERT_EQ(value, "dhruba");
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello2", &value));
    ASSERT_EQ(value, "borthakur");

    // Write one more record.
    // There should be 3 local sst files in the ephemeral db.
    ASSERT_OK(cloud_db->Put(WriteOptions(), "zip", "94087"));
    ASSERT_OK(cloud_db->Flush(FlushOptions()));
    ASSERT_EQ(3, GetSSTFilesClone("db_ephemeral").size());

    // check that cloud files did not get updated
    ASSERT_OK(GetCloudLiveFilesSrc(&cloud_files));
    ASSERT_EQ(2, cloud_files.size());
    cloud_files.clear();
  }

  // reopen main db and write two more records to it
  OpenDB();
  ASSERT_EQ(2, GetSSTFiles(dbname_).size());

  // write two more records to it.
  ASSERT_OK(db_->Put(WriteOptions(), "Key1", "onlyInMainDB"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Key2", "onlyInMainDB"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_EQ(4, GetSSTFiles(dbname_).size());
  CloseDB();
  ASSERT_OK(GetCloudLiveFilesSrc(&cloud_files));
  ASSERT_EQ(4, cloud_files.size());
  cloud_files.clear();

  // At this point, the main db has 4 files while the ephemeral
  // database has diverged earlier with 3 local files. If we try
  // to reopen the ephemeral clone, it should not download new
  // files from the cloud
  {
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    std::string dbid;
    options_.info_log = nullptr;
    CreateLoggerFromOptions(clone_dir_ + "/db_ephemeral", options_, &options_.info_log);

    CloneDB("db_ephemeral", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of this clone. It should be same as before
    ASSERT_OK(cloud_db->GetDbIdentity(dbid));
    ASSERT_EQ(newdb1_dbid, dbid);

    ASSERT_EQ(3, GetSSTFilesClone("db_ephemeral").size());

    // verify that a key written to the ephemeral db still exists
    ASSERT_OK(cloud_db->Get(ReadOptions(), "zip", &value));
    ASSERT_EQ(value, "94087");

    // verify that keys written to the main db after the ephemeral
    // was clones do not appear in the ephemeral db.
    ASSERT_NOK(cloud_db->Get(ReadOptions(), "Key1", &value));
    ASSERT_NOK(cloud_db->Get(ReadOptions(), "Key2", &value));
  }
}

//
// Test Ephemeral clones with resyncOnOpen mode.
// In this mode, every open of the ephemeral clone db causes its
// data to be resynced with the master db.
//
TEST_F(CloudTest, EphemeralResync) {
  cloud_env_options_.keep_local_sst_files = true;
  cloud_env_options_.ephemeral_resync_on_open = true;
  options_.level0_file_num_compaction_trigger = 100; // never compact

  // Create a primary DB with two files
  OpenDB();
  std::string value;
  std::string newdb1_dbid;
  std::set<uint64_t> cloud_files;
  ASSERT_OK(db_->Put(WriteOptions(), "Name", "dhruba"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Hello2", "borthakur"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDB();
  ASSERT_EQ(2, GetSSTFiles(dbname_).size());

  // Reopen the same database in ephemeral mode by cloning the original.
  // Do not destroy the local dir. Writes to this db does not make it back
  // to any cloud storage.
  {
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("db_ephemeral", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of the first reopen
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // verify that we still have two sst files
    ASSERT_EQ(2, GetSSTFilesClone("db_ephemeral").size());

    ASSERT_OK(cloud_db->Get(ReadOptions(), "Name", &value));
    ASSERT_EQ(value, "dhruba");
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello2", &value));
    ASSERT_EQ(value, "borthakur");

    // Write one more record.
    // There should be 3 local sst files in the ephemeral db.
    ASSERT_OK(cloud_db->Put(WriteOptions(), "zip", "94087"));
    ASSERT_OK(cloud_db->Flush(FlushOptions()));
    ASSERT_EQ(3, GetSSTFilesClone("db_ephemeral").size());

    // check that cloud files did not get updated
    ASSERT_OK(GetCloudLiveFilesSrc(&cloud_files));
    ASSERT_EQ(2, cloud_files.size());
    cloud_files.clear();
  }

  // reopen main db and write two more records to it
  OpenDB();
  ASSERT_EQ(2, GetSSTFiles(dbname_).size());

  // write two more records to it.
  ASSERT_OK(db_->Put(WriteOptions(), "Key1", "onlyInMainDB"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Key2", "onlyInMainDB"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_EQ(4, GetSSTFiles(dbname_).size());
  CloseDB();
  ASSERT_OK(GetCloudLiveFilesSrc(&cloud_files));
  ASSERT_EQ(4, cloud_files.size());
  cloud_files.clear();

  // At this point, the main db has 4 files while the ephemeral
  // database has diverged earlier with 3 local files.
  // Reopen the ephemeral db with resync_on_open flag.
  // This means that earlier updates to the ephemeral db are lost.
  // It also means that the most latest updates in the master db
  // are reflected in the newly opened ephemeral database.
  {
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    std::string dbid;
    options_.info_log = nullptr;
    CreateLoggerFromOptions(clone_dir_ + "/db_ephemeral", options_, &options_.info_log);

    CloneDB("db_ephemeral", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of this clone. It should be same as before
    ASSERT_OK(cloud_db->GetDbIdentity(dbid));
    ASSERT_EQ(newdb1_dbid, dbid);

    // verify that a key written to the ephemeral db does not exist
    ASSERT_NOK(cloud_db->Get(ReadOptions(), "zip", &value));

    // verify that keys written to the main db after the ephemeral
    // was clones appear in the ephemeral db.
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Key1", &value));
    ASSERT_EQ(value, "onlyInMainDB");
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Key2", &value));
    ASSERT_EQ(value, "onlyInMainDB");
  }
}

#ifdef AWS_DO_NOT_RUN
//
// Verify that we can cache data from S3 in persistent cache.
//
TEST_F(CloudTest, PersistentCache) {
  std::string pcache = test::TmpDir() + "/persistent_cache";
  SetPersistentCache(pcache, 1);

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();

  // Reopen and validate
  OpenDB();
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
  CloseDB();
}
#endif /* AWS_DO_NOT_RUN */

}  //  namespace rocksdb

// A black-box test for the cloud wrapper around rocksdb
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else  // USE_AWS

#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr,
          "SKIPPED as DBCloud is supported only when USE_AWS is defined.\n");
  return 0;
}
#endif

#else  // ROCKSDB_LITE

#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr, "SKIPPED as DBCloud is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !ROCKSDB_LITE
