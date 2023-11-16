// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include "exec/data-sink.h"

#include "runtime/descriptors.h"

namespace impala {

class TupleRow;

class TableSinkBaseConfig : public DataSinkConfig {
 public:
  void Close() override;

  /// Expressions for computing the target partitions to which a row is written.
  std::vector<ScalarExpr*> partition_key_exprs_;

  ~TableSinkBaseConfig() override {}
};

class TableSinkBase : public DataSink {
public:
  TableSinkBase(TDataSinkId sink_id, const TableSinkBaseConfig& sink_config,
      const std::string& name, RuntimeState* state) :
      DataSink(sink_id, sink_config, name, state),
      table_id_(sink_config.tsink_->table_sink.target_table_id),
      partition_key_exprs_(sink_config.partition_key_exprs_) {}

  virtual bool is_overwrite() const { return false; }
  virtual bool is_result_sink() const { return false; }
  virtual int64_t write_id() const { return -1; }
  virtual std::string staging_dir() const { return ""; }
  virtual int skip_header_line_count() const { return 0; }
  virtual TSortingOrder::type sorting_order() const = 0;
  virtual const vector<int32_t>& sort_columns() const {
    static vector<int32_t> dummy;
    return dummy;
  }
  virtual const std::map<string, int64_t>& GetParquetBloomFilterColumns() const {
    static std::map<string, int64_t> dummy;
    return dummy;
  }

  Status Prepare(RuntimeState* state, MemTracker* parent_mem_tracker) override;
  Status Open(RuntimeState* state) override;
  void Close(RuntimeState* state) override;

  RuntimeProfile::Counter* rows_inserted_counter() { return rows_inserted_counter_; }
  RuntimeProfile::Counter* bytes_written_counter() { return bytes_written_counter_; }
  RuntimeProfile::Counter* encode_timer() { return encode_timer_; }
  RuntimeProfile::Counter* hdfs_write_timer() { return hdfs_write_timer_; }
  RuntimeProfile::Counter* compress_timer() { return compress_timer_; }

  virtual std::string DebugString() const = 0;
protected:
  /// Key is the concatenation of the evaluated dynamic_partition_key_exprs_ generated by
  /// GetHashTblKey(). Maps to an OutputPartition and a vector of indices of the rows in
  /// the current batch to insert into the partition. The PartitionPair owns the
  /// OutputPartition via a unique_ptr so that the memory is freed as soon as the
  /// PartitionPair is removed from the map. This is important, because the
  /// PartitionPairs can have different lifetimes. For example, a clustered insert into a
  /// partitioned table iterates over the partitions, so only one PartitionPairs is
  /// in the map at any given time.
  typedef std::pair<std::unique_ptr<OutputPartition>, std::vector<int32_t>> PartitionPair;
  typedef boost::unordered_map<std::string, PartitionPair> PartitionMap;

  /// Returns TRUE if the target table is transactional.
  bool IsTransactional() const { return IsHiveAcid() || IsIceberg(); }

  virtual bool IsHiveAcid() const { return false; }

  virtual Status ConstructPartitionInfo(
      const TupleRow* row,
      OutputPartition* output_partition) = 0;

  /// Initialises the filenames of a given output partition, and opens the temporary file.
  /// The partition key is derived from 'row'. If the partition will not have any rows
  /// added to it, empty_partition must be true.
  Status InitOutputPartition(RuntimeState* state,
      const HdfsPartitionDescriptor& partition_descriptor, const TupleRow* row,
      OutputPartition* output_partition, bool empty_partition) WARN_UNUSED_RESULT;

  /// Sets hdfs_file_name and tmp_hdfs_file_name of given output partition.
  /// The Hdfs directory is created from the target table's base Hdfs dir,
  /// the partition_key_names_ and the evaluated partition_key_exprs_.
  /// The Hdfs file name is the unique_id_str_.
  void BuildHdfsFileNames(const HdfsPartitionDescriptor& partition_descriptor,
      OutputPartition* output);

  /// Returns the ith partition name of the table.
  std::string GetPartitionName(int i);

  // Directory names containing partition-key values need to be UrlEncoded, in
  // particular to avoid problems when '/' is part of the key value (which might
  // occur, for example, with date strings). Hive will URL decode the value
  // transparently when Impala's frontend asks the metastore for partition key values,
  // which makes it particularly important that we use the same encoding as Hive. It's
  // also not necessary to encode the values when writing partition metadata. You can
  // check this with 'show partitions <tbl>' in Hive, followed by a select from a
  // decoded partition key value.
  std::string UrlEncodePartitionValue(const std::string& raw_str);

