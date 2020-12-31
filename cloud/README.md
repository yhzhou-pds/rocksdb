## RocksDB-Cloud on Amazon Web Services (AWS)

This directory contains the extensions needed to make rocksdb store
files in AWS environment.

### Example
Here is an [example](https://github.com/rockset/rocksdb-cloud/blob/master/cloud/examples/cloud_durable_example.cc)  of code that uses rocksdb-cloud. The Makefile in that directory shows how you can link your application with the rocksdb-cloud library.

### Compile
The compilation process assumes that the AWS c++ SDK is installed in
the default location of /usr/local. You can follow the steps listed
here https://github.com/aws/aws-sdk-cpp to install the c++ AWS sdk.

If you want to compile rocksdb with AWS support, please set the following
environment variable USE_AWS=1. 

If you want to compile rocksdb so that the write ahead log is stored
in Kafka, then set environment variable USE_KAFKA=1. You have to use
the C++ kafka client by downloading and installing the code from
https://github.com/edenhill/librdkafka.

Then run

   make clean all db_bench

This will create the libraries that you can link into your application.

The cloud unit tests need a AWS S3 bucket to store files. Please set the
following environment variables to run the cloud unit tests:

AWS_ACCESS_KEY_ID     : your aws access credentials

AWS_SECRET_ACCESS_KEY : your secret key

AWS_DEFAULT_REGION : the AWS region of your client (e.g. us-west-2)

### Run Unit Tests

make check J=1

### Measure Performance
To run dbbench,
   db_bench --env_uri="s3://" --aws_access_id=xxx and --aws_secret_key=yyy
This will create files in a bucket named rockset.dbbench.$USER where $USER is the name of the user who is running the benchmark.



