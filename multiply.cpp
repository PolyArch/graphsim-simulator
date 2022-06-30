#include "asic_core.hh"
#include "network.hh"
#include "multiply.hh"
#include "common.hh"
#include "asic.hh"

// waiting_addr means how many input are we waiting for?
multiply::multiply(asic *host) : _asic(host)
{
  if (_asic->_config->_algo == gcn)
  {
    for (int i = 0; i < FEAT_LEN; ++i)
    {
      for (int j = 0; j < FEAT_LEN; ++j)
      {
        _weight[i].push_back(rand());
      }
    }
  }
  _lane_blocked_until = (int *)malloc(MAX_DFGS * sizeof(int));
  _prev_vid = (int *)malloc(MAX_DFGS * sizeof(int));
  for (int i = 0; i < MAX_DFGS; ++i)
  {
    _stat_vec_mat_local[i] = 0;
    _last_push_cycle[i] = 0;
    _pending_task[i].weight_rows_done = 0;
    _lane_blocked_until[i] = 0;
    // _process_reduce[i].clear();
  }
}

int multiply::get_live_tasks_per_dfg(int dfg_id)
{
  int tot_live_tasks = 0;
  tot_live_tasks += _lsq_process[dfg_id].size();
  tot_live_tasks += _process_reduce[dfg_id].size();
  tot_live_tasks += _scratch_process[dfg_id].size();
  // FIXME: this doesn't work...!!!
  // tot_live_tasks += _local_coarse_task_queue.size();
  tot_live_tasks += _prefetch_lsq.size();
  /*if(_asic->_cur_cycle==6000) {
    // scratch_process is too filled up
    cout << "Core: " << _core_id << " tot_live_tasks: " << tot_live_tasks << endl;
    cout << " lsq_process: " << _lsq_process[dfg_id].size() << " process_reduce: " << _process_reduce[dfg_id].size()
    << " local_coarse: " << _local_coarse_task_queue.size() << " prefetch_lsq: " << _prefetch_lsq.size()
    << " scratch_process: " << _scratch_process[dfg_id].size() << endl;
  }*/
  return tot_live_tasks;
}

// TODO: Apply completion buffer entries bottleneck..!!! (this task should be
// split in multiple smaller tasks...)
int multiply::dispatch(int dfg_id, vector<int> rows, vector<int> weight_rows_done, vector<bool> second_buffer)
{ // prefetch
  /*if(_core_id==0) {
    cout << "Issuing mult tasks: " << rows.size() << " at core: " << _core_id << " and cycle: " << _asic->_cur_cycle << endl;
  }*/

  // just send a single broadcast and rows
  uint64_t line_addr = _asic->_mem_ctrl->_weight_offset;
  pref_tuple cur_tuple;
  cur_tuple.src_id = rows[0];

#if 1
  /*
  int row = cur_tuple.src_id;
  mult_data next_tuple;
  for(int i=0; i<FEAT_LEN; ++i) {
    next_tuple.vec1.push_back(_weight[i][i]);
    next_tuple.vec2.push_back(_asic->_scratch_vec[i][row]); // this should be based on next_tuple.src_id
  }
  next_tuple.insert_cycle=_asic->_cur_cycle;
  _lsq_process.push(next_tuple);
  */

  // send a remote scratch read.. (low latency but does not solve the bandwidth
  // problem)

  // TODO: change the loop to start with weight rows done!!!
  // this is loading FEAT_LEN*FEAT_LEN (and in each cycle, it works on 1 cache line)
  // Ok so FEAT_LEN requests for FEAT_LEN sent...(for their reuse, they should be duplicate)
  // TODO: for gcn, it should access weights for lines=feat_len*feat_len and VID for scratch read (let me ignore the second point)

  cur_tuple.end_edge_served = rows.size();
  int i = 0;

  cur_tuple.req_core = _core_id;
  cur_tuple.core_type = coarseScratch; // cannot handle two types of these requests?
  cur_tuple.second_buffer = second_buffer[0];
  cur_tuple.src_id = rows[0]; // FIXME: if only one is served per cycle
  cur_tuple.dfg_id = dfg_id;
  if (_asic->_config->_algo == gcn)
  {
    for (i = weight_rows_done[0]; i < FEAT_LEN && _asic->_coarse_reorder_buf[_core_id][dfg_id]->can_push(num_cache_lines_per_weight); ++i)
    {
      assert(_asic->_coarse_reorder_buf[_core_id][dfg_id]->can_push(num_cache_lines_per_weight) && "sufficient entries should be available");
      cur_tuple.src_dist = i;
      cur_tuple.edge.dst_id = 0; // for weight address..
      auto rob = _asic->_coarse_reorder_buf[_core_id][dfg_id];
      for (int c = 0; c < num_cache_lines_per_weight; ++c)
      {

        ////////////////////////////////////////////////
        if (_asic->_config->_hats == 0 || _asic->_config->_inter_task_reorder == 0)
        {
          cur_tuple.cb_entry = _asic->_coarse_reorder_buf[_core_id][dfg_id]->allocate_cb_entry(1);
        }
        else
        {
          if (i == 0 && c == 0)
          { // if first
            int first = 0;
            first = rob->_free_cb_parts.front().first;
            assert(rob->_free_cb_parts.front().second == false && "cannot assign new entry for already pending free cb parts");
            rob->_free_cb_parts.front().second = true;
          }
          cur_tuple.cb_entry = rob->_free_cb_parts.front().first + (i * num_cache_lines_per_weight) + c;

          assert(rob->_free_cb_parts.front().second == true && "should be the pending one");
          cur_tuple.cb_entry = cur_tuple.cb_entry % COMPLETION_BUFFER_SIZE;
          if (i == FEAT_LEN - 1 && c == num_cache_lines_per_weight - 1)
          {
            rob->_free_cb_parts.pop(); // nobody else should use it...
            rob->_reorder_buf[cur_tuple.cb_entry].last_entry = true;
          }
          assert(rob->_reorder_buf[cur_tuple.cb_entry].waiting_addr == -1 && "cb should have default waiting addr");
          assert(!rob->_reorder_buf[cur_tuple.cb_entry].valid && "cb should be free while allocating");
          rob->_reorder_buf[cur_tuple.cb_entry].waiting_addr = 1;
          rob->_entries_remaining--;
        }

        // set this to the original core..
        rob->_reorder_buf[cur_tuple.cb_entry].cur_tuple.second_buffer = cur_tuple.second_buffer;
        rob->_reorder_buf[cur_tuple.cb_entry].cur_tuple.src_id = cur_tuple.src_id;

        ///////////////////////////////////////////

        line_addr += line_size; // TODO: not used anywhere...
#if LOCAL_HATS == 0
        _asic->_scratch_ctrl->send_scratch_request(_core_id, i, cur_tuple);
#else
        _asic->_local_coalescer[_core_id]->insert_leaf_task(i, _core_id, cur_tuple.cb_entry, dfg_id);
#endif
        _num_wgt_reqs_really_sent++;
      }
    }
    return i;
    if (i != FEAT_LEN)
    {
      // re-push this task if not enough completion buffer entries were not push (it should be pushed at front) -- FIXME
      for (unsigned r = 0; r < rows.size(); ++r)
      {
        mult_task pending_task;
        _asic->_task_ctrl->insert_local_coarse_grained_task(_core_id, rows[r], i, second_buffer[r]);
      }
    }
  }
  // TODO: for kNN, it should access data for vid=L from cache/memory and Query data for scratch read (let me ignore this)
  if (_asic->_config->_domain == tree)
  {
    uint64_t line_addr = _asic->_mem_ctrl->_weight_offset + (rows[0] * num_cache_lines_per_leaf * line_size);

    if (_asic->_config->_mult_cache_hit_aware_sched == 1)
    {
      assert(_asic->_config->_hats == 0 && "currently we have not implemented hats with cache-hit optimization");

      for (int i = 0; i < num_cache_lines_per_leaf; ++i)
      {
        bool hit = _asic->_mem_ctrl->is_cache_hit(_core_id, line_addr, ACCESS_LOAD, true, 0);
        // cout << "Trying to access address: " << line_addr << endl;
        if (hit)
        {
          ++_asic->_mem_ctrl->_l2accesses;
          ++_asic->_mem_ctrl->_l2hits;
          // cout << "it was hit\n";
          mult_data next_tuple;
          next_tuple.insert_cycle = get_updated_insert_cycle(dfg_id);

          next_tuple.row = cur_tuple.src_id;
          next_tuple.second_buffer = cur_tuple.second_buffer;
          _lsq_process[dfg_id].push(next_tuple);
          // cout << "Hit request for addr: " << line_addr << " and vid: " << cur_tuple.src_id << endl;
        }
        else
        {
          if ((!_asic->_config->_update_coalesce) || (_asic->_config->_update_coalesce && _asic->_task_ctrl->_present_in_miss_gcn_queue[rows[0]] == 0))
          {
            // cout << "it was miss\n";
            mult_task same_task;
            same_task.row = rows[0];
            same_task.weight_rows_done = i;
            _local_miss_task_queue.push(same_task); // check it each cycle..
          }
          else
          {
            ++_asic->_mem_ctrl->_l2hits;
            ++_asic->_mem_ctrl->_l2accesses;
            ++_asic->_mem_ctrl->_l2coalesces;
          }
          if (_asic->_config->_update_coalesce)
          {
            ++_asic->_task_ctrl->_present_in_miss_gcn_queue[rows[0]];
          }
        }
        line_addr += line_size;
      }
    }
    else if (_asic->_config->_hats == 1)
    {
      // cout << "Requested for vid: " << rows[0] << " at a core: " << _core_id << " and cycle: " << _asic->_cur_cycle << endl;
      /*if(_asic->_leaf_access_count[rows[0]]==BATCH_WIDTH) { // or keep it 4; but temporal also saves or no?

      } else {
        ++_asic->_leaf_access_count[rows[0]];
      }*/
      if (_asic->_config->_central_batch)
      {
        ++_asic->_leaf_access_count[rows[0]];
        ++_asic->_pending_hats_requests;
      }
      else
      {
        _asic->_remote_coalescer[_core_id]->insert_leaf_task(rows[0], -1, -1, -1);
      }
      // cout << "Update hats bitvector for vid: " << rows[0] << endl;
    }
    else
    {
      // cur_tuple.cb_entry = _asic->_coarse_compl_buf[_core_id]->allocate_cb_entry(num_cache_lines_per_leaf);
      cur_tuple.core_type = coarseMem;
      cur_tuple.req_core = _core_id;
      for (int c = 0; c < num_cache_lines_per_leaf; ++c)
      {
        cur_tuple.cb_entry = _asic->_coarse_compl_buf[_core_id][dfg_id]->allocate_cb_entry(1); // num_cache_lines_per_leaf);
        // cout << "Sent request for addr: " << line_addr << " and vid: " << cur_tuple.src_id << endl;
        _asic->_mem_ctrl->send_mem_request(0, line_addr, cur_tuple, WEIGHT_SID);
        _num_wgt_reqs_really_sent++;
        line_addr += line_size;
      }
    }
  }

#else
  // FIXME: make it in a loop until there are entries. In this case, do not pop
  // from the task queue. Or push it again...
  cur_tuple.start_edge_served = rows.size();
  int num_cache_lines_per_weight = _dot_unit / (line_size / message_size); // single feat_len
  int i = 0;
  // TODO: change the loop to start with weight rows done!!!
  for (i = weight_rows_done[0]; i < FEAT_LEN && _asic->_coarse_compl_buf[_core_id]->can_push(); ++i)
  {
    cur_tuple.cb_entry = _asic->_coarse_compl_buf[_core_id]->allocate_cb_entry(num_cache_lines_per_weight);
    cur_tuple.req_core = _core_id;
    cur_tuple.core_type = coarseScratch;
    cur_tuple.end_edge_served = i;
    for (int c = 0; c < num_cache_lines_per_weight; ++c)
    {
      line_addr += (c * line_size);
      // cout << "Sent wgt matrix: " << line_addr << endl;
      bool sent_request = _asic->_mem_ctrl->send_mem_request(0, line_addr, cur_tuple, WEIGHT_SID);
      if (sent_request)
      {
        _num_wgt_reqs_really_sent++;
      }
    }
  }
  if (i != FEAT_LEN)
  {
    // re-push this task
    for (unsigned r = 0; r < rows.size(); ++r)
    {
      mult_task pending_task;
      _asic->_task_ctrl->insert_local_coarse_grained_task(_core_id, rows[r], i);
    }
  }
#endif
  return 0;
}

