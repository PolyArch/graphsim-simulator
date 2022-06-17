#ifndef _MULTIPLY_H
#define _MULTIPLY_H

#include "asic_core.hh"
#include "common.hh"

class asic_core;
class asic;
class network;
class multiply;
class completion_buffer;

// TODO: may be extended to all dense implementations?
class multiply {
  friend class asic;
  friend class completion_buffer;

  public:
  multiply(asic* host);
  int dispatch(int dfg_id, vector<int> rows, vector<int> weight_rows_done, vector<bool> second_buffer);
  void push_lsq_data(pref_tuple cur_tuple);
  void push_scratch_data(int dfg_id, pref_tuple cur_tuple);
  void push_scratch_data_to_pipeline(int dfg_id);
  void process(int dfg_id);
  void reduce(int dfg_id);
  bool cycle(int phase);
  bool pipeline_inactive(bool show);
  bool can_push_local_coarse_grain_task(int dfg_id);
  bool can_push_in_process(int dfg_id);
  bool can_push_in_scratch_process(int dfg_id);
  bool can_push_in_process_reduce(int dfg_id);
  void serve_miss_task_queue(int dfg_id);
  int serve_hats_requests();
  int serve_local_hats_requests();
  int serve_local_coalescer_requests();
  int get_live_tasks_per_dfg(int dfg_id);
  int get_updated_insert_cycle(int dfg_id);
  int get_free_dfg_id(); // for load imbalance

  public:

    // Specialized task scheduling hardware for dynamic batching and better cache hit rate 
    // Queue1: that holds pointers in the priority queue (serve in the priority order)
    // assert second value should be less than batched size; first value can be according to batch size, cache-hit (hopefully few levels)
    // its size should be limited; if update come in -- it will lookup and if available and valid bit set, push. Otherwise, push a new entry.
    // If full, send the tasks in the overflow buffer 

    int _stalled=0;
    int _prev_priority=0;

    // for temporal multicast
    int *_lane_blocked_until; // can also serve other cores
    // for spatial multicast -- just check the mcast available
    // int _blocked_until[MAX_DFGS];

    int _mult_fifo_len=FIFO_DEP_LEN;
    int _phase_cycle=0;

    int _stat_exp_bw_save=0;
    int _num_wgt_reqs_really_sent=0;
    int _stat_vec_mat_local[MAX_DFGS]; // =0;
    // uint64_t _last_push_cycle[MAX_DFGS]; // should be incremented by 1/coarse_alloc
    float _last_push_cycle[MAX_DFGS]; // should be incremented by 1/coarse_alloc
    // TODO: declare pipeline buffers
    queue<mult_data> _prefetch_lsq; // this type is all the input data required..so should be new for each dfg
    queue<mult_data> _lsq_process[MAX_DFGS];
    queue<mult_data> _scratch_process[MAX_DFGS];
    queue<mult_data> _process_reduce[MAX_DFGS];
    // queue<int> _cgra_output;
    // assumes that each task is completed in a VEC_LEN*x times. That x should be a dimension of weight?
    const int vid_done_cycle = FEAT_LEN*FEAT_LEN/(bus_width/message_size);
    int *_prev_vid;
    vector<DTYPE> _weight[FEAT_LEN]; // data
    queue<mult_task> _local_coarse_task_queue;
    mult_task _pending_task[MAX_DFGS]; // so this will be served first if not empty
    queue<mult_task> _local_miss_task_queue;

    const int _dot_unit = FEAT_LEN;
    const int num_cache_lines_per_weight = ceil(_dot_unit/float((line_size/message_size))); // single feat_len
    const int num_cache_lines_per_leaf = (NUM_DATA_PER_LEAF*message_size)/line_size;
    int _core_id=0;

    const int single_dfg_gran = line_size/message_size;
    int _data_parallel_throughput=1;
    int _last_dfg_id=0;
    int _dfg_latency=DFG_LENGTH;
    int _multicast_batch_size=MULTICAST_BATCH;

    asic *_asic;
};

#endif
