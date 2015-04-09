/**
 * PRIVATE HEADER
 *
 * Data structures for main memory storage.
 *
 * Copyright (c) 2013 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */


#pragma once
#include "page.h"
#include "cursor.h"

#include <tuple>
#include <vector>
#include <algorithm>
#include <memory>
#include <mutex>

namespace Akumuli {

struct TimeSeriesValue {

    // Type definitions
    struct Blob {
        aku_EntryOffset value;
        uint32_t value_length;
    };

    enum ValueType {
        BLOB,
        DOUBLE,
    };

    // Data members
    aku_Timestamp                           key_ts_;  // Key value (time)
    aku_ParamId                             key_id_;  // Key value (id)
    ValueType                               type_;    // Payload type
    union {
            Blob                            blob;     // Binary payload
            double                          value;    // Numeric payload
    } payload;
    // NOTE: this structure can be packed better without tuple.
    // Tuple needed only for comparison and can be created on the stack.

    TimeSeriesValue();

    TimeSeriesValue(aku_Timestamp ts, aku_ParamId id, aku_EntryOffset offset, uint32_t value_length);

    TimeSeriesValue(aku_Timestamp ts, aku_ParamId id, double value);

    aku_Timestamp get_timestamp() const;

    aku_ParamId get_paramid() const;

    CursorResult to_result(const PageHeader *page) const;

    void add_to_header(ChunkHeader *chunk_header) const;

    bool is_blob() const;

    friend bool operator < (TimeSeriesValue const& lhs, TimeSeriesValue const& rhs);

    //! Chunk order less then operator (id goes first, then goes timestamp)
    friend bool chunk_order_LT (TimeSeriesValue const& lhs, TimeSeriesValue const& rhs);

} __attribute__((packed));


/** Time-series sequencer.
  * @brief Akumuli can accept unordered time-series (this is the case when
  * clocks of the different time-series sources are slightly out of sync).
  * This component accepts all of them, filter out late writes and reorder
  * all the remaining samples by timestamp and parameter id.
  */
struct Sequencer {
    typedef std::vector<TimeSeriesValue> SortedRun;
    typedef std::shared_ptr<SortedRun>   PSortedRun;
    typedef std::mutex                   Mutex;
    typedef std::unique_lock<Mutex>      Lock;

    static const int RUN_LOCK_MAX_BACKOFF = 0x100;
    static const int RUN_LOCK_BUSY_COUNT = 0xFFF;
    static const int RUN_LOCK_FLAGS_MASK = 0x0FF;
    static const int RUN_LOCK_FLAGS_SIZE = 0x100;

    std::vector<PSortedRun>      runs_;           //< Active sorted runs
    std::vector<PSortedRun>      ready_;          //< Ready to merge
    PSortedRun                   key_;
    const aku_Duration           window_size_;
    const PageHeader* const      page_;
    aku_Timestamp                top_timestamp_;  //< Largest timestamp ever seen
    uint32_t                     checkpoint_;     //< Last checkpoint timestamp
    mutable std::atomic_int      sequence_number_;   //< Flag indicates that merge operation is in progress and
                                                  //< search will return inaccurate results.
                                                  //< If progress_flag_ is odd - merge is in progress if it is
                                                  //< even - there is no merge and search will work correctly.
    mutable Mutex                runs_resize_lock_;
    mutable std::vector<RWLock>  run_locks_;
    uint32_t                     space_estimate_; //< Space estimate for storing all data
    const size_t                 c_threshold_;    //< Compression threshold

    Sequencer(PageHeader const* page, aku_Config config);

    /** Add new sample to sequence.
      * @brief Timestamp of the sample can be out of order.
      * @returns error code and flag that indicates whether of not new checkpoint is createf
      */
    std::tuple<int, int> add(TimeSeriesValue const& value);

    //! Simple merge and sync without compression. (depricated)
    void merge(Caller& caller, InternalCursor* cur);

    /** Merge all values (ts, id, offset, length)
      * and write it to target page.
      * caller and cur parameters used for communication with storage (error reporting).
      */
    aku_Status merge_and_compress(PageHeader* target);

    //! Close cache for writing, merge everything to page header.
    aku_Status close(PageHeader* target);

    /** Reset sequencer.
      * All runs are ready for merging.
      * @returns new sequence number.
      */
    int reset();

    /** Search in sequencer data.
      * @param caller represents caller
      * @param cur search cursor
      * @param query represents search query
      * @param sequence_number sequence number obtained with get_window function
      * @note search method follows common pattern used by all methods except sequence_number
      * parameter. This parameter is used to organize optimistic concurrency control. User must
      * call get_window fn and get current window and seq-number. This seq-number then passed to
      * search method. If seq-number is changed between calls to get_window and search - search
      * will be aborted and AKU_EBUSY.error code will be returned If merge occures during search -
      * search will be aborted and AKU_EBUSY error code will be returned.
      */
    void search(Caller& caller, InternalCursor* cur, SearchQuery query, int sequence_number) const;

    std::tuple<aku_Timestamp, int> get_window() const;

    /** Returns number of bytes needed to store all data from the checkpoint
     *  in compressed mode. This number can be more than actually needed but
     *  can't be less (only overshoot is ok, undershoot is error).
     */
    uint32_t get_space_estimate() const;

private:
    //! Checkpoint id = ⌊timestamp/window_size⌋
    uint32_t get_checkpoint_(aku_Timestamp ts) const;

    //! Convert checkpoint id to timestamp
    aku_Timestamp get_timestamp_(uint32_t cp) const;

    // move sorted runs to ready_ collection
    int make_checkpoint_(uint32_t new_checkpoint);

    /** Check timestamp and make checkpoint if timestamp is large enough.
      * @returns error code and flag that indicates whether or not new checkpoint is created
      */
    std::tuple<int, int> check_timestamp_(aku_Timestamp ts);

    void filter(PSortedRun run, SearchQuery const& q, std::vector<PSortedRun> *results) const;
};
}