// This should be done after collecting some updates I think
/*
Opt1: Spatial/temporal reuse (MULTICAST_BATCH, data_parallel_thr) inside data-parallel tasks (temporal for better balance but spatial when less tasks? Varying vector size dynamically would be too complicated)
Point 1: skip some elements that are not enough to multicast (delay to find that)
Point 2: we cannot provision to a very large batch size (will be a waste for different configurations; impractical to utilize)
*/
// Opt2: merging with cache-hit-aware (prioritize these batches based on for whether they are hits instead of serial)
// Let me first find a scenario with a much larger difference

int multiply::get_free_dfg_id()
{
  // cur_tuple.dfg_id = (_last_dfg_id+1)%_data_parallel_throughput;
  // _last_dfg_id = (_last_dfg_id+1)%_data_parallel_throughput;

  // TODO: block for temporal serialization; chose one of them for load balance among vector lanes -- allocating to the one with more free entries (if same, just rand??)
  int larger_entry = 0;
  /*int free_entry1 = _asic->_coarse_compl_buf[_core_id][0]->_entries_remaining;
  if(_lane_blocked_until[0]<_asic->_cur_cycle) {
    larger_entry = free_entry1;
  }*/
  int dfg_id = rand() % _data_parallel_throughput; // TODO: does it always prioritize dfg_id=0?
  int chosen_cb_entry = 0;
  // for(int d=0; d<_data_parallel_throughput; ++d) {
  for (int d = dfg_id, i = 0; i < _data_parallel_throughput; ++i, d = (d + 1) % _data_parallel_throughput)
  {
    if (_lane_blocked_until[d] < _asic->_cur_cycle)
    {
      int free_entry2 = _asic->_coarse_compl_buf[_core_id][d]->_entries_remaining; // + 4*_mult_fifo_len - get_live_tasks_per_dfg(d);
      if (free_entry2 > larger_entry)
      {
        chosen_cb_entry = _asic->_coarse_compl_buf[_core_id][d]->_entries_remaining;
        dfg_id = d;
        larger_entry = free_entry2;
      }
    }
  }

  // cur_tuple.dfg_id = rand()%MAX_DFGS;
  // if(_blocked_until[cur_tuple.dfg_id] > _asic->_cur_cycle) return 0;
  // larger_entry = _asic->_coarse_compl_buf[_core_id][cur_tuple.dfg_id]->_entries_remaining;

  // if(larger_entry < num_cache_lines_per_leaf*_multicast_batch_size) {
  if (_asic->_config->_domain == tree && chosen_cb_entry < num_cache_lines_per_leaf)
  {
    return -1;
  }
  else
  {
    return dfg_id;
  }
}