  /// Add a temporary file to an output partition.  Files are created in a
  /// temporary directory and then moved to the real partition directory by the
  /// coordinator in a finalization step. The temporary file's current location
  /// and final destination are recorded in the state parameter.
  /// If this function fails, the tmp file is cleaned up.
  Status CreateNewTmpFile(RuntimeState* state, OutputPartition* output_partition)
      WARN_UNUSED_RESULT;

  /// Updates runtime stats of HDFS with rows written, then closes the file associated
  /// with the partition by calling ClosePartitionFile()
  Status FinalizePartitionFile(RuntimeState* state, OutputPartition* partition)
      WARN_UNUSED_RESULT;

  /// Writes all rows referenced by the row index vector in 'partition_pair' to the
  /// partition's writer and clears the row index vector afterwards.
  Status WriteRowsToPartition(
      RuntimeState* state, RowBatch* batch, PartitionPair* partition_pair)
      WARN_UNUSED_RESULT;

  /// Closes the hdfs file for this partition as well as the writer.
  Status ClosePartitionFile(RuntimeState* state, OutputPartition* partition)
      WARN_UNUSED_RESULT;

  // Returns TRUE if the staging step should be skipped for this partition. This allows
  // for faster INSERT query completion time for the S3A filesystem as the coordinator
  // does not have to copy the file(s) from the staging locaiton to the final location. We
  // do not skip for INSERT OVERWRITEs because the coordinator will delete all files in
  // the final location before moving the staged files there, so we cannot write directly
  // to the final location and need to write to the temporary staging location.
  bool ShouldSkipStaging(RuntimeState* state, OutputPartition* partition);

  /// Returns TRUE for Iceberg tables.
  bool IsIceberg() const { return table_desc_->IsIcebergTable(); }

  /// Returns TRUE if an external output directory was provided.
  bool HasExternalOutputDir() { return !external_output_dir_.empty(); }

  /// Generates string key for hash_tbl_ as a concatenation of all evaluated exprs,
  /// evaluated against 'row'. The generated string is much shorter than the full Hdfs
  /// file name.
  void GetHashTblKey(const TupleRow* row,
      const std::vector<ScalarExprEvaluator*>& evals, std::string* key);

  /// Table id resolved in Prepare() to set tuple_desc_;
  TableId table_id_;

  /// string representation of the unique fragment instance id. Used for per-partition
  /// Hdfs file names, and for tmp Hdfs directories. Set in Prepare();
  std::string unique_id_str_;

  /// Descriptor of target table. Set in Prepare().
  const HdfsTableDescriptor* table_desc_ = nullptr;

  /// The partition descriptor used when creating new partitions from this sink.
  /// Currently we don't support multi-format sinks.
  const HdfsPartitionDescriptor* prototype_partition_;

  /// Expressions for computing the target partitions to which a row is written.
  const std::vector<ScalarExpr*>& partition_key_exprs_;
  std::vector<ScalarExprEvaluator*> partition_key_expr_evals_;

  /// Subset of partition_key_expr_evals_ which are not constant. Set in Prepare().
  /// Used for generating the string key of hash_tbl_.
  std::vector<ScalarExprEvaluator*> dynamic_partition_key_expr_evals_;

  /// Stores the current partition during clustered inserts across subsequent row batches.
  /// Only set if 'input_is_clustered_' is true.
  PartitionPair* current_clustered_partition_ = nullptr;

  /// Stores the current partition key during clustered inserts across subsequent row
  /// batches. Only set if 'input_is_clustered_' is true.
  std::string current_clustered_partition_key_;

  /// The directory in which an external FE expects results to be written to.
  std::string external_output_dir_;

  RuntimeProfile::Counter* partitions_created_counter_;
  RuntimeProfile::Counter* files_created_counter_;
  RuntimeProfile::Counter* rows_inserted_counter_;
  RuntimeProfile::Counter* bytes_written_counter_;

  /// Time spent converting tuple to on disk format.
  RuntimeProfile::Counter* encode_timer_;
  /// Time spent writing to hdfs
  RuntimeProfile::Counter* hdfs_write_timer_;
  /// Time spent compressing data
  RuntimeProfile::Counter* compress_timer_;
};

}