// TODO: No checks, just need to send scratch read requests in
// send_batched_read_response()
int multiply::serve_local_coalescer_requests()
{
  if (_asic->_local_coalescer[_core_id]->_entries_in_priority_update == 0)
  { // if empty..
    return 0;
  }
  float batch_size = 0;
  if (_asic->_local_coalescer[_core_id]->_entries_in_priority_update > 0)
  {
    batch_size = _asic->_local_coalescer[_core_id]->get_avg_batch_size();
  }
  const int hrst_stall = 0; // 50;
  ++_stalled;
  if (_stalled == hrst_stall)
    _stalled = -hrst_stall;
  if (((_asic->_cur_cycle - _phase_cycle) < LSQ_WAIT || (batch_size < _asic->_local_coalescer[_core_id]->_multicast_batch_size / 2)) && _stalled > 0 && _stalled < hrst_stall)
  { // there should be some delay somewhere; maybe for total batching available?
    ++_asic->_stats->_stat_delayed_batch_cycles;
    return 0;
  }

  pref_tuple cur_tuple;
  int count = 0;

  // No need to pop, just want to shift the serving pointer..
  // pair<int, int> temp = _asic->_local_coalescer[_core_id]->peek();
  pair<int, int> temp = _asic->_local_coalescer[_core_id]->pop();
  if (temp.first == -1 && temp.second == -1)
    return 0;
  int leaf_id = temp.first, index_into_batched = temp.second;

  assert(_asic->_config->_algo == gcn && "currently we do not need a local read for no-gcn");
  _asic->_local_coalescer[_core_id]->send_batched_read_requests(leaf_id, index_into_batched);
  return 0;
}

int multiply::serve_local_hats_requests()
{
  // cout << "Cycle: " << _asic->_cur_cycle << " outstanding memory size: " << _asic->_mem_ctrl->_outstanding_mem_req.size() << endl;
  if (_asic->_remote_coalescer[_core_id]->_entries_in_priority_update == 0)
  { // if empty..
    /*
    if(_asic->_cur_cycle-_phase_cycle>LSQ_WAIT && (batch_size>_batch_width*0.3)) {
      cout << "Cycle: " << _asic->_cur_cycle << " core: " << _core_id << " stalled due to empty task queue\n";
    }
    ++_asic->_stats->_stat_stall_thr[_core_id];
    */
    return 0;
  }
  float batch_size = 0;
  if (_asic->_remote_coalescer[_core_id]->_entries_in_priority_update > 0)
  {
    batch_size = _asic->_remote_coalescer[_core_id]->get_avg_batch_size();
  }
  const int hrst_stall = 0; // 50;
  ++_stalled;
  if (_stalled == hrst_stall)
    _stalled = -hrst_stall;
  // minimum phase cycle = 250
  // maximum stall cycle = 500
  // cout << "batch size: " << batch_size << " batch width: " << BATCH_WIDTH << " stalled: " << _stalled << " cycle: " << _asic->_cur_cycle << endl;
  if (((_asic->_cur_cycle - _phase_cycle) < LSQ_WAIT || (batch_size < _asic->_remote_coalescer[_core_id]->_multicast_batch_size / 2)) && _stalled > 0 && _stalled < hrst_stall)
  { // there should be some delay somewhere; maybe for total batching available?
    /*if(_asic->_cur_cycle>LSQ_WAIT) {
      cout << "Cycle: " << _asic->_cur_cycle << " core: " << _core_id << " stalled due to waiting on lsq\n";
    }*/
    ++_asic->_stats->_stat_delayed_batch_cycles;
    return 0;
  }

  if (_asic->_config->_domain == tree && _asic->_mem_ctrl->_outstanding_mem_req.size() > 256)
  {
    /*if(_asic->_cur_cycle>LSQ_WAIT) {
      cout << "Cycle: " << _asic->_cur_cycle << " core: " << _core_id << " stalled due to full outstanding memory request\n";
    }*/
    return 0;
  }

  pref_tuple cur_tuple;

  cur_tuple.dfg_id = get_free_dfg_id();
  if (cur_tuple.dfg_id == -1)
  {
    ++_asic->_stats->_stat_mem_stall_thr[_core_id];
    /*if(_asic->_cur_cycle>LSQ_WAIT) {
      cout << "Cycle: " << _asic->_cur_cycle << " core: " << _core_id << " stalled due to insufficient completion buffer entries\n";
    }*/
    return 0;
  }

  if (_asic->_config->_domain == tree)
  {
    assert(_asic->_coarse_compl_buf[_core_id][cur_tuple.dfg_id]->can_push(num_cache_lines_per_leaf) && "can only dispatch with sufficient cache lines");
  }

  int count = 0;

  pair<int, int> temp = _asic->_remote_coalescer[_core_id]->pop();
  int leaf_id = temp.first, index_into_batched = temp.second;
  // _is_leaf_valid.set(leaf_id);

  // TODO: it should index at an offset by the core...
  // cout << "Issuing at cycle: " << _asic->_cur_cycle << " core: " << _core_id << " dfg_id: " << cur_tuple.dfg_id
  // << " number of replications: " << _num_batched_tasks_per_index[index_into_batched]  << " and leaf id: " << leaf_id << endl;
  if (_asic->_config->_domain == tree)
  { // send with temporal reuse
    _asic->_remote_coalescer[_core_id]->send_batched_compute_request(leaf_id, cur_tuple, index_into_batched);
  }
  else
  { // batch and reduce only a pack, send a multicast scratch packet that will push to scratch write
    assert(_asic->_config->_algo == gcn && "currently we do not need a remote read for no-gcn");
    // assert(_asic->_remote_coalescer[_core_id]->_num_batched_tasks_per_index[index_into_batched]<=_batch_width && "cannot have more mcast than allowed");
    _asic->_remote_coalescer[_core_id]->send_batched_read_response(index_into_batched);
  }
  return _multicast_batch_size;
}

// TODO: for cache miss, push to memory on a condition otherwise push to the miss queue that will be handled later, in parallel allocate a single completion buffer entry for a single miss request...
void multiply::serve_miss_task_queue(int dfg_id)
{
  if (!_local_miss_task_queue.empty() && _lsq_process[dfg_id].size() < LSQ_WAIT && _asic->_coarse_compl_buf[_core_id][dfg_id]->_entries_remaining > 0)
  {
    mult_task same_task = _local_miss_task_queue.front();
    uint64_t line_addr = _asic->_mem_ctrl->_weight_offset + (same_task.row * NUM_DATA_PER_LEAF * message_size);
    line_addr += (same_task.weight_rows_done * line_size);

    pref_tuple cur_tuple;
    cur_tuple.end_edge_served = 1;

    cur_tuple.req_core = _core_id;
    cur_tuple.core_type = coarseMem;
    cur_tuple.second_buffer = false;
    cur_tuple.src_id = same_task.row;
    cur_tuple.cb_entry = _asic->_coarse_compl_buf[_core_id][dfg_id]->allocate_cb_entry(1);

    // cout << "Miss request for addr: " << line_addr << " and vid: " << cur_tuple.src_id << endl;
    bool sent_request = _asic->_mem_ctrl->send_mem_request(0, line_addr, cur_tuple, WEIGHT_SID);
    if (_asic->_config->_update_coalesce)
    {
      assert(sent_request && "should not require coalesce for a missed request");
    }
    _local_miss_task_queue.pop();
  }
}

void multiply::push_scratch_data(int dfg_id, pref_tuple cur_tuple)
{              // consume the loaded values
  int row = 0; // cur_tuple.arob_entry; (for now, this doesn't work with scratch)
  assert(row < V && "even though we are not loading vector currently, it should be less than V");
  // cout << "Rows size: " << cur_tuple.entry_cycle << endl;

  // Oh the number of packets it correspond to
  // FIXME: things about this entry cycle...
  // for(int c=0; c<cur_tuple.entry_cycle; ++c) {
  for (int c = 0; c < 1; ++c)
  { // number of cache lines... (why entry cycle: need better names)
    mult_data next_tuple;
    // No need to push original data when we do use the result
    /*if(_asic->_config->_algo==gcn) {
      for(int i=0; i<FEAT_LEN; ++i) { // 16 stages..instead of 4
        next_tuple.vec1.push_back(_weight[i][i]);
        next_tuple.vec2.push_back(_asic->_scratch_vec[i][row]);
      }
    }*/
    next_tuple.row = cur_tuple.src_id;
    next_tuple.second_buffer = cur_tuple.second_buffer;
    next_tuple.insert_cycle = _asic->_cur_cycle; // get_updated_insert_cycle(dfg_id);   //_asic->_cur_cycle;
    next_tuple.dfg_id = cur_tuple.dfg_id;
    assert(dfg_id == next_tuple.dfg_id && "dfg does not match with the source");
    _scratch_process[dfg_id].push(next_tuple); // why shouldn't this also push to lsq data? (just because we want to model it as a new pipeline stage)
  }
}

bool multiply::can_push_in_process(int dfg_id)
{
  return (_lsq_process[dfg_id].size() < _mult_fifo_len);
}

bool multiply::can_push_in_process_reduce(int dfg_id)
{
  return (_process_reduce[dfg_id].size() < _mult_fifo_len);
}

bool multiply::can_push_in_scratch_process(int dfg_id)
{
  // return true;
  return (_scratch_process[dfg_id].size() < _mult_fifo_len);
}

int multiply::get_updated_insert_cycle(int dfg_id)
{
  uint64_t new_push_cycle = int(_last_push_cycle[dfg_id]);
  if (new_push_cycle == _last_push_cycle[dfg_id])
  { // integer
    new_push_cycle += 1;
  }
  else
  { // do not add
    assert(_asic->_config->_prefer_tq_latency && "coarse alloc should be greater than 1 for prefer tq latency");
  }
  int insert_cycle = std::max(_asic->_cur_cycle, new_push_cycle); // 1 -- 2
  // this should not have been incremented if added earlier
  if (_asic->_coarse_alloc[_core_id] > 1)
  {
    int float_cycles = insert_cycle;
    if (_last_push_cycle[dfg_id] > insert_cycle)
    {
      insert_cycle = _last_push_cycle[dfg_id];
    }
    _last_push_cycle[dfg_id] = float_cycles + (1 / (float)_asic->_coarse_alloc[_core_id]); // should be 1.5
  }
  else
  {
    _last_push_cycle[dfg_id] = insert_cycle;
  }
  return insert_cycle;
}

void multiply::push_scratch_data_to_pipeline(int dfg_id)
{
  int num_pops = 0;
  while (!_scratch_process[dfg_id].empty() && can_push_in_process(0) && can_push_in_process(1) && num_pops < 1)
  {
    ++num_pops;
    mult_data next_tuple = _scratch_process[dfg_id].front();
    assert(next_tuple.dfg_id == dfg_id && "same dfg_id should traverse through the pipeline");

    // next_tuple.insert_cycle = get_updated_insert_cycle(next_tuple.dfg_id);

    // next_tuple.insert_cycle=max(_asic->_cur_cycle, _last_push_cycle[next_tuple.dfg_id]+1);
    // _last_push_cycle[next_tuple.dfg_id]=next_tuple.insert_cycle;
    _lsq_process[next_tuple.dfg_id].push(next_tuple);
    _scratch_process[dfg_id].pop();
  }
}

// TODO: dfg_id should be attached to each memory accesses
void multiply::push_lsq_data(pref_tuple cur_tuple)
{ // consume the loaded values
  // push weight vector and source feature vector to the dfg
  // each task should create multiple pipeline tuples: Computation/vector_width
  // (here the rate of dispatch has to be much smaller -- TODO: model fixed depth of
  // these buffers)
  int row = cur_tuple.src_dist;
  if (_asic->_config->_algo == gcn)
  {
    assert(row < V && "even though we are not loading vector currently, it should be less than V");
  }
  // cout << "Rows size: " << cur_tuple.entry_cycle << endl;
  mult_data next_tuple;
  // cur_tuple.dfg_id = rand()%_data_parallel_throughput;
  next_tuple.insert_cycle = get_updated_insert_cycle(cur_tuple.dfg_id); // max(_asic->_cur_cycle, _last_push_cycle[0]+1);
  /*if(_core_id==0) {
    cout << "Pushing in the pipeline for dfg: " << cur_tuple.dfg_id << " at cycle: " << _asic->_cur_cycle << endl;
  }*/
  // _last_push_cycle[0]=next_tuple.insert_cycle;
  next_tuple.row = cur_tuple.src_id;
  next_tuple.second_buffer = cur_tuple.second_buffer;

  // FIXME: later
  // cur_tuple.dfg_id = rand()%_data_parallel_throughput;

  if (_asic->_config->_algo == gcn)
  {
    for (int c = 0; c < cur_tuple.end_edge_served; ++c)
    {
      // No need to push original data when we do use the result
      /*for(int i=0; i<FEAT_LEN; ++i) { // 16 stages..instead of 4
        next_tuple.vec1.push_back(_weight[i][i]);
        next_tuple.vec2.push_back(_asic->_scratch_vec[i][row]);
      }*/
    }
    _lsq_process[cur_tuple.dfg_id].push(next_tuple);
  }
  if (_asic->_config->_domain == tree)
  {
    _lsq_process[cur_tuple.dfg_id].push(next_tuple);
    /*int total_count = num_cache_lines_per_leaf;
    if(_asic->_config->_cache_hit_aware_sched) {
      total_count=1;
    }
    for(int i=0; i<total_count; ++i) {
      _lsq_process.push(next_tuple);
    }*/
  }
}

// TODO: we only need to do 1 update per cycle based on this granularity?
// I need to add a delay in this buffer...
void multiply::process(int dfg_id)
{
  int num_pops = 0;
  // add cycles for the dot product -- and write the value in output
  bool latency_cond = !_lsq_process[dfg_id].empty() && _asic->_cur_cycle > (_lsq_process[dfg_id].front().insert_cycle + _dfg_latency);
  /*if(_asic->_cur_cycle>50000 && _core_id==0) {
    cout << "Cycle: " << _asic->_cur_cycle << " core: " << _core_id << " dfg: " << dfg_id << " process_reduce full: " << _process_reduce[dfg_id].size() << " insert_cycle_cond: " << latency_cond << " lsq_empty? " << _lsq_process[dfg_id].empty() << endl;
  }*/

  while (num_pops < 1 && latency_cond && can_push_in_process_reduce(dfg_id))
  { // only 1 pop
    ++num_pops;
    mult_data cur_tuple = _lsq_process[dfg_id].front();
    mult_data dot_product = cur_tuple;
    int dotp = 0;
    cur_tuple.vec1.clear();
    cur_tuple.vec2.clear();
    // for(int i=0; i<4; ++i) { // 16 stages..instead of 4
    /*if(_asic->_config->_algo==gcn) {
      for(int i=0; i<FEAT_LEN; ++i) {
        dotp += cur_tuple.vec1[i]*cur_tuple.vec2[i];
      }
    }*/
    // dot_product.row=cur_tuple.row;
    dot_product.dotp = dotp;
    // dot_product.second_buffer = cur_tuple.second_buffer;
    _process_reduce[dfg_id].push(dot_product);
    _lsq_process[dfg_id].pop();
  }
}

void multiply::reduce(int dfg_id)
{
  int num_pops = 0;
  /*if(_asic->_coarse_alloc[_core_id]==2) {
    cout << "Cycle: " << _asic->_cur_cycle << " size of process_reduce: " << _process_reduce[dfg_id].size() << endl;
  }*/
  // write the accumulated value?
  while (num_pops < 1 && !_process_reduce[dfg_id].empty())
  { // only 1 pop
    ++_stat_vec_mat_local[dfg_id];
    mult_data output = _process_reduce[dfg_id].front(); // FIXME: over the network, this information is lost
    output.vec1.clear();
    output.vec2.clear();
    _process_reduce[dfg_id].pop();
    // cout << "Completed leaf at core: " << _core_id << "at cycle: " << _asic->_cur_cycle << endl;

    if (output.second_buffer)
      ++_asic->_odd_phase_matrix_mult;
    else
      ++_asic->_even_phase_matrix_mult;

    ++_asic->_stats->_stat_finished_coarse_tasks;
    ++_asic->_stats->_stat_vec_mat;

    // I should have a statistic for average latency??

    int is_matrix = (_stat_vec_mat_local[dfg_id] % vid_done_cycle == 0);
    int is_first_matrix = (_stat_vec_mat_local[dfg_id] % vid_done_cycle == 1);

    if (is_matrix)
    {
      if (!output.second_buffer)
      {
        ++_asic->_stats->_stat_tot_finished_first_buffer_data_parallel_tasks;
      }
      else
      {
        ++_asic->_stats->_stat_tot_finished_second_buffer_data_parallel_tasks;
      }
      /*if(_core_id==14 && _asic->_cur_cycle > 1000 && _asic->_cur_cycle < 3000) {
        cout << "Completed vid: " << output.row << " at cycle: " << _asic->_cur_cycle << endl;
      }*/
      /*if(_core_id==14) {
        cout << "Completed mult tasks: at core: " << _core_id << " for vid: " << output.row << " and cycle: " << _asic->_cur_cycle << " and vid done cycle: " << vid_done_cycle << endl;
      }*/
      // cout << " Double buffer: " << output.second_buffer << endl;
    }
    else if (is_first_matrix)
    {
      _prev_vid[dfg_id] = output.row;
    }
    else
    {
      /*if(_prev_vid[dfg_id]!=output.row) {
        cout << "core: " << _core_id << " dfg_id: " << dfg_id << " prev vid: " << _prev_vid[dfg_id] << " current vid: " << output.row << endl;
      }*/
      assert((_asic->_config->_domain == tree || _prev_vid[dfg_id] == output.row) && "same matrix multiply should come in sequence");
    }
    /*if(_core_id==5) {
      cout << "core: " << _core_id << " dfg_id: " << dfg_id << " prev vid: " << _prev_vid[dfg_id] << " current vid: " << output.row << endl;
    }*/

    // FIXME: how do I check that these many vertices are done for this?? Do only if a next layer is left
    // After the current barrier, we can apply for the next layer, it needs to ping-pong
    // So, do this only if it is not second_buffer.
    bool allowed_to_create = false;
    if (output.second_buffer && _asic->_gcn_even_phase == false)
      allowed_to_create = true;
    if (!output.second_buffer && _asic->_gcn_even_phase == true)
      allowed_to_create = true;
    // FIXME: What about the tasks lost when the multiplications of layer 2 could not produce aggregation?
    if (_asic->_stats->_stat_barrier_count < GCN_LAYERS - 1 && _asic->_config->_mult_agg_type != global && is_matrix)
    {
      int vid = output.row; // row...should be send along
      assert(vid < V && "completed vertex is wrong");
      task_entry new_task(vid, _asic->_offset[vid]);
      int core = _asic->_scratch_ctrl->get_local_scratch_id(vid);
      // if deat==2, it will check this new data-structure...
      // _asic->_correct_vertex_data_double_buffer[vid] = (_asic->_scratch_ctrl->_in_degree[vid])*FEAT_LEN*message_size/bus_width;
      if (allowed_to_create)
      {
        if (_asic->_gcn_even_phase)
        {
          new_task.second_buffer = true;
        }
        // cout << "Multiplication completed for vid: " << new_task.vid << " at core: " << _core_id << " and cycle: " << _asic->_cur_cycle << endl;
        _asic->_task_ctrl->insert_local_task(0, 0, core, 0, new_task);
        // cout << "Even phase: " << _asic->_gcn_even_phase << " created agg task for vid: " << vid << " at cycle: " << _asic->_cur_cycle << " for second buffer: " << new_task.second_buffer << endl;
      }
      else
      {
        if (_asic->_gcn_even_phase)
        {
          new_task.second_buffer = false;
        }
        // Do not want to create tasks for the next-next layer
        if (_asic->_stats->_stat_barrier_count < (GCN_LAYERS - 2))
        {
          _asic->_task_ctrl->_pending_coarse_buffer.push(new_task);
        }
      }
    }

    // this should be done only for even iterations
    // FIXME: not working for FEAT_LEN=128..
    // Also, it is coming here after the first global barrier...
    // if(_asic->_even_phase_matrix_mult%(_asic->_graph_vertices*vid_done_cycle)==0 || _asic->_even_phase_matrix_mult%(_asic->_graph_vertices*vid_done_cycle)==0) {
    if (_asic->_config->_mult_agg_type != global && _asic->_cur_cycle % 10000 == 0)
    {
      cout << "Odd phase matrix: " << _asic->_odd_phase_matrix_mult << " even phase matrix: " << _asic->_even_phase_matrix_mult << endl;
      cout << "Barrier count: " << _asic->_stats->_stat_barrier_count << " and mult to agg config: " << _asic->_config->_mult_agg_type << endl;
      cout << "Even phase condition, barrier: " << _asic->_stats->_stat_barrier_count << " is even phase: " << _asic->_gcn_even_phase << " num even matrix mult: " << _asic->_even_phase_matrix_mult << " barrier mult: " << (_asic->_non_dangling_graph_vertices * vid_done_cycle) << " mult_agg: " << _asic->_config->_mult_agg_type << endl;
    }
    // could be any lot, just to keep track...
    if (_asic->_stats->_stat_barrier_count < (GCN_LAYERS - 1) && _asic->_gcn_even_phase && _asic->_even_phase_matrix_mult == 1 * (_asic->_non_dangling_graph_vertices * vid_done_cycle) && _asic->_config->_mult_agg_type != global)
    {
      // if(_asic->_stats->_stat_barrier_count<(GCN_LAYERS-1) && _asic->_gcn_even_phase && _asic->_even_phase_matrix_mult>0.95*(_asic->_graph_vertices*vid_done_cycle) && _asic->_config->_mult_agg_type!=global) {
      ++_asic->_stats->_stat_barrier_count;
      for (int vid = 0; vid < _asic->_graph_vertices; ++vid)
      {
        _asic->_correct_vertex_data[vid] = (_asic->_scratch_ctrl->_in_degree[vid] * FEAT_LEN * message_size) / bus_width;
      }
      cout << "Assigned new correct data: " << _asic->_correct_vertex_data[0] << " and " << _asic->_correct_vertex_data[1] << endl;
      _asic->_gcn_even_phase = false;
      _asic->_even_phase_matrix_mult = 0;
      int tasks_pushed = 0;
      while (!_asic->_task_ctrl->_pending_coarse_buffer.empty())
      {
        task_entry pending_task = _asic->_task_ctrl->_pending_coarse_buffer.front();
        if (_asic->_stats->_stat_barrier_count < (GCN_LAYERS - 1))
        {
          ++tasks_pushed;
          _asic->_task_ctrl->insert_local_task(0, 0, _asic->_scratch_ctrl->get_local_scratch_id(pending_task.vid), 0, pending_task);
        }
        _asic->_task_ctrl->_pending_coarse_buffer.pop();
      }
      cout << "Even phase completed with new barrier count: " << _asic->_stats->_stat_barrier_count << " and new tasks pushed: " << tasks_pushed << endl;
      // FIXME: is that correct?
      cout << "Finished edges till now: " << _asic->_stats->_stat_tot_finished_edges << endl;
      // even phase completed; aggregation of the previous layer and the next should be max done
      assert(_asic->_stats->_stat_tot_finished_edges <= (2 * FEAT_LEN / 16 * _asic->_stats->_stat_barrier_count * _asic->_graph_edges) && "agg for gcn layers should have been completed");
      // assert(_asic->_stats->_stat_tot_finished_edges<(FEAT_LEN/16*2*_asic->_stats->_stat_barrier_count*_asic->_graph_edges) && "agg for gcn layers should have been completed");
      _asic->_scratch_ctrl->push_dangling_vertices(true);
    }
    // okay, this means that second layer is done. No need to set new phases, Odd phase matrix should remain reset.
    if (_asic->_stats->_stat_barrier_count < (GCN_LAYERS - 1) && !_asic->_gcn_even_phase && _asic->_odd_phase_matrix_mult == (1 * _asic->_non_dangling_graph_vertices * vid_done_cycle) && _asic->_config->_mult_agg_type != global)
    {
      ++_asic->_stats->_stat_barrier_count;
      for (int vid = 0; vid < _asic->_graph_vertices; ++vid)
      {
        _asic->_correct_vertex_data_double_buffer[vid] = (_asic->_scratch_ctrl->_in_degree[vid] * FEAT_LEN * message_size) / bus_width;
      }
      cout << "Assigned second-buffer correct data: " << _asic->_correct_vertex_data_double_buffer[0] << " and " << _asic->_correct_vertex_data_double_buffer[1] << endl;
      _asic->_gcn_even_phase = true;
      _asic->_odd_phase_matrix_mult = 0;
      int tasks_pushed = 0;
      while (!_asic->_task_ctrl->_pending_coarse_buffer.empty())
      {
        task_entry pending_task = _asic->_task_ctrl->_pending_coarse_buffer.front();
        if (_asic->_stats->_stat_barrier_count < (GCN_LAYERS - 1))
        {
          ++tasks_pushed;
          _asic->_task_ctrl->insert_local_task(0, 0, _asic->_scratch_ctrl->get_local_scratch_id(pending_task.vid), 0, pending_task);
        }
        _asic->_task_ctrl->_pending_coarse_buffer.pop();
      }
      cout << "Odd phase completed with new barrier count: " << _asic->_stats->_stat_barrier_count << " and tasks pushed: " << tasks_pushed << endl;
      cout << "Finished edges till now: " << _asic->_stats->_stat_tot_finished_edges / (FEAT_LEN / 16) << endl;
      assert(_asic->_stats->_stat_tot_finished_edges < (FEAT_LEN / 16) * 2 * _asic->_stats->_stat_barrier_count * _asic->_graph_edges && "agg for gcn layers should have been completed");
      _asic->_scratch_ctrl->push_dangling_vertices(false);
    }
    ++num_pops;
  }
}

bool multiply::can_push_local_coarse_grain_task(int dfg_id)
{
  bool are_two_tasks_ready = _local_coarse_task_queue.size() >= BDCAST_WAIT || _pending_task[dfg_id].weight_rows_done > 0;
  if (are_two_tasks_ready)
    return true;
  if (!are_two_tasks_ready && !_local_coarse_task_queue.empty())
  {
    if (_asic->_cur_cycle - _local_coarse_task_queue.front().entry_cycle > MAX_DELAY)
      return true;
  }
  return false;
}

// For a double throughput, they should call each core twice.
bool multiply::cycle(int phase)
{
  int served = 0;
  if (_asic->_config->_hats == 1)
  { //  && _asic->_config->_domain==tree) {
    served += serve_local_hats_requests();
    serve_local_coalescer_requests();
  }

  _multicast_batch_size = _asic->_remote_coalescer[_core_id]->_multicast_batch_size;
  _asic->_remote_coalescer[_core_id]->cycle();
  _asic->_local_coalescer[_core_id]->cycle();
  /*if(_asic->_cur_cycle>10000) {
    cout << "Break for lower throughput\n";
  }*/
  // writing the computed value to its corresponding location

  // performing the computation
  for (int i = 0; i < _data_parallel_throughput; ++i)
  {
    reduce(i);
    process(i);
  }
  // FIXME: receive request should know whether it should push here...

  // while(!_asic->_coarse_compl_buf[_core_id]->_pref_lsq.empty() && can_push_in_process(0) && can_push_in_process(1)) {
  int start_core = (_core_id / _asic->_config->_batched_cores) * _asic->_config->_batched_cores;
  int end_core = start_core + _asic->_config->_batched_cores;
  int last_core = start_core + rand() % _asic->_config->_batched_cores;
  for (int d = 0; d < _data_parallel_throughput; ++d)
  {
    // TODO: if cannot, then push to the other core
    while (!_asic->_coarse_compl_buf[_core_id][d]->_pref_lsq.empty())
    {
      if (can_push_in_process(d))
      {
        assert(_asic->_coarse_compl_buf[_core_id][d]->peek_lsq_dfg() == d);
        pref_tuple cur_tuple = _asic->_coarse_compl_buf[_core_id][d]->receive_lsq_responses();
        for (int r = 0; r < cur_tuple.repeat_times; ++r)
        { // should push to different cores
          if (_asic->_config->_update_coalesce && _asic->_config->_mult_cache_hit_aware_sched)
          {
            for (int i = 0; i < _asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.src_id]; ++i)
            {
              push_lsq_data(cur_tuple);
            }
            _asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.src_id] = 0;
          }
          else
          {
            if (_asic->_config->_batched_cores > 1)
            {
              _asic->_mult_cores[last_core]->push_lsq_data(cur_tuple); // should set prev_vid as well!!
              // cout << "Cycle: " << _asic->_cur_cycle << " pushing to core: " << last_core << endl;
              last_core = (last_core + 1);
              if (last_core == end_core)
                last_core = start_core;
            }
            else
            {
              push_lsq_data(cur_tuple);
            }
          }
        }
      }
      else
      { // push to other core
        // FIXME: why does lsq process has so much more data?
        /*if(_asic->_cur_cycle>10000) {
          cout << "Core: " << _core_id << " dfg: " << d << " prefetch_full: " << _lsq_process[d].size() << endl;
        }*/
        // Chose another core to insert
        // break;
        int min_pref_size = _mult_fifo_len;
        int new_dst_core = -1;
        int new_dfg = -1;
        for (int x = _core_id, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
        {
          for (int dfg = 0; dfg < _asic->_mult_cores[x]->_data_parallel_throughput; ++dfg)
          {
            if (_asic->_mult_cores[x]->_lsq_process[dfg].size() < min_pref_size)
            {
              min_pref_size = _asic->_mult_cores[x]->_lsq_process[dfg].size();
              new_dst_core = x;
              new_dfg = dfg;
            }
          }
        }
        if (new_dst_core >= 0)
        {
          // cout << "Cycles: " << _asic->_cur_cycle << " new selected core: " << new_dst_core << endl;
          pref_tuple cur_tuple = _asic->_coarse_compl_buf[_core_id][d]->receive_lsq_responses();
          assert(_asic->_mult_cores[new_dst_core]->can_push_in_process(new_dfg));
          cur_tuple.dfg_id = new_dfg;
          for (int r = 0; r < cur_tuple.repeat_times; ++r)
          { // should push to different cores
            _asic->_mult_cores[new_dst_core]->push_lsq_data(cur_tuple);
          }
        }
        else
        {
          break;
        }
      }
    }
    _asic->_coarse_compl_buf[_core_id][d]->cb_to_lsq();
  }

  // consuming prefetched and reordered data in the computation pipeline
  // initiating prefetch
  int max_cb_entries = num_cache_lines_per_weight;
  if (_asic->_config->_domain == tree)
  {
    max_cb_entries = num_cache_lines_per_leaf;
  }
  // num_cache_lines_per_weight *= FEAT_LEN;
  bool call = true;
  // if(phase==1 && _asic->_config->_prefer_tq_latency) {
  //   call=false;
  // }
  for (int i = 0, d = (_last_dfg_id + 1) % _data_parallel_throughput; i < _data_parallel_throughput && call; ++i, d = (d + 1) % _data_parallel_throughput)
  {
    bool cond1 = _asic->_coarse_compl_buf[_core_id][d]->_entries_remaining >= max_cb_entries;  // Okay, this is reorder buffer
    bool cond2 = (!_local_coarse_task_queue.empty() || _pending_task[d].weight_rows_done > 0); // some task is available or not!!
    // cout << "Cycle: " << _asic->_cur_cycle << " cond1: " << cond1 << " cond2: " << cond2 << endl; // many times free cb partitions are not available

    if (_asic->_coarse_compl_buf[_core_id][d]->can_push() && can_push_local_coarse_grain_task(d))
    {
      // if(_asic->_coarse_compl_buf[_core_id]->can_push() && _asic->_task_ctrl->can_push_coarse_grain_task()) {
      vector<int> rows;
      vector<int> weight_rows_done;
      vector<bool> second_buffer;
      int dfg_id_per_task;
      // while(_asic->_coarse_compl_buf[_core_id]->_entries_remaining>((rows.size()+1)*num_cache_lines_per_weight) && !_asic->_task_ctrl->_coarse_task_queue.empty() && rows.size()<BDCAST_WAIT) {
      // cout << "Entries remaining in the completion buffer: " << _asic->_coarse_compl_buf[_core_id]->_entries_remaining << endl;
      cond1 = _asic->_coarse_compl_buf[_core_id][d]->_entries_remaining >= ((rows.size() + 1) * max_cb_entries);
      while (cond1 && cond2 && rows.size() < BDCAST_WAIT)
      {
        // mult_task new_task = _asic->_task_ctrl->schedule_coarse_grain_task();
        // cout << "Came in to pop new task from core: " << _core_id << " with size: " << rows.size() << "\n";
        mult_task new_task; // = _local_coarse_task_queue.front();
        if (_pending_task[d].weight_rows_done > 0)
        {
          new_task = _pending_task[d];
        }
        else
        {
          new_task = _local_coarse_task_queue.front();
        }
        rows.push_back(new_task.row);
        second_buffer.push_back(new_task.second_buffer);
        weight_rows_done.push_back(new_task.weight_rows_done);
        if (new_task.weight_rows_done == 0)
        {
          dfg_id_per_task = d; // dfg_id_per_task.push_back(d); // assign a new dfg id
        }
        else
        {
          dfg_id_per_task = new_task.dfg_id; // dfg_id_per_task.push_back(new_task.dfg_id); // should use same dfg id as assigned earlier
          d = dfg_id_per_task;
          // Let's say second was over (oh this should be considered)
        }
        /*if(rows[0]==732 && _core_id==5) {
          cout << "276 rows core: " << _core_id << " dfg_id: " << dfg_id_per_task << " cycle: " << _asic->_cur_cycle << endl;
        }*/
        // _local_coarse_task_queue.pop();
      }
      /*if(_core_id==8) {
        cout << "Output BREAKPOINT with remaining entries: " << _asic->_task_ctrl->_remaining_local_coarse_task_entries[_core_id] << "\n";
      }*/
      bool cond_for_split = (_asic->_coarse_reorder_buf[_core_id][d]->_free_cb_parts.size() > 0) || (_asic->_coarse_reorder_buf[_core_id][d]->_cb_start_partitions[d].size() == 0);
      // cout << "cond for split: " << cond_for_split << endl;
      if (rows.size() > 0 && (_asic->_coarse_reorder_buf[_core_id][d]->_entries_remaining >= max_cb_entries) && cond_for_split)
      {
        _stat_exp_bw_save += (rows.size() - 1);
        int num_done = dispatch(dfg_id_per_task, rows, weight_rows_done, second_buffer);
        int done_this_cycle = num_done - weight_rows_done[0];
        /*if(_core_id==14) {
          cout << "Dispatched for dfg_id: " << dfg_id_per_task << " and num_cache_lines_for_8: " <<  done_this_cycle
          << " cycle: " << _asic->_cur_cycle << " core: " << _core_id << endl;
        }*/
        /*if(_core_id==14) {
          cout << "Dispatched a multiply task with vid: " << rows[0] << " at cycle: " << _asic->_cur_cycle 
          << " with num_done: " << num_done << " compl_buf: " << _asic->_coarse_reorder_buf[_core_id]->_entries_remaining << endl;
        }*/
        if (_asic->_config->_algo == gcn)
        {
          assert(done_this_cycle > 0 && "did all tests for allocating at least 1 feature length cache lines");
          assert(num_done > 0);
          if (num_done == FEAT_LEN)
          {
            if (_pending_task[d].weight_rows_done > 0)
            { // taken from pending buffer
              _pending_task[d].weight_rows_done = 0;
            }
            else
            {
              _local_coarse_task_queue.pop();
            }
            _asic->_task_ctrl->_remaining_local_coarse_task_entries[_core_id]++;
          }
          else
          {
            // this should be pushed to its old dfg id
            if (_pending_task[d].weight_rows_done > 0)
            { // taken from pending buffer
              _pending_task[d].weight_rows_done = num_done;
            }
            else
            { // pushing to pending buffer
              _local_coarse_task_queue.front().weight_rows_done = num_done;
              _local_coarse_task_queue.front().dfg_id = dfg_id_per_task;
              _pending_task[d] = _local_coarse_task_queue.front();
              _local_coarse_task_queue.pop();
            }
          }
        }
        else
        {
          _local_coarse_task_queue.pop();
          _asic->_task_ctrl->_remaining_local_coarse_task_entries[_core_id]++;
        }

        if (_asic->_config->_algo == gcn)
        {
          _last_dfg_id = (_last_dfg_id + 1) % _data_parallel_throughput;
        }
        _asic->_last_dispatch_core = _core_id;
      }
    }
  }

  // if done in later cycles, then??
  for (int dfg_id = 0; dfg_id < _data_parallel_throughput; ++dfg_id)
  {
    push_scratch_data_to_pipeline(dfg_id);
    if (!_asic->_coarse_reorder_buf[_core_id][dfg_id]->_pref_lsq.empty() && can_push_in_scratch_process(dfg_id))
    {
      push_scratch_data(dfg_id, _asic->_coarse_reorder_buf[_core_id][dfg_id]->receive_lsq_responses());
    }
    _asic->_coarse_reorder_buf[_core_id][dfg_id]->cb_to_lsq();
  }
  _asic->_scratch_ctrl->receive_scratch_request();

  if (_asic->_config->_mult_cache_hit_aware_sched == 1 && _asic->_config->_domain == tree)
  {
    for (int i = 0; i < _data_parallel_throughput; ++i)
    {
      serve_miss_task_queue(i);
    }
  }
  // cout << "Served at core: " << _core_id << " cycle: " << _asic->_cur_cycle << " is: " << served << endl;

  bool done = pipeline_inactive(false);
  return !done;
}

bool multiply::pipeline_inactive(bool show)
{
  if (!_asic->_local_coalescer[_core_id]->pipeline_inactive(show))
  {
    if (show)
      cout << "Local coalescer was active" << endl;
    return false;
  }
  if (!_asic->_remote_coalescer[_core_id]->pipeline_inactive(show))
  {
    if (show)
      cout << "Remote coalescer was active" << endl;
    return false;
  }
  if (!_asic->_task_ctrl->_pending_coarse_buffer.empty())
  {
    if (show)
      cout << "pending coarse buffer queue not empty: " << _asic->_task_ctrl->_pending_coarse_buffer.size() << endl;
    return false;
  }
  if (!_prefetch_lsq.empty())
  {
    if (show)
      cout << "prefetch_lsq queue not empty: " << _prefetch_lsq.size() << endl;
    return false;
  }
  for (int i = 0; i < MAX_DFGS; ++i)
  {
    if (_asic->_cur_cycle < _lane_blocked_until[i])
    {
      if (show)
        cout << "Cur lane blocked for temporal reuse at core: " << _core_id << " dfg: " << i << " until: " << _lane_blocked_until[i] << endl;
      return false;
    }
    if (!_asic->_coarse_compl_buf[_core_id][i]->pipeline_inactive(show))
    {
      if (show)
        cout << "Local compl buf is not empty: " << _asic->_coarse_compl_buf[_core_id][i]->_entries_remaining << endl;
      return false;
    }
    if (!_lsq_process[i].empty())
    {
      if (show)
        cout << "lsq_process queue not empty: " << _lsq_process[i].size() << " at core: " << _core_id << " dfg_id: " << i << endl;
      /*if(_lsq_process[i].size()==FIFO_DEP_LEN) {
        cout << "Break to debug\n";
      }*/
      return false;
    }
    if (!_process_reduce[i].empty())
    {
      if (show)
        cout << "process_reduce queue not empty: " << _process_reduce[i].size() << endl;
      return false;
    }
    if (!_scratch_process[i].empty())
    {
      if (show)
        cout << "scratch_process queue not empty: " << _scratch_process[i].size() << endl;
      return false;
    }
    if (!_asic->_coarse_reorder_buf[_core_id][i]->pipeline_inactive(show))
    {
      if (show)
        cout << "Local reorder buf is not empty: " << _asic->_coarse_reorder_buf[_core_id][i]->_entries_remaining << endl;
      return false;
    }
    if (_pending_task[i].weight_rows_done > 0)
    {
      if (show)
        cout << "Buffer for pending task is not empty: " << _pending_task[i].weight_rows_done << endl;
      return false;
    }
  }

  if (!_asic->_task_ctrl->_coarse_task_queue.empty())
  {
    if (show)
      cout << "Coarse grained task queue is not empty: " << _asic->_task_ctrl->_coarse_task_queue.size() << endl;
    return false;
  }
  if (!_local_coarse_task_queue.empty())
  {
    if (show)
      cout << "Local coarse grained task queue is not empty: " << _local_coarse_task_queue.size() << " and core: " << _core_id << endl;
    return false;
  }
  if (!_local_miss_task_queue.empty())
  {
    if (show)
      cout << "Local miss task queue is not empty: " << _local_miss_task_queue.size() << endl;
    return false;
  }
  if (_asic->_pending_hats_requests != 0)
  {
    if (show)
      cout << "Pending hats requests is not empty: " << _asic->_pending_hats_requests << endl;
    return false;
  }

  return true;
}

// TODO: this is the one issuing tasks right now; so it has the responsibility of assigning tasks to different dfg_id
// instead of dispatch...
// FIXME: this is a common batching for all cores; and then a core gets its leaf. Not all cores are filled up..
int multiply::serve_hats_requests()
{
  // this stall should be introduced while looping every time
  if ((_asic->_cur_cycle - _asic->_phase_cycle) < LSQ_WAIT)
  {
    ++_asic->_stats->_stat_delayed_batch_cycles;
    // cout << "Core: " << _core_id << " cycle: " << _asic->_cur_cycle << " batch cycles: " << _asic->_stats->_stat_delayed_batch_cycles << endl;
    return 0;
  }
  if (_asic->_pending_hats_requests == 0)
  {
    ++_asic->_stats->_stat_stall_thr[_core_id];
    return 0;
  }

  pref_tuple cur_tuple;
  // cur_tuple.dfg_id = (_last_dfg_id+1)%_data_parallel_throughput;
  // _last_dfg_id = (_last_dfg_id+1)%_data_parallel_throughput;

  // TODO: block for temporal serialization; chose one of them for load balance among vector lanes -- allocating to the one with more free entries (if same, just rand??)
  int free_entry1 = _asic->_coarse_compl_buf[_core_id][0]->_entries_remaining;
  int larger_entry = 0;
  if (_lane_blocked_until[0] < _asic->_cur_cycle)
  {
    larger_entry = free_entry1;
  }
  cur_tuple.dfg_id = 0; // TODO: does it always prioritize dfg_id=0?
  for (int d = 1; d < _data_parallel_throughput; ++d)
  {
    if (_lane_blocked_until[d] < _asic->_cur_cycle)
    {
      int free_entry2 = _asic->_coarse_compl_buf[_core_id][d]->_entries_remaining;
      if (free_entry2 > larger_entry)
      {
        cur_tuple.dfg_id = d;
        larger_entry = free_entry2;
      }
    }
  }

  // cur_tuple.dfg_id = rand()%MAX_DFGS;
  // if(_blocked_until[cur_tuple.dfg_id] > _asic->_cur_cycle) return 0;
  // larger_entry = _asic->_coarse_compl_buf[_core_id][cur_tuple.dfg_id]->_entries_remaining;

  // if(larger_entry < num_cache_lines_per_leaf*_multicast_batch_size) {
  if (larger_entry < num_cache_lines_per_leaf)
  {
    ++_asic->_stats->_stat_mem_stall_thr[_core_id];
    return 0;
  }
  // Chose the one with larger free entries
  /*if(_asic->_coarse_compl_buf[_core_id]->_entries_remaining<num_cache_lines_per_leaf) {
    ++_asic->_stats->_stat_mem_stall_thr[_core_id];
    return;
  }*/

  int count = 0;
  while (_asic->_pending_hats_requests > 0 && _asic->_leaf_access_count[_asic->_prev_leaf_id] == 0 && count < _asic->num_kdtree_leaves)
  {
    _asic->_prev_leaf_id = (_asic->_prev_leaf_id + 1) % _asic->num_kdtree_leaves;
    ++count;
    if (_asic->_prev_leaf_id == 0)
    { // or if pending request is less than X, then we should stall.
      ++_asic->_round;
      if (_asic->_round == _asic->max_rounds)
      {
        _asic->_phase_cycle = _asic->_cur_cycle;
        _asic->_round = 0;
        cout << "Setting the phase cycle to be: " << _asic->_phase_cycle << " served coarse tasks: " << _asic->_stats->_stat_vec_mat << endl;
        // TODO: print certain statistics here...
        // _asic->print_simulation_status();
      }
    }
  }
  assert(_asic->_leaf_access_count[_asic->_prev_leaf_id] > 0 && "positive number of queries for a leaf are left");
  // cout << "Serving hats bitvector for vid: " << _asic->_prev_leaf_id
  // << " entries: " << _asic->_leaf_access_count[_asic->_prev_leaf_id]  << endl;
  int to_reduce = 1;
  to_reduce = min(_asic->_leaf_access_count[_asic->_prev_leaf_id], int(_multicast_batch_size));

  int num_temporal_rounds = ceil(_asic->_leaf_access_count[_asic->_prev_leaf_id] / (float)to_reduce);
  _lane_blocked_until[cur_tuple.dfg_id] = _asic->_cur_cycle + num_temporal_rounds - 1;
  // cout << "Issuing at cycle: " << _asic->_cur_cycle << " core: " << _core_id << " dfg_id: " << cur_tuple.dfg_id << " number of replications: " << _asic->_leaf_access_count[_asic->_prev_leaf_id]  << " and leaf id: " << _asic->_prev_leaf_id << endl;

  ++_asic->_stats->_num_spatial_issue;
  _asic->_stats->_num_tasks_batched += to_reduce;
  _asic->_mem_ctrl->_l2accesses += (to_reduce - 1);
  _asic->_mem_ctrl->_l2hits += (to_reduce - 1);
  _asic->_stats->_stat_batched_tasks += (to_reduce);
  // TODO: this is once per task
  ++_asic->_stats->_stat_cycles_batched;
  // cout << "Leaf id: " << _asic->_prev_leaf_id << " batch size: " << to_reduce
  // << " data present: " << _asic->_leaf_access_count[_asic->_prev_leaf_id] << " at cycle: " << _asic->_cur_cycle << endl;
  // _asic->_pending_hats_requests -= to_reduce;
  // _asic->_leaf_access_count[_asic->_prev_leaf_id] -= to_reduce;
  _asic->_pending_hats_requests -= _asic->_leaf_access_count[_asic->_prev_leaf_id];
  _asic->_leaf_access_count[_asic->_prev_leaf_id] = 0;
  uint64_t line_addr = _asic->_mem_ctrl->_weight_offset + (_asic->_prev_leaf_id * num_cache_lines_per_leaf * line_size);
  cur_tuple.end_edge_served = 1;

  cur_tuple.req_core = _core_id;
  cur_tuple.core_type = coarseMem;
  cur_tuple.second_buffer = false;
  cur_tuple.src_id = _asic->_prev_leaf_id;
  // TODO: this should be different from the one for dispatch; not required at this time...

  for (int c = 0; c < num_cache_lines_per_leaf; ++c)
  {
    cur_tuple.cb_entry = _asic->_coarse_compl_buf[_core_id][cur_tuple.dfg_id]->allocate_cb_entry(1); // num_cache_lines_per_leaf);
    // cout << "Sent request for addr: " << line_addr << " and vid: " << cur_tuple.src_id << " core: " << _core_id
    // << " dfg_id: " << cur_tuple.dfg_id << " cycle: " << _asic->_cur_cycle << endl;
    _asic->_mem_ctrl->send_mem_request(0, line_addr, cur_tuple, WEIGHT_SID);
    _num_wgt_reqs_really_sent++;
    line_addr += line_size;
  }
  return to_reduce;
}
