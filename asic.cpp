#include "network.hh"
#include "stats.hh"
#include <fstream>

void memory_controller::power_callback(double a, double b, double c, double d)
{
  // printf("power callback: %0.3f, %0.3f, %0.3f, %0.3f\n",a,b,c,d);
}

int coalescing_buffer::get_avg_batch_size()
{
  return _total_current_entries / (float)_entries_in_priority_update;
}

void coalescing_buffer::push_local_read_responses_to_cb(int index_into_batched)
{
  // pop the requests from the current batch as to be to served..this should be
  // done at served
  while (_num_batched_tasks_per_index[index_into_batched] > 0)
  {
    pref_tuple return_to_lsq;
    remote_read_task remote_read_data = _data_of_batched_tasks[_num_batched_tasks_per_index[index_into_batched] - 1][index_into_batched];
    // push these to reorder buffer as in receive_scratch_request..
    return_to_lsq.dfg_id = remote_read_data.dfg_id;

    // return_to_lsq.entry_cycle = remote_read_data.entry_cycle;
    int cb_buf_entry = remote_read_data.cb_entry;
    // cb_buf_entry = remote_read_data.dst_id;
    auto reorder_buf = _asic->_coarse_reorder_buf[remote_read_data.req_core][return_to_lsq.dfg_id];

    return_to_lsq.second_buffer = reorder_buf->_reorder_buf[cb_buf_entry].cur_tuple.second_buffer;
    return_to_lsq.src_id = reorder_buf->_reorder_buf[cb_buf_entry].cur_tuple.src_id;

    assert(!reorder_buf->_reorder_buf[cb_buf_entry].valid && "cb overflow, no element should be available here beforehand");
    assert(reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr > 0 && "it should be waiting on certain number of cache lines");
    reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr--;

    if (reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr == 0)
    {
      reorder_buf->_reorder_buf[cb_buf_entry].valid = true;
      reorder_buf->_reorder_buf[cb_buf_entry].cur_tuple = return_to_lsq;
    }
    remote_read_task data(-1, -1, -1);
    _data_of_batched_tasks[_num_batched_tasks_per_index[index_into_batched] - 1][index_into_batched] = data;
    --_num_batched_tasks_per_index[index_into_batched];
  }
  assert(index_into_batched >= 0 && "cannot push local responses from a negative location");
  free_batch_entry(index_into_batched);
}

void coalescing_buffer::cycle()
{
  if (_asic->_cur_cycle % 10 == 0)
  {
    fill_updates_from_overflow_buffer();
  }
  if (_asic->_cur_cycle % 1 == 0)
  {
    update_priorities_using_cache_hit();
  }
}

/*pair<int, int> coalescing_buffer::peek() {
  auto it = _priority_update_queue.begin();
  auto it2 = (it->second).begin();
  int done=0;
  for(it=_priority_update_queue.begin(); it!=_priority_update_queue.end(); ++it) {
    for(it2=(it->second).begin(); it2!=(it->second).end(); ++it2) {
      if(done==_cur_local_coalescer_ptr) {
        ++done;
        break;
      }
      ++done;
    }
  }
  assert(done>0 && "shouldn't have come here for empty update queue");
  if(done<_cur_local_coalescer_ptr) return make_pair(-1,-1);
  int leaf_id = it2->first;
  int index_into_batched = it2->second;

  _cur_local_coalescer_ptr = (_cur_local_coalescer_ptr+1)%_batch_length;
  return make_pair(leaf_id, index_into_batched);
}*/

// no need to traverse for finding empty entries; we can just pop a value from the priority task queue
pair<int, int> coalescing_buffer::pop()
{
  auto it = _priority_update_queue.begin();
  auto it2 = (it->second).begin();
  int leaf_id = it2->first;
  int index_into_batched = it2->second;

  // erase only if mcast batch is sufficient or temporal reuse is allowed
  bool should_pop = _asic->_config->_domain == tree;
  // FIXME: should consider the next round..!!
  if (_asic->_config->_domain == graphs && _num_batched_tasks_per_index[index_into_batched] <= _multicast_batch_size)
  {
    should_pop = true;
  }
  if (should_pop)
  {
    (it->second).pop_front();
    if ((it->second).empty())
    {
      _priority_update_queue.erase(it->first);
    }
    --_entries_in_priority_update;
    _is_leaf_present.reset(leaf_id);
  }
  assert(index_into_batched != -1 && "cannot be pointing to -ve entry loc");
  return make_pair(leaf_id, index_into_batched);
}

void coalescing_buffer::free_batch_entry(int index_into_batched)
{
  if (_multicast_batch_size == 0)
  {
    _multicast_batch_size = 0.125;
  }
  _num_batched_tasks_per_index[index_into_batched] = 0;
  _batch_free_list.push(index_into_batched);
  assert(_batch_free_list.size() <= _batch_length && "free list cannot be larger than maximum size");
}

void coalescing_buffer::send_batched_compute_request(int leaf_id, pref_tuple cur_tuple, int index_into_batched)
{
  // int num_temporal_rounds = ceil(_num_batched_tasks_per_index[index_into_batched]/(float)_multicast_batch_size);
  int num_temporal_rounds = ceil(_num_batched_tasks_per_index[index_into_batched] / (float)(_multicast_batch_size * _asic->_config->_batched_cores));

  _total_current_entries -= _num_batched_tasks_per_index[index_into_batched];

  // spatial batching
  int spatial_batch = (_num_batched_tasks_per_index[index_into_batched] - num_temporal_rounds);
  for (int c = 0; c < spatial_batch; ++c)
  {
  }
  _asic->_mult_cores[_core_id]->_lane_blocked_until[cur_tuple.dfg_id] = _asic->_cur_cycle; //  + num_temporal_rounds - 1;

  assert(_num_batched_tasks_per_index[index_into_batched] <= _batch_width && "cannot have more mcast than allowed");

  ++_asic->_stats->_stat_cycles_batched;

  // statistics for l2 accesses
  int vectorized = (_num_batched_tasks_per_index[index_into_batched] - 1);
  _asic->_stats->_num_spatial_issue += num_temporal_rounds;
  _asic->_stats->_num_temporal_issue++;
  _asic->_stats->_num_tasks_batched += (vectorized + 1);
  _asic->_mem_ctrl->_l2accesses += (vectorized * num_cache_lines_per_leaf);
  _asic->_mem_ctrl->_l2hits += (vectorized * num_cache_lines_per_leaf);
  _asic->_mem_ctrl->_l2hits_per_core[_core_id] += (vectorized * num_cache_lines_per_leaf);
  _asic->_mem_ctrl->_l2accesses_per_core[_core_id] += (vectorized * num_cache_lines_per_leaf);
  _asic->_stats->_stat_batched_tasks += vectorized;

  // cur tuple should store this information
  _asic->_remote_coalescer[_core_id]->free_batch_entry(index_into_batched);
  // pushing a memory request
  cur_tuple.end_edge_served = 1;
  cur_tuple.req_core = _core_id;
  cur_tuple.core_type = coarseMem;
  cur_tuple.src_id = leaf_id;
  cur_tuple.second_buffer = false;

  uint64_t line_addr = _asic->_mem_ctrl->_weight_offset + (leaf_id * num_cache_lines_per_leaf * line_size);
  // TODO: Set temporal repeat for cur_tuple to be temporal_rounds.. (let's store in cur_tuple) -- this is like ss_repeat_port
  cur_tuple.repeat_times = num_temporal_rounds;
  bool sent_request = true;
  bool all_hits = true;
  for (int c = 0; c < num_cache_lines_per_leaf; ++c)
  {
    int old_coalesces = _asic->_mem_ctrl->_l2coalesces;
    cur_tuple.cb_entry = _asic->_coarse_compl_buf[_core_id][cur_tuple.dfg_id]->allocate_cb_entry(1);
    sent_request = _asic->_mem_ctrl->send_mem_request(0, line_addr, cur_tuple, WEIGHT_SID);
    if (sent_request || _asic->_mem_ctrl->_l2coalesces > old_coalesces)
    { // if coalesced or sent request
      all_hits = false;
    }

    _asic->_mult_cores[_core_id]->_num_wgt_reqs_really_sent++;
    line_addr += line_size;
  }
  all_hits = false;
  // if all are hits, then just push in the lsq_process -- otherwise do
  // according to the completion buffer
  if (all_hits)
  {
    int d = cur_tuple.dfg_id;
    int min_pref_size = _asic->_mult_cores[_core_id]->_mult_fifo_len;
    int new_dst_core = -1;
    if (_asic->_mult_cores[_core_id]->can_push_in_process(d))
    {
      new_dst_core = _core_id;
    }
    else
    {
      for (int x = (_core_id + 1) % core_cnt, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
      {
        if (_asic->_mult_cores[x]->_data_parallel_throughput > d && _asic->_mult_cores[x]->_lsq_process[d].size() < min_pref_size)
        {
          min_pref_size = _asic->_mult_cores[x]->_lsq_process[d].size();
          new_dst_core = x;
        }
      }
    }

    for (int c = 0; c < num_cache_lines_per_leaf && new_dst_core > -1; ++c)
    { // repeat_times for each..
      int cb_entry = _asic->_coarse_compl_buf[_core_id][cur_tuple.dfg_id]->deallocate_cb_entry(1);
      for (int r = 0; r < cur_tuple.repeat_times; ++r)
      {
        mult_data next_tuple;
        next_tuple.insert_cycle = _asic->_mult_cores[_core_id]->get_updated_insert_cycle(cur_tuple.dfg_id);

        next_tuple.row = cur_tuple.src_id;
        next_tuple.second_buffer = cur_tuple.second_buffer;
        _asic->_mult_cores[new_dst_core]->_lsq_process[cur_tuple.dfg_id].push(next_tuple);
      }
    }
  }
}

// TODO: send the index_into_batched, leaf_id and req_core to the remote core
void coalescing_buffer::send_batched_read_requests(int leaf_id, int index_into_batched)
{
  remote_read_task info = _data_of_batched_tasks[_num_batched_tasks_per_index[index_into_batched] - 1][index_into_batched]; // the last one in the batch
  // Let's send the read request corresponding to "to_reduce" requests
  pref_tuple cur_tuple;
  cur_tuple.cb_entry = index_into_batched; // info.cb_entry;
  // cur_tuple.second_buffer = info.second_buffer;
  cur_tuple.dfg_id = info.dfg_id;
  cur_tuple.core_type = coarseScratch;
  cur_tuple.edge.dst_id = 0; // for weight accesses..
  _asic->_scratch_ctrl->send_scratch_request(_core_id, leaf_id, cur_tuple);
}

void coalescing_buffer::send_batched_read_response(int index_into_batched)
{
  int to_reduce = min(int(_multicast_batch_size), _num_batched_tasks_per_index[index_into_batched]); // should have been 3?

  // statistics for average batching
  _total_current_entries -= to_reduce;
  _asic->_stats->_num_spatial_issue += 1;
  _asic->_stats->_num_temporal_issue++;
  _asic->_stats->_num_tasks_batched += to_reduce;
  _asic->_stats->_stat_batched_tasks += (to_reduce - 1);

  vector<iPair> mcast_dst_wgt;
  vector<int> mcast_dst_id;
  vector<int> mcast_dfg_id;
  // accessing the batched tasks from back (2 means 3,2) -- this might cause ROB
  // to wait for too long. (I need to fix that although shouldn't be an error)
  while (mcast_dst_wgt.size() < to_reduce)
  {

    int core = _data_of_batched_tasks[_num_batched_tasks_per_index[index_into_batched] - 1][index_into_batched].req_core;
    int cb_entry = _data_of_batched_tasks[_num_batched_tasks_per_index[index_into_batched] - 1][index_into_batched].cb_entry;
    int dfg_id = _data_of_batched_tasks[_num_batched_tasks_per_index[index_into_batched] - 1][index_into_batched].dfg_id;

    // TODO: need to pass dfg_id during multicast
    assert(core != -1 && cb_entry != -1 && "shouldn't have accessed a negative data element");
    remote_read_task data(-1, -1, -1);
    _data_of_batched_tasks[_num_batched_tasks_per_index[index_into_batched] - 1][index_into_batched] = data;
    mcast_dst_wgt.push_back(make_pair(core, 0));
    mcast_dst_id.push_back(cb_entry);
    mcast_dfg_id.push_back(dfg_id);
    --_num_batched_tasks_per_index[index_into_batched];
  }

  if (_num_batched_tasks_per_index[index_into_batched] == 0)
  {
    _batch_free_list.push(index_into_batched);
  }
  uint64_t line_addr = _asic->_mem_ctrl->_weight_offset; // should have stored network side
  DTYPE x[FEAT_LEN];
  net_packet net_tuple = _asic->_network->create_vector_update_packet(0, _core_id, mcast_dst_wgt, x); // _weight[c]);
  net_tuple.packet_type = vectorizedRead;
  net_tuple.multicast_dst_core_id.clear();
  net_tuple.multicast_dst_wgt.clear();
  net_tuple.multicast_dfg_id.clear();
  if (mcast_dst_wgt.size() > 0)
  {
    for (unsigned d = 0; d < mcast_dst_wgt.size(); ++d)
    {
      net_tuple.multicast_dst_core_id.push_back(mcast_dst_wgt[d].first);
      net_tuple.multicast_dst_wgt.push_back(make_pair(mcast_dst_id[d], 0)); // not stored with batching
      net_tuple.multicast_dfg_id.push_back(mcast_dfg_id[d]);
      // cout << "Dst core added it: " << net_tuple.multicast_dst_core_id[d] << endl;
    }
    _asic->_network->push_net_packet(_core_id, net_tuple);
    mcast_dst_wgt.clear();
    mcast_dst_id.clear();
    mcast_dfg_id.clear();
  }
}

void coalescing_buffer::update_priorities_using_cache_hit()
{
  return;
  for (auto it = _priority_update_queue.begin(); it != _priority_update_queue.end(); ++it)
  {
    for (auto it2 = (it->second).begin(); it2 != (it->second).end(); ++it2)
    {
      int leaf_id = it2->first;
      int pointer_into_batch = it2->second;
      int old_priority = it->first;
      int priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, (old_priority == 0), pointer_into_batch, leaf_id, _batch_width, _num_batched_tasks_per_index[pointer_into_batch]);
      // cout << "Old priority: " << old_priority << " new priority: " << priority << endl;
      if (priority != old_priority)
      {
        // push for a new priority
        if (_priority_update_queue.find(priority) == _priority_update_queue.end())
        {
          list<pair<int, int>> x;
          x.push_back(make_pair(leaf_id, it2->second));
          _priority_update_queue.insert(make_pair(priority, x));
        }
        else
        {
          (it->second).push_back(make_pair(leaf_id, it2->second));
        }
        // erase the curent one
        it2 = (it->second).erase(it2);
        if ((it->second).empty())
        {
          _priority_update_queue.erase(it->first);
        }
      }
    }
  }
}

coalescing_buffer::coalescing_buffer(asic *host, int core) : _asic(host), _core_id(core)
{
  // initialize weight matrix
  if (_asic->_config->_hats == 1 && _asic->_config->_central_batch == 0)
  {
    assert(_asic->num_kdtree_leaves < 16000 && "lookup bitset is so small");
    _is_leaf_present.reset();
    // _is_leaf_valid.set();
  }
  if (_asic->_config->_hats == 1)
  {
    int max_number_required = _batch_length; // 1+_asic->num_kdtree_leaves; // /core_cnt;
    _num_batched_tasks_per_index = (int *)malloc(max_number_required * sizeof(int));
    for (int i = 0; i < max_number_required; ++i)
    {
      _batch_free_list.push(i);
      _num_batched_tasks_per_index[i] = 0;
    }
    if (_asic->_config->_algo == gcn)
    {
      for (int i = 0; i < BATCH_WIDTH; ++i)
      {
        _data_of_batched_tasks[i] = (remote_read_task *)malloc(_batch_length * sizeof(remote_read_task));
        for (int j = 0; j < _batch_length; ++j)
        {
          remote_read_task data(-1, -1, -1);
          _data_of_batched_tasks[i][j] = data;
        }
      }
    }
  }
}

bool coalescing_buffer::pipeline_inactive(bool show)
{
  if (!_overflow_for_updates.empty())
  {
    if (show)
      cout << "Overflow queue for updates at core: " << _core_id << " is not empty: " << _overflow_for_updates.size() << endl;
    return false;
  }
  if (!_priority_update_queue.empty())
  {
    if (show)
      cout << "Priority update queue for core: " << _core_id << " is not empty: " << _priority_update_queue.size() << endl;
    return false;
  }
  return true;
}

void coalescing_buffer::fill_updates_from_overflow_buffer()
{
  while (_entries_in_priority_update < _batch_length && !_overflow_for_updates.empty() && !_batch_free_list.empty())
  {
    if (_asic->_config->_domain == tree)
    {
      int leaf_id = _overflow_for_updates.front().req_core;
      insert_leaf_task(leaf_id, -1, -1, -1);
    }
    else
    { // GCN
      assert(_asic->_config->_algo == gcn && "no other option");
      remote_read_task overflow_task = _overflow_for_updates.front();
      insert_leaf_task(overflow_task.leaf_id, overflow_task.req_core, overflow_task.cb_entry, overflow_task.dfg_id); // What if that dfg_id is no longer active? (2 dfg's, that shouldn't be messed up)
    }
    _overflow_for_updates.pop();
  }
}

// Need to insert a new task; so follow the above process
void coalescing_buffer::insert_leaf_task(int leaf_id, int req_core, int cb_entry, int dfg_id)
{
  bool cur_leaf_present = _is_leaf_present.test(leaf_id), cur_leaf_valid = false;
  int pointer_into_batch = -1;
  int old_priority = -1;
  if (cur_leaf_present)
  {
    for (auto it = _priority_update_queue.begin(); it != _priority_update_queue.end(); ++it)
    {
      for (auto it2 = (it->second).begin(); it2 != (it->second).end(); ++it2)
      {
        if (it2->first == leaf_id)
        { // should not be just leaf_id for a fixed length buffer
          old_priority = it->first;
          pointer_into_batch = it2->second;
          cur_leaf_valid = _num_batched_tasks_per_index[pointer_into_batch] < _batch_width;
          if (cur_leaf_valid)
            break; // it will chose the one that is valid
        }
      }
    }
  }
  ++_total_current_entries;

  remote_read_task data(req_core, dfg_id, cb_entry);
  if (cur_leaf_present && cur_leaf_valid)
  { // valid for width, present for length
    ++_num_batched_tasks_per_index[pointer_into_batch];
    _data_of_batched_tasks[_num_batched_tasks_per_index[pointer_into_batch] - 1][pointer_into_batch] = data;
    assert(_num_batched_tasks_per_index[pointer_into_batch] <= _batch_width && "should not have come in increment that");
  }
  else
  {
    if (_entries_in_priority_update < _batch_length && !_batch_free_list.empty())
    { // push a new entry for both scenarios
      // TODO: this may not be true?? priority_update has space but the free
      // list is not empty yet!!
      // assert(!_batch_free_list.empty() && "should have only tested for full batch size");
      int pointer_into_batch = _batch_free_list.front();
      assert(pointer_into_batch >= 0 && "batch free list cannot point to -ve element");
      _batch_free_list.pop();
      _is_leaf_present.set(leaf_id);
      int priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, cur_leaf_present, pointer_into_batch, leaf_id, _batch_width, _num_batched_tasks_per_index[pointer_into_batch]);
      auto it = _priority_update_queue.find(priority);

      if (it == _priority_update_queue.end())
      {
        list<pair<int, int>> x;
        x.push_back(make_pair(leaf_id, pointer_into_batch));
        _priority_update_queue.insert(make_pair(priority, x));
      }
      else
      {
        (it->second).push_back(make_pair(leaf_id, pointer_into_batch));
      }
      ++_entries_in_priority_update;
      ++_num_batched_tasks_per_index[pointer_into_batch];
      _data_of_batched_tasks[_num_batched_tasks_per_index[pointer_into_batch] - 1][pointer_into_batch] = data;
    }
    else
    { // push to the overflow buffer
      --_total_current_entries;
      if (_asic->_config->_domain == tree)
      { // if remote read, set the data to req_core
        remote_read_task data(leaf_id, -1, -1);
        _overflow_for_updates.push(data);
      }
      else
      {
        remote_read_task data(req_core, dfg_id, cb_entry);
        data.leaf_id = leaf_id;
        _overflow_for_updates.push(data);
      }
    }
  }
}

completion_buffer::completion_buffer()
{
  // TODO: separate for each task type? (or should the ratio of allotted size be predetermined or arbitrary?)
  _cb_size = COMPLETION_BUFFER_SIZE;
  for (int i = 0; i < COMPLETION_BUFFER_SIZE; ++i)
  {
    _reorder_buf[i].valid = false;
    _reorder_buf[i].waiting_addr = -1;
    _reorder_buf[i].last_entry = false;
  }
}

bool completion_buffer::pipeline_inactive(bool show)
{
  if (_cur_cb_push_ptr != _cur_cb_pop_ptr)
  {
    if (show)
      cout << "Pending memory reqs" << endl;
    return false;
  }
  if (_entries_remaining != COMPLETION_BUFFER_SIZE)
  {
    if (show)
      cout << "Completion buffer not empty" << endl;
    return false;
  }
  if (!_pref_lsq.empty())
  {
    if (show)
      cout << "Pref lsq not empty" << endl;
    return false;
  }
  // I do not think this is required
  // int part_size = ceil(FEAT_LEN*FEAT_LEN/float((line_size/message_size)));
  // int num_part = COMPLETION_BUFFER_SIZE/part_size;
  // if(_free_cb_parts.size()<num_part) { // all should be free
  //   if(show) cout << "No all completion buffers free cb parts is empty" << endl;
  //   return false;
  // }
  return true;
}

int completion_buffer::peek_lsq_dfg()
{
  pref_tuple cur_tuple = _pref_lsq.front();
  return cur_tuple.dfg_id;
}

pref_tuple completion_buffer::receive_lsq_responses()
{
  assert(!_pref_lsq.empty() && "cannot pop from an empty pref lsq buffer");
  // cout << "Previous size of pref lsq: " << _pref_lsq.size() << endl;
  pref_tuple cur_tuple = _pref_lsq.front(); // maybe not enough memory to save it??
  // cout << "New size of pref lsq: " << _pref_lsq.size() << endl;
  _pref_lsq.pop();
  return cur_tuple;
}

/*pref_tuple completion_buffer::insert_in_pref_lsq(pref_tuple next_tuple) {
  assert(_pref_lsq_remaining_entries>0 && "cannot assign entries when queue is full");
  _pref_lsq.push(next_tuple);
  _pref_lsq_remaining_entries--;
}*/

bool completion_buffer::can_push_in_pref_lsq()
{
  return (_pref_lsq.size() < FIFO_DEP_LEN);
}

// TODO: it can start from different access points...
void completion_buffer::cb_to_lsq()
{
#if GCN == 1 || PULL == 1
  if (_cb_start_partitions[0].size() > 0)
  {
    cbpart_to_lsq();
    return;
  }
#endif
  int init_start_point = _cur_cb_pop_ptr;
  for (int i = init_start_point; i < COMPLETION_BUFFER_SIZE; ++i)
  {
    if (!_reorder_buf[i].valid)
      return;
    if (!can_push_in_pref_lsq())
      return;
    _entries_remaining++;
    // This is for dummy entries for source vertices
    if (_reorder_buf[i].cur_tuple.src_id != -1)
    {
      _pref_lsq.push(_reorder_buf[i].cur_tuple);
    }
    else
    {
      // cout << "Skipped a reorder buffer entry due to negative src id\n";
      // assert(GRAPHMAT==1 && "source vertex accesses ");
    }

    _reorder_buf[i].cur_tuple.src_id = -1;
    _cur_cb_pop_ptr++;
    _reorder_buf[i].valid = false;
  }
  _cur_cb_pop_ptr = 0; // if it came here, then it is cycling around
  for (int i = 0; i < init_start_point; ++i)
  {
    if (!_reorder_buf[i].valid)
      return;
    if (!can_push_in_pref_lsq())
      return;
    // This is for dummy entries for source vertices
    if (_reorder_buf[i].cur_tuple.src_id != -1)
    {
      _pref_lsq.push(_reorder_buf[i].cur_tuple);
    }
    else
    {
      // assert(GRAPHMAT==1 && "source vertex accesses ");
      // cout << "Skipped a reorder buffer entry due to negative src id\n";
    }
    // _pref_lsq.push(_reorder_buf[i].cur_tuple);
    _entries_remaining++;
    _reorder_buf[i].cur_tuple.src_id = -1;
    _cur_cb_pop_ptr = i + 1;
    _reorder_buf[i].valid = false;
  }
}

// assign partitions instead of current cb pointer (it should store
// .first+index)
// Oh god, this will not allow to serve in sequence...
void completion_buffer::cbpart_to_lsq()
{
#if GCN == 0
  assert(INTERTASK_REORDER == 1 && "should come here only when task reorder is allowed");
#endif
  for (int d = 0; d < MAX_DFGS; ++d)
  {
    for (unsigned c = 0; c < _cb_start_partitions[d].size(); ++c)
    {
      // if all valid, then only push
      bool all_found = true;
      if (_cb_start_partitions[d][c].second < _cb_start_partitions[d][c].first)
      {
        // if(_cb_start_partitions[d][c].second<_cur_start_ptr[d][c]) {
        _cb_start_partitions[d][c].second += COMPLETION_BUFFER_SIZE;
        assert(GCN == 1 && "other graph workloads should have partitions multiple of size");
      }
      int pref_entries_required = _cb_start_partitions[d][c].second - _cb_start_partitions[d][c].first;
      // int pref_entries_required = _cb_start_partitions[d][c].second-_cur_start_ptr[d][c];
      assert(pref_entries_required <= FIFO_DEP_LEN && "an ROB partition cannot require more spaces than available");
      // TODO: this should be less or just use very large FIFO size...
      if (_pref_lsq.size() > abs(FIFO_DEP_LEN - pref_entries_required))
      {
        all_found = false;
      }
      // TODO: keep a start pointer to serve as soon as available, just pop when all are found
      for (int i = _cb_start_partitions[d][c].first; i < _cb_start_partitions[d][c].second && all_found; ++i)
      {
        // for(int i=_cur_start_ptr[d][c]; i<_cb_start_partitions[d][c].second && all_found; ++i) {
        int index = i % COMPLETION_BUFFER_SIZE;
        if (!_reorder_buf[index].valid)
        {
          all_found = false;
          break;
        }
        if (_reorder_buf[index].last_entry)
        {
          break;
        }
      }
      // FIXME: do not want to serve unless all are available since they should come in sequence for accumulation (this is possible in SIMT)
      if (!all_found)
      {
        continue;
      }
      if (all_found)
      {
        bool last_entry_occured = false;
        int correct_entries = 0;
        int i = 0;
        for (i = _cb_start_partitions[d][c].first; i < _cb_start_partitions[d][c].second; ++i)
        {
          // for(i=_cur_start_ptr[d][c]; i<_cb_start_partitions[d][c].second; ++i) {
          int index = i % COMPLETION_BUFFER_SIZE;
          if (!_reorder_buf[index].valid)
          {
            break;
          }
          if (!last_entry_occured)
          {
            ++correct_entries;
            ++_entries_remaining;
            _pref_lsq.push(_reorder_buf[index].cur_tuple);
          }
          if (_reorder_buf[index].last_entry)
          {
            assert(!last_entry_occured && "only 1 entry should be last");
            last_entry_occured = true;
          }
          _reorder_buf[index].cur_tuple.src_id = -1;
          _reorder_buf[index].valid = false;
          _reorder_buf[index].waiting_addr = -1;
          _reorder_buf[index].last_entry = false;
        }
        if (all_found)
        {
          assert(last_entry_occured && "last entry should definitely have occured if all accesses fit in the current partition");
          _free_cb_parts.push(make_pair(_cb_start_partitions[d][c].first, false));
          // _cur_start_ptr[d][c] = _cb_start_partitions[d][c].first;
        }
        else
        {
          assert(0 && "not required without using cur start ptr");
          assert(!last_entry_occured && "last entry should not occur for half served entries");
          _cur_start_ptr[d][c] = i;
        }
      }
    }
  }
}

// #if GCN==0
//         int num_part = COMPLETION_BUFFER_SIZE/MAX_DEGREE; // 25
//         assert(_free_cb_parts.size() <= num_part && "free partitions should be less than max avail");
//         // FIXME: how is this pushed when nobody allocated any entries to it?
//         /*if(c==0 && d==0) { // c is 12th partition here and not core
//           cout << "pushed a new free cb part with start partitions: " << _cb_start_partitions[d][c].first << " correct entries: " << correct_entries << endl;
//         }*/
// #endif

// 31, 0, 1, 2 (cbpush=3)
int completion_buffer::allocate_cb_entry(int waiting_count)
{
  int cb_entry = _cur_cb_push_ptr;
  // cout << "Allocated space at cb entry: " << _cur_cb_push_ptr << " count: " << waiting_count << endl;
  assert(waiting_count > 0);
  assert(_entries_remaining > 0 && "seems like it allocated entries multiple times");
  _reorder_buf[_cur_cb_push_ptr].waiting_addr = waiting_count;
  assert(!_reorder_buf[_cur_cb_push_ptr].valid && "cb should be free while allocating");
  _entries_remaining--;
  _cur_cb_push_ptr = (_cur_cb_push_ptr + 1) % COMPLETION_BUFFER_SIZE;
  return cb_entry;
}

int completion_buffer::deallocate_cb_entry(int waiting_count)
{
  int cb_entry = (_cur_cb_push_ptr - 1) % COMPLETION_BUFFER_SIZE;
  if (cb_entry < 0)
    cb_entry += COMPLETION_BUFFER_SIZE;
  assert(waiting_count > 0);
  _reorder_buf[cb_entry].waiting_addr = 0;
  _reorder_buf[cb_entry].valid = false;
  _entries_remaining++;
  _cur_cb_push_ptr = (_cur_cb_push_ptr - 1) % COMPLETION_BUFFER_SIZE;
  if (_cur_cb_push_ptr < 0)
    _cur_cb_push_ptr += COMPLETION_BUFFER_SIZE;
  return cb_entry;
}

bool completion_buffer::can_push(int reqd_entries)
{
  return _entries_remaining >= reqd_entries;
}

bool completion_buffer::can_push()
{
  return _entries_remaining > 0;
}

void asic::call_simulate()
{
  // in the new model -- just call simulate_slice();
  switch (_config->_exec_model)
  {
  case async_slicing:
    simulate_slice(); // simulate_sgu_slice();
    break;
  case sync_slicing:
    simulate_slice(); // simulate_graphmat_slice();
    break;
  // case blocked_async: simulate_slice(); // simulate_blocked_async_slice();
  case blocked_async:
    simulate_blocked_async_slice();
    break;
  case dyn_graph:
    simulate_dyn_graph();
    break;
  default:
    simulate();
    break;
  }
}

void asic::reset_algo_switching()
{
  _cur_cycle = 0;
  _global_iteration = 0;
  _mem_ctrl->_l2hits = 0;
  _mem_ctrl->_l2accesses = 0;
  _switched_async = false;
  _switched_cache = false;
  _stats->_stat_tot_finished_edges = 0;
  _stats->_stat_global_finished_edges = 0;

  _stats->_stat_online_tasks = 0;
}

// Sol2: Assign timestamp to E/2 edges. Then, traverse in sequence.
void asic::assign_rand_tstamp()
{
  // int tot_parts=4;
  int edges_assigned[8];
  for (int i = 0; i < _dyn_tot_parts; ++i)
    edges_assigned[i] = 0;
  for (int i = 0; i < _dyn_tot_parts; ++i)
    _tot_map_ind[i] = i;

  int temp_partitions = _dyn_tot_parts;
  int half_edge = _graph_edges / 2;
  // assert(half_edge<=_dyn_batch_size*_dyn_tot_parts && "otherwise matching doesn't work");

  for (int i = 0; i < half_edge; ++i)
  {
    // Oh it can just search for the groups left
    int temp_stamp = rand() % temp_partitions;
    temp_stamp = _tot_map_ind[temp_stamp];
    _assigned_tstamp[i] = temp_stamp;
    edges_assigned[temp_stamp]++;
    // for the last, partition we can let it have more edges
    if (temp_partitions > 1 && edges_assigned[temp_stamp] == _dyn_batch_size)
    { // iteration size...a new parameter (should be send while simulation
      temp_partitions--;
      for (int i = _tot_map_ind[temp_stamp]; i < (temp_partitions); ++i)
      {
        _tot_map_ind[temp_stamp] = _tot_map_ind[temp_stamp + 1];
      }
    }
  }
  for (int i = 0; i < _dyn_tot_parts; ++i)
    _tot_map_ind[i] = 0;
  for (int i = 0; i < half_edge; ++i)
  {
    _tot_map_ind[_assigned_tstamp[i]]++;
  }
  // save the original neighbor array
  for (int i = 0; i < _graph_edges; ++i)
  {
    // _original_neighbor[i]=_neighbor[i]; // actually can just save the pointer
    _original_neighbor[i].src_id = _neighbor[i].src_id;
    _original_neighbor[i].wgt = _neighbor[i].wgt;
    _original_neighbor[i].dst_id = _neighbor[i].dst_id;
  }
  cout << "Timestamps assigned to half edges" << endl;
  int tot = 0;
  for (int i = 0; i < _dyn_tot_parts; ++i)
  {
    cout << "Partition: " << i << " timestamp assigned: " << _tot_map_ind[i] << endl;
    tot += _tot_map_ind[i];
  }
  assert(tot == E / 2 && "total assigned edges should be equal to half graph");
}

// TODO: also need to initialize properly
// Update data structure: Insert as
// normal when you find edges of current id. Fill offset array as usual. New
// edge array is required.
void asic::rand_edge_update_stream(int part)
{
  int half_edge = E / 2;
  // int cur_tstamp = (cur_edges-half_edge)/_dyn_batch_size;
  // cur_tstamp=cur_tstamp-1;
  int cur_tstamp = part - 1;

  // calculate the new edges
  _graph_edges = half_edge;
  for (int i = 0; i <= cur_tstamp; ++i)
  {
    _graph_edges += _tot_map_ind[i];
  }
  // -----------------

  // cout << "Came to insert edges with current timestamp: " << cur_tstamp << endl;
  int prev_vid = 0;
  int count = 0;
  // insert static edges
  if (_graph_edges == half_edge)
  {
    for (int i = 0; i < half_edge; ++i)
    {
      if (_original_neighbor[i].src_id != prev_vid)
      { // Is this wrong?
        prev_vid = _original_neighbor[i].src_id;
        // _offset[prev_vid]=count;
      }
      // _neighbor[count].src_id = _original_neighbor[i].src_id;
      // _neighbor[count].dst_id = _original_neighbor[i].dst_id;
      // _neighbor[count].wgt = _original_neighbor[i].wgt;
      ++count;
    }
    task_entry cur_task(0, _offset[0]);
    push_pending_task(0, cur_task);

    for (int i = prev_vid + 1; i <= _graph_vertices; ++i)
      _offset[i] = _offset[i - 1];
    _half_vertex = prev_vid;
    return;
  }
  else
  {
    prev_vid = _half_vertex;
    count = half_edge;
  }

  // insert new dynamic edges
  // TODO: currently we consider that other vertices are dangling
  _inc_comp = true;
  int impacted_vertices = 0;
  int corner = E;
  if ((E % 2) == 1)
    corner -= 1;
  cout << "corner case edges: " << corner << endl;
  for (int i = half_edge; i < corner; ++i)
  { // insert all previous timestamps
    // for(int i=half_edge; i<E; ++i) { // insert all previous timestamps
    if (_assigned_tstamp[i - half_edge] <= cur_tstamp)
    {
      _neighbor[count].src_id = _original_neighbor[i].src_id;
      _neighbor[count].dst_id = _original_neighbor[i].dst_id;
      _neighbor[count].wgt = _original_neighbor[i].wgt;
      int cur_src = _original_neighbor[i].src_id;
      if (cur_src != prev_vid)
      {
        assert(_original_neighbor[i].src_id > prev_vid && "offset should be filled in sorted order of source vertices");
        for (int middle = prev_vid + 1; middle <= _original_neighbor[i].src_id; ++middle)
        {
          _offset[middle] = count;
        }
        prev_vid = cur_src;
      }
      // initialize the source vertex
#if INC_DYN == 1
      int vert_activated = 0;
      if ((_assigned_tstamp[i - half_edge] == cur_tstamp) && cur_src != vert_activated)
      {
        vert_activated = cur_src;
        // cout << "prev vid: " << prev_vid << " and the new vertex data: " << _vertex_data[prev_vid] << endl;
        if (_vertex_data[cur_src] != MAX_TIMESTAMP)
        {
          DTYPE priority = apply(_vertex_data[cur_src], cur_src);
          task_entry cur_task(cur_src, _offset[cur_src]);
          ++impacted_vertices;
          assert(priority > 0);
          push_pending_task(priority, cur_task);
        }
      }
#endif
      ++count;
    }
  }
  for (int i = prev_vid + 1; i <= _graph_vertices; ++i)
    _offset[i] = count;
  cout << "cur edges: " << _graph_edges << " offset: " << _offset[_graph_vertices] << " count: " << count << endl;
  assert(_offset[V] == _graph_edges && "for correct offset array, final should point to the the current graph edges");
  cout << "New impacted vertices are: " << impacted_vertices << endl;

  // But then why incorrect answer?
  // TODO: Check how many edges should be traversed in ideal dijkstra?
}

void asic::seq_edge_update_stream(int cur_edges, int prev_edges, int original_graph_vertices)
{
  // Update data-structure (offset array) to include more edges
  // Step1: fix the previous offsets
  _cur_used_vert = 0;
  for (int i = prev_edges; i < cur_edges; ++i)
  {
    if (_neighbor[i].src_id != _cur_used_vert)
    {
      _cur_used_vert = _neighbor[i].src_id;
      _offset[_cur_used_vert] = i;
    }
  }
  // Step2: reset the new offsets after prev_vid
  for (int removed = _cur_used_vert + 1; removed < (original_graph_vertices + 1); ++removed)
  {
    _offset[removed] = cur_edges;
  }
}

// TODO: does it work for both sync/async? are challenges different for those?
// FIXME: destination should not have vertices from the non-added vertex or
// vertices are not added, only edges. OKay let me check methodology in prior work
// Case 1: only edges: change the offset array for all later vertices,
// graph_vertices still same
// Case 2: both: add vertices and the edges corresponding to those only (would
// require complicated modifications to the graph structure)
// Conditions: should be non-sliced
void asic::simulate_dyn_graph()
{
  int original_graph_edges = _graph_edges;
  int original_graph_vertices = _graph_vertices;
  int vert_prev_comp = 0;
  int prev_edges = 0;
  // TODO: When inserting next edges seqeuntially, both slices will have sufficient reuse if both impl (what if we create a new slice for all inserted edges? is the assumption that they will not depend much on the previous computation?)
  // Sol1: Let's slicing be METIS and vertices be assigned in vid (they do not
  // change numbering right?).
  // Sol3: use some other src_id.
  // Impl: store original graph in another array, store edges in random order. Each edge will then be inserted such that sorted order is maintained. Oh I see the simulator should be flexible to different data-structure implementations of the graph
  // into the current structure
#if SEQ_EDGE == 0
  assign_rand_tstamp();
#endif
  for (int cur_edges = original_graph_edges / 2, iter = 0; iter < (_dyn_tot_parts + 1) && cur_edges < original_graph_edges; cur_edges += _dyn_batch_size, ++iter)
  {
#if SEQ_EDGE == 1
    seq_edge_update_stream(cur_edges, prev_edges, original_graph_vertices);
    prev_edges = cur_edges;
    _graph_edges = cur_edges; // graph vertices should remain same
#else
    // rand_edge_update_stream(cur_edges, prev_edges, original_graph_vertices);
    rand_edge_update_stream(iter);
#endif
    // FIXME: it uses scratch and then initializes at the end
    fill_correct_vertex_data();
#if RECOMP_DYN == 1
    // TODO: Anything else to reset?
    // seems to have stuck after a point in next computation
    init_vertex_data();
#else
    assert(INC_DYN == 1 && "currently we only support recomp and incremental computation");
    // Activate all the impacted vertices with the new edges (it includes all
    // source vertices for push? dest will be updated anyways)
    // prev_vid -- current total vertices in the graph.
    if (vert_prev_comp != 0)
    { // we do not need to activate if it is the first case
      int impacted_vertices = 0;
#if SEQ_EDGE == 0
      // nothing...we already inserted there!
#else
      for (int v = vert_prev_comp; v < _cur_used_vert; ++v)
      {
        if (_vertex_data[v] == MAX_TIMESTAMP)
          continue;
        DTYPE priority = apply(_vertex_data[v], v);
        task_entry cur_task(v, _offset[v]);
        // TODO: if there was no way to reach this vertex earlier, an edge
        // outgoing from here will not impact solution. Hence, it does not need
        // to be activated.
#if SGU_SLICING == 1 || GRAPHMAT_SLICING == 1
        int wklist = _scratch_ctrl->get_slice_num(v);
        _task_ctrl->push_task_into_worklist(wklist, priority, cur_task);
#else
        _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(v), priority, cur_task);
#endif
        ++impacted_vertices;
      }
      cout << "Vertices activated in incremental computation: " << impacted_vertices << endl;
#endif
    }
    vert_prev_comp = _cur_used_vert;
#endif

#if SEQ_EDGE == 1
    if (cur_edges == original_graph_edges / 2)
    {
      // insert new vertices
      task_entry cur_task(0, _offset[0]);
#if SGU_SLICING == 1 || GRAPHMAT_SLICING == 1
      int wklist = _scratch_ctrl->get_slice_num(0);
      _task_ctrl->push_task_into_worklist(wklist, 0, cur_task);
#else
      // Reasons that they might not be pushed? same task already exists?
      _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(0), 0, cur_task);
#endif
    }
#endif
    cout << "Initializing re/inc-computation with current edges: " << _graph_edges << endl;
    // " added vertices: " << (_cur_used_vert-vert_prev_comp) << endl;
    // FIXME: here source vertex doesn't need to be activated, but if it is, any
    // problem? (maybe for PR/CC)
#if SGU_SLICING == 1
    simulate_sgu_slice();
#elif GRAPHMAT_SLICING == 1
    simulate_graphmat_slice();
#else
    simulate();
#endif
    // not sure if we need this?
    for (int i = 0; i < _graph_vertices; ++i)
    {
      assert(_vertex_data[i] == _scratch[i] && "not sure why they may not be equal");
      // _scratch[i]=_vertex_data[i];
    }
    cout << "Done with the computation at cycle: " << _cur_cycle << " and total finished edges: " << _stats->_stat_tot_finished_edges << endl;
    cout << "Done with the computation with total cycles: " << (_global_iteration * _graph_vertices * 8 / 512 + _cur_cycle) << endl;
    reset_algo_switching();
  }
}

asic::asic()
{
  _graph_vertices = V;
  _graph_edges = E;
  for (int c = 0; c < core_cnt; ++c)
  {
    _fine_alloc[c] = 1;
    _coarse_alloc[c] = 1;
  }
  assert(UPDATE_BATCH <= 16 && "currently we have not allowed storage for batches smaller that e/16");
  _dyn_batch_size = E / UPDATE_BATCH;
  _dyn_tot_parts = UPDATE_BATCH / 2; // partitions=8?
  cout << "Setting batch size to be: " << _dyn_batch_size << " and partitions: " << _dyn_tot_parts << endl;
  _config = new config(this);
  if (_config->is_async())
  {
    if (_config->_pull_mode)
    {              // 10 infinite simulation, 15 nan too late
      _gama *= 10; // 0.01 (100), 0.005 (50), 0.001 (10)
    }
    else
    {
#if FIFO == 1
      _gama *= 5000;
#else
      _gama *= 1000;
#endif
    }
  }
  else if (_config->_blocked_async)
  { // blocked async
    _gama *= 100;
  }
  else
  {
    _gama *= 1;
  }
  if (_config->_hats == 1)
  {
    assert(BATCH_WIDTH >= MULTICAST_BATCH && "no point of having a multicast wider than the buffer width");
  }
  if (_config->_algo == gcn)
  {
    _gcn_minibatch = (int *)malloc(V * sizeof(int));
  }
  _gcn_updates = (int *)malloc(core_cnt * num_banks * sizeof(int));
  _prev_gcn_updates = (int *)malloc(core_cnt * num_banks * sizeof(int));
  if (_config->_domain == tree)
  {
    assert(num_kdtree_leaves < 469 && "leaf done metric for only compulsory misses is incorrect");
    assert(2 * num_kdtree_leaves < V && "we cannot study on a tree smaller than the required kdtree");
    _is_leaf_done.reset();
    if (_config->_hats == 1 && _config->_central_batch == 1)
    {
      _leaf_access_count = (int *)malloc(num_kdtree_leaves * sizeof(int));
      for (int i = 0; i < num_kdtree_leaves; ++i)
      {
        _leaf_access_count[i] = 0;
      }
    }
  }

  // required for balancing edges
  if (_config->_dyn_load_bal == 1)
  {
    for (int i = 0; i < core_cnt / 4; ++i)
    {
      _last_cb_core[i] = 0;
    }
  }

  for (int c = 0; c < core_cnt; ++c)
  {
    _last_bank_per_core[c] = -1;
  }

#if DYN_GRAPH == 1 && SEQ_EDGE == 0
  int dyn_edges = _graph_edges / 2;
  _assigned_tstamp = (int *)malloc(dyn_edges * sizeof(int));
  _original_neighbor = (edge_info *)malloc(_graph_edges * sizeof(edge_info));
  // _original_neighbor = (edge_info**)malloc(_graph_edges*sizeof(edge_info*));
#endif

#if MODEL_NET_DELAY == 1
#if NETWORK == 0
  _bank_queue_to_mem_latency = log2(process_thr * core_cnt);
#else
  _bank_queue_to_mem_latency = CROSSBAR_LAT; // log2(process_thr);
#endif
#endif
#if CROSSBAR_LAT > 0 && NETWORK == 0
  _bank_queue_to_mem_latency = CROSSBAR_LAT;
#endif
  assert(_bank_queue_to_mem_latency < MAX_NET_LATENCY && "net latency is greater than max allowed 16");
  // TODO: how to model crossbar packets that can be traversed more?
  _xbar_bw = CROSSBAR_BW;
  // cout << "Calculated bank to queue latency: " << _bank_queue_to_mem_latency << endl;

  /* for(int i=0; i<_graph_vertices; ++i) {
        done = (bitset<SLICE_COUNT>*)malloc(_graph_vertices*sizeof(bitset<SLICE_COUNT>));
    }*/

#if ABCD == 1
  for (int i = 0; i < SLICE_COUNT; ++i)
    _grad[i] = 0;
#endif

#if PROFILING == 1
  for (int i = 0; i < core_cnt; ++i)
  {
    _inc_freq[i] = (int *)malloc(_graph_vertices * sizeof(int));
    for (int v = 0; v < _graph_vertices; ++v)
    {
      _inc_freq[i][v] = 0;
    }
  }
  _map_vertex_to_core = (int *)malloc(_graph_vertices * sizeof(int));
  _edge_freq = (int *)malloc(_graph_edges * sizeof(int));
  for (int e = 0; e < _graph_edges; ++e)
    _edge_freq[e] = 0;
#endif

#if GRAPHMAT_SLICING == 1 && ALLHITS == 1
  uint64_t num_cache_lines = _graph_edges / (line_size / message_size);
  _is_edge_hot_miss = (bool *)malloc(num_cache_lines * sizeof(bool));
#endif

  _slice_count = SLICE_COUNT;
  // #if SGU_SLICING==1 || GRAPHMAT_SLICING==1
  //   _slice_count=SLICE_COUNT;
  // #else
  //   _slice_count=1;
  // #endif

  for (int i = 0; i < _slice_count; ++i)
  {
    _slice_fill_addr[i] = i * _graph_vertices / _slice_count;
  }

  _neighbor = (edge_info *)malloc(_graph_edges * sizeof(edge_info));
  _correct_vertex_data = (int *)malloc(_graph_vertices * sizeof(int));
  _offset = (int *)malloc((_graph_vertices + 1) * sizeof(int));
#if (MULT_AGG_TYPE > 0) || (AGG_MULT_TYPE > 0)
  _correct_vertex_data_double_buffer = (int *)malloc(_graph_vertices * sizeof(int));
#endif

#if PULL == 1
  _csr_neighbor = (edge_info *)malloc(_graph_edges * sizeof(edge_info));
  _csr_offset = (int *)malloc((_graph_vertices + 1) * sizeof(int));
#endif

#if CF == 0
  _vertex_data = (DTYPE *)malloc(_graph_vertices * sizeof(DTYPE));
  // _scratch = (DTYPE*)malloc(SCRATCH_SIZE*sizeof(DTYPE));
  _scratch = (DTYPE *)malloc((_graph_vertices + 10) * sizeof(DTYPE));
#else
  for (int i = 0; i < FEAT_LEN; ++i)
  {
    _vertex_data_vec[i] = (DTYPE *)malloc(_graph_vertices * sizeof(DTYPE));
    _scratch_vec[i] = (DTYPE *)malloc((_graph_vertices + 10) * sizeof(DTYPE));
  }
#endif

#if NETWORK == 0
  _tot_banks = num_banks;
  _banks_per_core = num_banks / core_cnt;
#else
  _tot_banks = num_banks * core_cnt;
  _banks_per_core = num_banks;
#endif

  for (int i = 0, end = core_cnt * num_banks; i < end; ++i)
  {
    _gcn_updates[i] = 0;
    _prev_gcn_updates[i] = 0;
  }

#if LADIES_GCN == 1
  for (int i = 0; i < LADIES_SAMPLE; i++)
    _is_dest_visited.reset(i);
#endif

#if BFS_MAP == 1
  // Mark all the vertices as not visited
  for (int i = 0; i < _graph_vertices; i++)
    _bfs_visited[i] = false;
    /*_cluster_elem_done = (int*)malloc(_graph_vertices*sizeof(int));
    for(int i = 0; i < _graph_vertices; i++) 
        _cluster_elem_done[i] = 0;
    */

#endif

#if 0 // GRAPHMAT_SLICING==1
  _mismatch_slices = (int*)malloc(_graph_vertices*sizeof(int));
  _counted = (bitset<_slice_count>*)malloc(_graph_vertices*sizeof(bitset<_slice_count>));
  for(int i=0; i<_graph_vertices; ++i) {
    _mismatch_slices[i]=0;
    for(int j=0; j<_slice_count; ++j) {
      _counted[i].reset(j);
    }
  }
#endif
    // need to initialize params using .ini file first

    // ofstream stat_file;
    // stat_file.open("default.ss-stats", ofstream::trunc | ofstream::out);
    // CS_graph_verticesWriter *mem_csv_out;
    // dram = new MemorySystem(1, 12800, *mem_csv_out, stat_file);

    // _neighbor = (edge_info*)malloc(e*sizeof(edge_info));
    // for (int i = 0; i < E; i++)
    //   _edge_traversed[i] = false;

#if GRAPHLAB == 1
  _update = (bool *)malloc(_graph_vertices * sizeof(bool));
  _atomic_issue_cycle = (int *)malloc(_graph_vertices * sizeof(int));
  for (int i = 0; i < _graph_vertices; ++i)
  { // not sure if needed
    _atomic_issue_cycle[i] = -2;
  }
#endif
  _network = new network(this);
  _stats = new stats(this);
  // cout << "Came here to assign cores to the system\n";
  // Task type 1 (put for other task types)
  for (int i = 0; i < core_cnt; ++i)
  {
#if GRAPHMAT == 1 || BLOCKED_ASYNC == 1
    _asic_cores[i] = new graphmat(this);
#elif SGU == 1
    _asic_cores[i] = new sgu(this);
#elif GRAPHLAB == 1
    _asic_cores[i] = new graphlab(this);
#elif DIJKSTRA == 1
    _asic_cores[i] = new dijkstra(this);
#elif SWARM == 1
    _asic_cores[i] = new swarm(this); // with inorder cores
#elif ESPRESSO == 1
    _asic_cores[i] = new espresso(this); // sgu-kind arrangement
#else
    cout << "Specify execution model!!\n";
    exit(0);
#endif
    _asic_cores[i]->_core_id = i;
    // if(_config->_algo==gcn)
    // Initialize cores with coarse-grained task

    _remote_coalescer[i] = new coalescing_buffer(this, i);
    _local_coalescer[i] = new coalescing_buffer(this, i);

    _mult_cores[i] = new multiply(this);
    _mult_cores[i]->_core_id = i;
    _compl_buf[i] = new completion_buffer();
    _compl_buf[i]->_core_id = i;
    // if(_config->is_vector()) {
    for (int d = 0; d < MAX_DFGS; ++d)
    {
      _coarse_reorder_buf[i][d] = new completion_buffer();
      _coarse_reorder_buf[i][d]->_core_id = i;
      _coarse_compl_buf[i][d] = new completion_buffer();
      _coarse_compl_buf[i][d]->_core_id = i;
    }
    // }
    // if(_config->_pull_mode) {
    _fine_reorder_buf[i] = new completion_buffer();
    _fine_reorder_buf[i]->_core_id = i;
    // }
  }

  _mem_ctrl = new memory_controller(this, _stats);
  _task_ctrl = new task_controller(this);
  _scratch_ctrl = new scratch_controller(this, _stats);

  if (_config->_algo == gcn && _config->_hats == 1 && _config->_inter_task_reorder == 1)
  { // static reconfiguration
    int part_size = ceil(FEAT_LEN * FEAT_LEN / float((line_size / message_size)));
    int num_part = COMPLETION_BUFFER_SIZE / part_size; // 32/16=2
    for (int c = 0; c < core_cnt; ++c)
    {
      for (int d = 0; d < MAX_DFGS; ++d)
      {
        for (int n = 0; n < num_part; ++n)
        {
          _coarse_reorder_buf[c][d]->_cb_start_partitions[d].push_back(make_pair(n * part_size, (n + 1) * part_size));
          _coarse_reorder_buf[c][d]->_free_cb_parts.push(make_pair(n * part_size, false));
        }
      }
    }
  }

  // split completion buffer in different partitions
#if INTERTASK_REORDER == 1
#if GCN == 0
  if (_config->_pull_mode == 1 && !_config->is_vector())
  {
    int part_size = MAX_DEGREE;                        // 16384
    int num_part = COMPLETION_BUFFER_SIZE / part_size; // 25
#else
  if (1)
  {
    int part_size = ceil(FEAT_LEN * FEAT_LEN / float((line_size / message_size)));
    int num_part = COMPLETION_BUFFER_SIZE / part_size; // 32/16=2
#endif

    for (int c = 0; c < core_cnt; ++c)
    {
      for (int n = 0; n < num_part; ++n)
      {
        _fine_reorder_buf[c]->_cb_start_partitions[0].push_back(make_pair(n * part_size, (n + 1) * part_size));
        _fine_reorder_buf[c]->_cur_start_ptr[0].push_back(n * part_size);
        _fine_reorder_buf[c]->_free_cb_parts.push(make_pair(n * part_size, false));
      }
    }
  }
#endif
}

// Does it need to travel over the network to know that it is a hit/miss -- of
// course? asic_core::serve_atomic_requests (and should be called from
// network.cpp instead of here)
// Here, caches are not private that would help.
void asic::vertex_memory_access(red_tuple cur_tuple)
{

  // cout << "Executing an update with vid: " << cur_tuple.dst_id << " cycle: " << _cur_cycle << endl;
  // cout << "Qid: " << cur_tuple.tid << " being pushed to banks in time: " << (_cur_cycle-_stats->_start_cycle_per_query[cur_tuple.tid]) << endl;

  // cout << "Came in vertex memory access with vid: " << cur_tuple.dst_id << "\n";
  int dest_core = _scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id);
  int bank_id_local = _scratch_ctrl->get_local_bank_id(cur_tuple.dst_id);
  // #if NETWORK==0
  int bank_id_global = _scratch_ctrl->get_global_bank_id(cur_tuple.dst_id);

  // FIXME: this is weird!!!
  if (_config->_net == mesh && _config->_perfect_lb == 1)
  {
    if (_config->_prac == 0)
    {
      _last_bank_per_core[dest_core] = (_last_bank_per_core[dest_core] + 1) % num_banks;
      bank_id_local = _last_bank_per_core[dest_core];
      _last_core = dest_core;
      // _last_core = (_last_core+1)%core_cnt;
      // dest_core = _last_core;
      // cout << "Core: " << dest_core << " chosen bank: " << bank_id_global << endl;
    }
    else
    {
      _last_bank = (_last_bank + 1) % _tot_banks;
      bank_id_global = _last_bank;
    }
  }
// #endif
/* FIXME: for SGU
 * finishing phase is shared with SGU
 * for cross-slice vertices, and working-cache=0, they need go to memory
 */
#if GRAPHMAT_SLICING == 1
  if (!_finishing_phase && !_switched_cache)
  {
#else
  if (1)
  {
#endif
#if WORKING_CACHE == 0 && ALL_CACHE == 0
    // cout << "Cycle: " << _cur_cycle << " dest core:  " << dest_core << " local bank id: " << bank_id_local << " global bank id: " << bank_id_global << endl;
#if NETWORK == 1
    if (_config->_perfect_lb == 0 && num_banks > 16)
    { // scale to num_banks
      bank_id_local = bank_id_local * (num_banks / 16) + rand() % 16;
    }
    // cout << "Executing an update with vid: " << cur_tuple.dst_id << " cycle: " << _cur_cycle << " to bank: " << bank_id_local << endl;
    _asic_cores[dest_core]->_local_bank_queues[bank_id_local].push(cur_tuple);
#else

#if XBAR_ABORT == 1 && PRIO_XBAR == 0
    if (_scratch_ctrl->_present_in_bank_queue[bank_id_global].test(cur_tuple.dst_id) == 1)
    {
      bool came_in = false;
      for (auto it = _bank_queues[bank_id_global].begin(); it != _bank_queues[bank_id_global].end(); ++it)
      {
        if (it->dst_id == cur_tuple.dst_id)
        {
          came_in = true;
          it->new_dist = reduce(cur_tuple.new_dist, it->new_dist);
          break;
        }
      }
      assert(came_in && "should find the old dst id in the bank queue");
      _stat_bank_coalesced++;
      return;
    }
    else
    {
      // cout << "Set dst id: " << cur_tuple.dst_id << endl;
      _scratch_ctrl->_present_in_bank_queue[bank_id_global].set(cur_tuple.dst_id);
      insert_global_bank(bank_id_global, cur_tuple.new_dist, cur_tuple);
      // _bank_queues[bank_id_global].push_back(cur_tuple);
    }
#elif PRIO_XBAR == 1 && XBAR_ABORT == 1
    if (_scratch_ctrl->_present_in_bank_queue[bank_id_global].test(cur_tuple.dst_id) == 1)
    {
      bool came_in = false;
      for (auto it = _bank_queues[bank_id_global].begin(); it != _bank_queues[bank_id_global].end();)
      {
        for (auto it2 = (*it).second.begin(); it2 != (*it).second.end();)
        {
          if (it2->dst_id == cur_tuple.dst_id)
          {
            if (cur_tuple.new_dist < it2->new_dist)
            {
              // cout << "Ersing entry for dst id: " << cur_tuple.dst_id << " at cucle: " << _cur_cycle << endl;
              came_in = true;
              it2 = (*it).second.erase(it2);
              if ((*it).second.empty())
              { // if nothing left in list
                it = _bank_queues[bank_id_global].erase(it);
                break;
              }
            }
            else
            {
              return;
            }
          }
          else
          {
            ++it2;
          }
        }
        if (came_in)
          break;
      }
      _stat_bank_coalesced++;
    }
    // cout << "Pushing entry for dst id: " << cur_tuple.dst_id << " at cucle: " << _cur_cycle << endl;
    _scratch_ctrl->_present_in_bank_queue[bank_id_global].set(cur_tuple.dst_id);
    insert_global_bank(bank_id_global, cur_tuple.new_dist, cur_tuple);
#else
    insert_global_bank(bank_id_global, cur_tuple.new_dist, cur_tuple);
#endif

#endif
    return;
#endif
  }

  // cout << "In graphmat slicing, pushing bank: " << bank_id2 << "\n";
  uint64_t paddr = _graph_edges * 4 + cur_tuple.dst_id * 4; // byte-addres
  // bool hit = _asic->_l2_cache->LookupAndFillCache(0, 0, paddr, ACCESS_LOAD, true, 0); // check cache statistics
  bool l2hit = false;

  // if boundary vertex, optimistically consider it as a hit
  if (_current_slice != _scratch_ctrl->get_slice_num(cur_tuple.dst_id))
  {
    l2hit = true;
  }
  else
  {
    if (!_mem_ctrl->_banked)
    {
      l2hit = _mem_ctrl->_l2_cache->LookupCache(paddr, ACCESS_LOAD, true, 0);
    }
    else
    {
      l2hit = _mem_ctrl->_banked_l2_cache[dest_core]->LookupCache(paddr, ACCESS_LOAD, true, 0);
    }
  }

  // #if GRAPHMAT_SLICING==1 && REUSE>1 && WORKING_CACHE==0
  if (_config->_graphmat_slicing && _config->_reuse > 1 && _config->_working_cache == 0)
  {
    if (_finishing_phase)
    {
      l2hit = true; // send to memory
      assert(_finishing_phase && "sending to memory only when finishing phase otherwise scratch");
    }
  }

  if (_config->_allhits)
  {
    l2hit = _config->_allhits;
  }
  _mem_ctrl->_l2accesses++;
  if (!l2hit)
  { // if(0)
    // TODO: send the real requests instead of just latency...
    pref_tuple update_tuple;
    update_tuple.update_width = -1;
    update_tuple.src_id = cur_tuple.dst_id;
    update_tuple.src_dist = cur_tuple.new_dist;
    update_tuple.req_core = _scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id);
    update_tuple.cb_entry = -1; // unordered packet
    bool sent_request = _mem_ctrl->send_mem_request(false, paddr, update_tuple, VERTEX_SID);
    if (!sent_request)
      _mem_ctrl->_l2hits++;
    _stats->_stat_pending_cache_misses++;
    _mem_ctrl->_num_pending_cache_reqs++;
  }
  else
  {
    _mem_ctrl->_l2hits++;
    if (_config->_network)
    {
      _asic_cores[dest_core]->_local_bank_queues[bank_id_local].push(cur_tuple);
    }
    else
    {
      insert_global_bank(bank_id_global, cur_tuple.new_dist, cur_tuple);
      // _bank_queues[bank_id_global].push_back(cur_tuple);
    }
  }
  // cout << "Searched in cache hit:: " << l2hit << "\n";
}

bool asic::can_push_to_bank_crossbar_buffer()
{
  return _pending_updates < (FIFO_DEP_LEN * core_cnt * process_thr);
}

void asic::execute_func(int src_core, int func, red_tuple cur_tuple)
{
  if (_config->_model_net_delay == 0)
  {
    graphmat_slice_configs(cur_tuple);
    return;
  }
  int delay = _bank_queue_to_mem_latency;
  // Delay should be different based on what core it is
  if (_config->_hrc_xbar)
  {
    assert(NETWORK == 0 && src_core != -1);
    // it has come here after network
    int dest_core = _scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id);
    if (src_core == dest_core)
    {
      graphmat_slice_configs(cur_tuple);
      return;
      // delay=log2(process_thr);
    }
    else
    {
      // Let's push these remote tasks in a buffer that will serve only half of them
      // delay = log2(core_cnt)+log2(process_thr);
      // delay = 16*_network->calc_hops(src_core, dest_core);
      delay = _network->calc_hops(src_core, dest_core);
      assert(delay < MAX_NET_LATENCY && "should not be delayed than max net latency");
    }
    // else delay = log2(core_cnt)+log2(process_thr);
    // else delay = log2(process_thr);
  }

  switch (func)
  {
  case BANK_TO_XBAR:
    _bank_to_crossbar[(_bank_xbar_ptr + delay) % MAX_NET_LATENCY].push(cur_tuple);
    ++_pending_updates;
    break;
  case ROUTER_XBAR_TO_BANK:
    delay = 2;
    _router_crossbar[(_router_xbar_ptr + delay) % MAX_NET_LATENCY].push(cur_tuple);
    break;
  default:
    assert(0 && "Should not have come here");
    break;
  }
  _delayed_net_tasks++;
}

// In fifo, it would get ready in order always. So, my technique is not so bad.
// FIXME: NET: fix the practical crossbar modeling
// FIXME: NET: fix the size of banks in a crossbar
void asic::cycle_pending_comp()
{
  // to a single dest, it can push only 1
  bitset<num_banks> dest_done;
  // TODO: @vidushi: it should check whether bank have space or not...!!!
  // dest_done.reset();
  while (!_bank_to_crossbar[_bank_xbar_ptr].empty())
  {
    red_tuple cur_tuple = _bank_to_crossbar[_bank_xbar_ptr].front();
    _bank_to_crossbar[_bank_xbar_ptr].pop();
    --_pending_updates;
    /* int dst_core = _scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id);
     * int bank_id = _scratch_ctrl->get_global_bank_id(cur_tuple.dst_id);
    if(dest_done.test(bank_id)==1) {
      int new_ptr = (_bank_xbar_ptr+1)%MAX_NET_LATENCY;
      _bank_to_crossbar[new_ptr].push(cur_tuple); // FIXME: ideally this should be in the reverse direction
      continue;
    }
    dest_done.set(bank_id);
    */
    graphmat_slice_configs(cur_tuple);
    _delayed_net_tasks--;
  }
  /*while(!_bank_to_crossbar[_bank_xbar_ptr].empty()) {
    graphmat_slice_configs(_bank_to_crossbar[_bank_xbar_ptr].front());
    _bank_to_crossbar[_bank_xbar_ptr].pop();
    _delayed_net_tasks--;
  }*/
  while (!_router_crossbar[_router_xbar_ptr].empty())
  {
    // FIXME.... (push to internal should be replaced by execute func)
    // _network->encode_scalar_packet(_router_crossbar[_router_xbar_ptr].front(), cur_core);
    _router_crossbar[_router_xbar_ptr].pop();
    _delayed_net_tasks--;
  }
  _bank_xbar_ptr = (_bank_xbar_ptr + 1) % MAX_NET_LATENCY;
  _router_xbar_ptr = (_router_xbar_ptr + 1) % MAX_NET_LATENCY;
}

double asic::randdouble()
{
  double t;
  t = rand() / (double)RAND_MAX;
  return t;
}

// I am not sure how would the crossbar be pipelined, oh yeah maybe multiplexers could be modeled that way
// TODO: is this causing update? the function name sounds weird??
void asic::graphmat_slice_configs(red_tuple cur_tuple)
{
  // TODO: perform this after a delay of N cycles
  // for a crossbar to bank queue
  // execute_func(graphmat_slice_configs, cycle+7);
  // mesh to bank queue: same, delay would be for 16x16 crossbar:execute_func(graphmat_slice_config, cycle+4);
  // for internal router, execute_func(push_next_loc, cycle+2);

  if (_config->_reuse_stats == 1)
  {
    _stats->_stat_owned_vertex_freq_per_itn[cur_tuple.dst_id]++;
  }

  _tot_mem_reqs_per_iteration += 4; // for each vertex update

  // applicable for all sync-only, blocked-async, async
  // cout << "Total: " << _tot_mem_reqs_per_iteration << endl;
  assert(cur_tuple.new_dist >= 0 && "before sending to banks, it is correct");
  if (_config->_graphmat_slicing == 0 || _config->_reuse == 1)
  {
    vertex_memory_access(cur_tuple);
    return;
  }
  if (_finishing_phase || _switched_cache)
  {
    // tot_finished will involve both...here i just split by phases
    if (_finishing_phase)
      _stats->_stat_sync_finished_edges++;
    if (_switched_cache)
      _stats->_stat_extreme_finished_edges++;
    vertex_memory_access(cur_tuple);
  }
  else
  {
    vertex_memory_access(cur_tuple);
    int dest_slice_id = _scratch_ctrl->get_slice_num(cur_tuple.dst_id);
    if (dest_slice_id != _current_slice)
    {
      _delta_updates[_current_reuse].push_back(make_pair(cur_tuple.dst_id, cur_tuple.new_dist));
    }
  }
}

void asic::init_vertex_data()
{
  if ((_config->_ladies_gcn || _config->_gcn) && _config->_agg_mult_type != global)
  {
    for (int i = 0; i < _graph_vertices; ++i)
    {
      DTYPE value = (_scratch_ctrl->_in_degree[i]) * FEAT_LEN * message_size / bus_width;
      _correct_vertex_data[i] = value;
      if (_config->_mult_agg_type != global)
      {
        _correct_vertex_data_double_buffer[i] = value;
      }
    }
    return;
  }

  if (_config->_acc)
  {
    for (int i = 0; i < _graph_vertices; ++i)
    {
      _scratch[i] = i;
      _vertex_data[i] = i;
      _correct_vertex_data[i] = 0; // for fully connected graph
    }
    for (int i = 0; i < _graph_edges; ++i)
    {
      _neighbor[i].wgt = 0;
    }
    return;
  }
  if (_config->_cf)
  {
    for (int i = 0; i < _graph_vertices; ++i)
    {
      for (int j = 0; j < FEAT_LEN; ++j)
      {
        _vertex_data_vec[j][i] = randdouble() * sqrt(3.0 / FEAT_LEN);
        if (_config->is_sync())
        {
          _scratch_vec[j][i] = 0;
          ;
        }
        else
        { // for blocked async as well...
          _scratch_vec[j][i] = randdouble() * sqrt(3.0 / FEAT_LEN);
        }
      }
    }
    print_cf_mean_error();
  }
  else
  {
    if (_config->_algo == pr)
    {
      if (_config->_pull_mode)
      {
        for (int i = 0; i < _graph_vertices; ++i)
        {
          _vertex_data[i] = (1 - alpha);
        }
      }
      else
      {
        for (int i = 0; i < _graph_vertices; ++i)
        {
          _vertex_data[i] = 0;
        }
        for (int i = 0; i < _graph_edges; ++i)
        {
          edge_info e = _neighbor[i];
          int out_degree = _offset[e.src_id + 1] - _offset[e.src_id];
          if (out_degree == 0)
            continue; // cout << e.src_id << endl;
          assert(out_degree > 0);
          _vertex_data[e.dst_id] += 1 / (float)out_degree; // e.wgt/(sum_inc);
        }
        float avg = 0;
        for (int i = 0; i < _graph_vertices; ++i)
        {
          assert(_vertex_data[i] >= 0);
          _vertex_data[i] = alpha * (1 - alpha) * _vertex_data[i];
          avg += _vertex_data[i];
          // if(_vertex_data[i]==0) cout << "i: " << i << " vertex has no incoming" << endl;
          // cout << "assigned: " << _vertex_data[i] << endl;
        }
        cout << "Average vertex data: " << avg / (double)_graph_vertices << endl;
      }
    }
    else
    {
      float constant = MAX_TIMESTAMP - 1;
      for (int i = 0; i < _graph_vertices; ++i)
      {
        _vertex_data[i] = constant; // INF;
        // cout << "Allocated vertex_data at i: " << i << " is: " << _vertex_data[i] << " and max_timestamp-1 is: " << constant << endl;
      }
      cout << "SRC VID: " << _src_vid << " distance to 0 and with out-degree: " << (_offset[_src_vid + 1] - _offset[_src_vid]) << "\n";
      _vertex_data[_src_vid] = 0; // src
    }
    // initialize cache information
    /*for(int i=0; i<E; ++i) {
      _last_accessed_cycle[i] = 0;
    }*/
    // need to fill scratch with the same data
    for (int i = 0; i < _graph_vertices; ++i)
    {
      _scratch[i] = _vertex_data[i];
    }
  }
}

int asic::get_degree(int vid)
{
  return (_offset[vid + 1] - _offset[vid]);
}

void asic::read_graph_structure(string file, int *offset, edge_info *neighbor, bool initialize_vd, bool is_csc_file)
{
  FILE *graph_file = fopen(file.c_str(), "r");

  char linetoread[5000];

  cout << "start reading graph input file with graph vertices: " << _graph_vertices << " and edges: " << _graph_edges << "!\n";

  offset[0] = 0;
  int prev_offset = 0;
  int e = -1, prev_v = -1; // indices start from 1
  bool first = true;
  // if csc file, always go the csr format
  while (fgets(linetoread, 5000, graph_file) != NULL)
  {
    std::string raw(linetoread);
    std::istringstream iss(raw.c_str());
    std::istringstream iss2(raw.c_str());
    char x;
    iss2 >> x;
    if (x == '%')
      continue;
    int src, dst, wgt;
    // cout << "string read: " << raw << endl;
    wgt = 0;
    // char ignore;
    if (is_csc_file || _graph_vertices == 23947347 || _graph_vertices == 6262104)
    {
      iss >> src >> dst >> wgt;
      // if(_config->_algo!=cf) {
      //   wgt = rand()%256;
      // }
      // cout << src << " " << dst << " " << wgt << endl;
      /*if(_graph_vertices==6262104) {
        cout << "src: " << src << " dst: " << dst << " wgt: " << wgt << endl;
      }*/
    }
    else if (_graph_vertices == 224000)
    {
      string temp;
      int index = 0;
      while (std::getline(iss, temp, ','))
      {
        stringstream token(temp);
        if (index == 0)
        {
          token >> src;
        }
        else if (index == 1)
        {
          token >> dst;
        }
        else if (index == 2)
        {
          token >> wgt;
        }
        ++index;
      }
      // cout << raw << " src: " << x << " dst: " << y << " wgt: " << z << endl;
      src = src - 1;
      dst = dst - 1;
    }
    else if (_graph_vertices == 82000 || _graph_vertices == 2700)
    { // for the movielens dataset (normal for pull mode)
      // char ignore;
      int extra;
      iss >> src >> dst >> wgt >> extra;
      src -= 1;
      dst -= 1;
      /*int l=0, token;
      while(std::getline(iss, token, "::")) {
        switch(l) {
          case 0: src = token;
                  break;
          case 1: dst = token;
                  break;
          case 2: wgt = token;
                  break;
          default: break;
        }
      }*/
    }
    else if (_graph_vertices == 403394)
    {
      iss >> dst >> src;
      src = src - 1;
      dst = dst - 1;
      wgt = 1;
    }
    else
    {
      if (_graph_vertices == 1632803)
      {
        iss >> src >> dst;
        wgt = 1;
        src = src - 1;
        dst = dst - 1;
#if PR == 0 && ABFS == 0
        wgt = rand() % 256;
#endif
      }
      else
      {
        if (_graph_vertices == 44906 || _graph_vertices == 1965206 || _graph_vertices == 4847571 || _graph_vertices == 2708 || _graph_vertices == 1048576)
        {
          iss >> src >> dst;
          wgt = 1; // for bfs, for sssp just make it random with fixed seed
          if (!_config->_pr && !_config->_abfs)
          {
            wgt = rand() % 256;
          }
        }
        else if (_graph_vertices == 3774768 || _graph_vertices == 2997166 || _graph_vertices == 896308 || _graph_vertices == 41652230 || _graph_vertices == 23947347)
        { // cit-Patents, orkut, imdb, usa_road
          if ((_graph_vertices == 41652230 || _graph_vertices == 23947347) && first)
          {
            iss >> dst;
            cout << "At first, destination read was: " << dst << endl;
            if (dst == _graph_vertices)
            {
              cout << "dst was _graph_vertices for mtx format\n";
              first = false;
              continue;
            }
          }
          iss >> dst >> src;
          if (src == _graph_vertices)
            continue;
          dst = dst - 1;
          src = src - 1;
          wgt = 1;
          if (!_config->_pr && !_config->_abfs)
          {
            wgt = rand() % 256;
          }
          // cout << "src: " << src << " dst: " << dst << " wgt: " << wgt << endl;
        }
        else
        {
          if (_graph_vertices == 7414866)
          { // matrix-market format
            /*if(_graph_vertices==23947347) {
              char l;
              // iss >> l >> dst >> src >> wgt;
              iss >> dst >> src;
            } else {
              iss >> dst >> src;
              wgt=1;
            }*/
            iss >> dst >> src;
            wgt = 1;

            dst = dst - 1;
            src = src - 1;
            // cout << "Read dst: " << dst << " src: " << src << endl;
            if (!_config->_pr && !_config->_abfs)
            {
              wgt = rand() % 256;
            }
          }
          else
          {

            iss >> src >> dst >> wgt;
            /*if(_graph_vertices==2097152 || _graph_vertices==4194301 || _graph_vertices==8388606 || _graph_vertices==16777211) {
              src = src-1;
              wgt = wgt-1;
            }*/
            // cout << "src: " << src << " dst: " << dst << " wgt: " << wgt << endl;
          }
        }
      }
    }
    // _scratch_ctrl->_in_degree[dst]++;

    neighbor[++e].dst_id = dst; // dst according to 0 index
    if (dst >= _graph_vertices)
    {
      cout << "Dst: " << dst << " graph vertices: " << _graph_vertices << endl;
    }
    assert(dst < _graph_vertices);
    if (!_config->_abfs || _config->_ladies_gcn)
    {
      neighbor[e].wgt = wgt;
    }
    else
    {
      neighbor[e].wgt = 1;
    }
    // srand(0);
    // _neighbor[e].wgt=1+rand()%8;
    // _neighbor[e].wgt=1;
    neighbor[e].src_id = src;
    // _incoming_edgeid[dst].push_back(e);
    // _neighbor[e].done = false;

    // _neighbor[e].id=e;
    // FIXME: still not sure if this correct

    if (_config->_pull_mode && initialize_vd)
    {
      int read_src = src;
      src = dst; // also used for writing offset
      neighbor[e].dst_id = read_src;
      neighbor[e].src_id = src;
    }
    // cout << "edge id: " << e << " src_id: " << neighbor[e].src_id << " dst_id: " << neighbor[e].dst_id
    // << " weight: " << neighbor[e].wgt << endl;
    /*
    0 1 // offset[0]=0;
    0 2
    1 3 // offset[1]=2
    3 4 // offset[2]=3, offset[3]=3, offset[4]=4
    */
    if (src != prev_v)
    {
      offset[prev_v + 1] = e; // shouldn't this be src??
      // cout << (prev_v+1) << " OFFSET: " << e << endl;
      int k = src + 1;
      while (offset[--k] == 0 && k > 0)
      {
        offset[k] = e; // prev_offset;
        // cout << k << " OFFSET: " << e << endl;
      }
      prev_offset = e;
      prev_v = src;
    }
    // cout << _neighbor[e].wgt << " " << _neighbor[e].dst_id << " " << _offset[prev_v-1] << endl;
  }
  offset[_graph_vertices] = _graph_edges; // +1;
  int k = _graph_vertices;
  while (offset[--k] == 0 && k > 0)
  {                           // offset[0] should be 0
    offset[k] = _graph_edges; // prev_offset; // _offset[_graph_vertices]; // prev_offset;
  }
  /*int k=prev_v+1; // _graph_vertices;
  for(k=prev_v+1; k<=_graph_vertices; ++k) {
      offset[k] = _graph_edges;
  }*/
  fclose(graph_file);
  cout << "Done reading graph file!\n";
  /*cout << " csc_offset: " << is_csc_file << endl;
  for(int i=0; i<=_graph_vertices; ++i){
    cout << "Offset at i: " << i << " is: " << offset[i] << endl;
  }*/

  // not required..
  // #if PR==1  && CF==0// edge wgt here is 1/out_degree
  //   for(int i=0; i<_graph_edges; ++i) {
  //     edge_info e = neighbor[i];
  //     // find out degree of the source node
  //     int out_degree = offset[e.src_id+1]-offset[e.src_id];
  //     neighbor[i].wgt = 1/(DTYPE)out_degree;
  //   }
  // #endif

  if (initialize_vd)
  {
    init_vertex_data();
    cout << "Done initializing vertex data!\n";
    cout << "First edge src: " << _neighbor[0].src_id << " dst_id: " << _neighbor[0].dst_id << " wgt: " << _neighbor[0].wgt << endl;
  }
  // for(int i=0; i<_graph_edges; ++i) assert(_neighbor[i].dst_id<_graph_vertices);

  // check the number of connected components in the twitter graph
#if 0
  int start_vert=0;
  int num_cc=0;
  while(_vertices_visited<_graph_vertices) {
        DFS(start_vert, 0);
        num_cc++;
        for(int i=0; i<_graph_vertices; ++i) {
            if(!_visited[i]) {
                start_vert=i;
                continue;
            }
        }
    }
  cout << "Number of cc: " << num_cc << endl;
  exit(0);
#endif
#if 0 // GRAPHMAT_SLICING==1 // collect graph statistics required for calculation
  for(int i=0; i<E; ++i) {
    edge_info e = _neighbor[i];
    int v = e.src_id;
    int dst_slice = get_slice_num(e.dst_id);
    if(_counted[v].test(dst_slice)) continue;
    _mismatch_slices[v]++;
    _counted[v].set(dst_slice);
    /*if(get_slice_num(e.src_id)!=dst_slice) {
      _mismatch_slices[v]++;
    }*/
  }
  // for(int i=0; i<_graph_vertices; ++i) cout << "mismatch for i: " << i << " is: " << _mismatch_slices[i] << endl;
  int tot_mismatch=0;
  for(int i=0; i<_graph_vertices; ++i) tot_mismatch += _mismatch_slices[i];
  cout << "Mismatch slices: " << tot_mismatch << endl;
  free(_counted);
#endif

  // exit(0);
}

// This works according to the push implementation, so it should use the
// corresponding data-structures
int asic::fill_correct_vertex_data()
{
#if PR == 0
  int *offset = _offset;
  edge_info *neighbor = _neighbor;
  int edges_traversed = 0;
  if (_config->_pull_mode)
  {
    offset = _csr_offset;
    neighbor = _csr_neighbor;
  }
  // let me try dijkstra here
  // Initializations just for SSSP, PR
  _scratch_ctrl->_is_visited.reset();
  float constant = MAX_TIMESTAMP - 1;
  for (int i = 0; i < _graph_vertices; ++i)
  {
    _correct_vertex_data[i] = constant; // INF;
  }
  _correct_vertex_data[_src_vid] = 0;
  _scratch_ctrl->_is_visited.set(_src_vid);
  //-----------------
  // timestamp, vertex
  priority_queue<iPair, vector<iPair>, greater<iPair>> pq;
  pq.push(make_pair(0, _src_vid));

  while (!pq.empty())
  {
    int src_id = pq.top().second;
    pq.pop();
    if (_scratch_ctrl->_is_visited.test(src_id) == 0)
    {
      _stats->_stat_vertices_done++;
    }
    _scratch_ctrl->_is_visited.set(src_id);
    /*if(_graph_edges<E) {
      cout << "COMMIT NEW VERTEX: " << src_id << " and its outgoing vertices: " << (_offset[src_id+1]-_offset[src_id]) << endl;
    }*/
    // because no is_visited condition
    for (int i = offset[src_id]; i < offset[src_id + 1]; ++i)
    {
      int dst_id = neighbor[i].dst_id;
      if (_scratch_ctrl->_is_visited[dst_id])
        continue;
      int weight = neighbor[i].wgt;
      edges_traversed++;

      // cout << "dst_id: " << dst_id << " weight: " << weight << " src dist: " << _correct_vertex_data[src_id] << " dst dist: " << _correct_vertex_data[dst_id] << endl;

      // process+relax step (algo specific)
      int temp_dist = _correct_vertex_data[src_id] + weight;
      if (_correct_vertex_data[dst_id] > temp_dist)
      {
        _correct_vertex_data[dst_id] = temp_dist;
        pq.push(make_pair(_correct_vertex_data[dst_id], dst_id));
      }
    }
    // if(_is_visited[src_id]) continue;
  }
  _stats->_stat_vertices_done++;
  cout << "Vertices marked: " << _stats->_stat_vertices_done << endl;
  cout << "Edges traversed during dijkstra: " << edges_traversed << endl;
  cout << "Percentage edges traversed during dijkstra: " << edges_traversed / (double)_graph_edges << endl;
  /*for(int i=0; i<_graph_vertices; ++i) {
    _correct_vertex_data[i] = _scratch[i];
  }*/
  _stats->_stat_vertices_done = 0;
  _scratch_ctrl->_is_visited.reset();
  // init_vertex_data();
  return edges_traversed;
#endif
  return 0;
}

bool asic::correctness_check()
{
  if (_config->_domain == true)
    return true;
#if CF == 0
  for (int i = 0; i < _graph_vertices; ++i)
  {
    _vertex_data[i] = _scratch[i];
  }
  for (int i = 0; i < _graph_vertices; ++i)
  {
    if (_vertex_data[i] != _correct_vertex_data[i])
    {
      cout << "vertex: " << i << " calculated: " << _vertex_data[i] << " corrected: " << _correct_vertex_data[i] << endl;
      return false;
    }
  }
  return true;
#endif
  return true;
}

void asic::print_output()
{
  return;
#if CF == 0
  int error = 0;
  for (int i = 0; i < _graph_vertices; ++i)
  {
    // if(_correct_vertex_data[i]!=_vertex_data[i]) {
    if (_correct_vertex_data[i] != _scratch[i])
    {
      // if(_correct_vertex_data[i]==MAX_TIMESTAMP-1 && _scratch[i]==MAX_TIMESTAMP) {
      if (_correct_vertex_data[i] > 10000)
      {
        continue;
      }
      error++;
      cout << "ERROR::";
      cout << i << " " << _correct_vertex_data[i] << ":" << _vertex_data[i] << endl;
      // if(error>10) break;
    }
  }
  // if(error==293402) cout << "CORRECT ANSWER, INF is large\n";
  // else cout << "Error in: " << error << " vertices\n";
#endif
}

// TODO: for tree domain, generate multiple queries for the same source (randomness in chosing the edge should solve the problem)
// But the input needs to be a tree
void asic::initialize_simulation()
{

  _mod_offset = (int *)malloc((_graph_vertices + 1) * sizeof(int));
  _mod_offset[0] = 0;
  for (int i = 1; i <= V; ++i)
  {
    int correct_vid = _scratch_ctrl->_mapping[i - 1];
    int degree = _offset[correct_vid + 1] - _offset[correct_vid];
    _mod_offset[i] = _mod_offset[i - 1] + degree;
  }
  // first fill the cache with the first slice
  if (_config->_working_cache || _config->_hybrid || _config->_domain == tree)
  {
    _mem_ctrl->initialize_cache();
  }

  if (_config->_dramsim_enabled)
  {
    // load_graph_in_memory();
  }

  // TODO: push initial tasks (this is explicit enqueue from the control core)
  int allotted_core = _scratch_ctrl->get_local_scratch_id(_src_vid);
  task_entry start_task(_src_vid, _offset[_src_vid]);

  int lane_count[core_cnt];
  for (int i = 0; i < core_cnt; ++i)
    lane_count[i] = 0;

  if (!_config->is_slice())
  { // sliced execution thing is done separately
    if (_config->is_non_frontier())
    {
#if GCN == 1
      for (int i = 0; i < MINIBATCH_SIZE; ++i)
      {
        int v = _gcn_minibatch[i]; // _mapping[i];
                                   // cout << "pushing vertex: " << v << endl;
#else
      for (int i = 0; i < _graph_vertices; ++i)
      {
        int v = i;
#endif
        // cout << "inserted vertex v: " << v << endl;
        if (i % 10000 == 0)
          cout << "10000 vertices inserted" << endl;
        if (_offset[v + 1] == _offset[v])
          continue; // if outgoing degree==0
        // TODO: vidushi: only allowed for dangling vertices
        if (_config->_algo == pr && _vertex_data[v] == 0)
          continue;

        allotted_core = _scratch_ctrl->get_local_scratch_id(v);
        task_entry cur_task(v, _offset[v]);
        _virtual_finished_counter++;

        DTYPE priority = 0;
#if CF == 0
        priority = apply(_vertex_data[v], v);
        cur_task.dist = _vertex_data[v];
#endif

        if (_config->no_swarm())
        {
          _task_ctrl->insert_new_task(_asic_cores[allotted_core]->_agg_push_index, 0, allotted_core, priority, cur_task);
        }
        else
        {
          _task_ctrl->insert_new_task(_asic_cores[allotted_core]->_agg_push_index, lane_count[allotted_core], allotted_core, priority, cur_task);

          lane_count[allotted_core]++;
          lane_count[allotted_core] = lane_count[allotted_core] % LANE_WIDTH;
        }
        _asic_cores[allotted_core]->_agg_push_index += 1;
        _asic_cores[allotted_core]->_agg_push_index %= NUM_TQ_PER_CORE;
      }
    }
    else
    {
      start_task.dist = 0;
      _task_ctrl->insert_new_task(0, 0, 0, 0, start_task);
    }
  }
  _prev_commit_id = 0;
  _prev_commit_core_id = 0;
  if (_config->is_non_frontier())
    _virtual_finished_counter = 1;
}

void asic::update_stats_per_cycle()
{
  for (int i = 0; i < core_cnt; ++i)
  {
    _stats->_stat_fine_gran_lb_factor[i] = 0;
    _stats->_stat_fine_gran_lb_factor_atomic[i] = 0;
  }

  for (int c = 0; c < core_cnt; ++c)
  {
    // cout << _stats->_stat_enq_this_cycle[c] << " ";
    _stats->_stat_tot_enq_per_core[c] += _stats->_stat_enq_this_cycle[c];
    if (_stats->_stat_enq_this_cycle[c] != 0)
      _tot_active_cycles[c]++;
    _stats->_stat_enq_this_cycle[c] = 0;
  }
}

void asic::print_simulation_status()
{
  // FIXME: checking: remove
  /*_asic_cores[0]->_fine_grain_throughput=16*_config->_process_thr*2; // (32-cores_to_agg); // 2;
  for(int c=1; c<core_cnt; ++c) {
    _asic_cores[c]->_fine_grain_throughput=0;
  }*/
  assert(_stats->_stat_pending_cache_misses >= 0 && "there cannot be negative cache misses");
  cout << "Cur Cycle: " << _cur_cycle << " and finished coarse tasks: " << _stats->_stat_finished_coarse_tasks
       << " with finished edges: " << _stats->_stat_tot_finished_edges / (FEAT_LEN / 16) << " and online tasks: " << _stats->_stat_online_tasks << " pushed to pending tasks: " << _stats->_stat_pushed_to_ps << " copy vertex updates: " << _stats->_stat_tot_num_copy_updates << " pending cache misses: " << _stats->_stat_pending_cache_misses << " pending coalesces entries: " << _stats->_stat_pending_colaesces << " free cb entries: " << _compl_buf[0]->_entries_remaining << " coarse-grained cb entries: " << _coarse_compl_buf[0][0]->_entries_remaining << " coarse-reorder buffer entries: " << _coarse_reorder_buf[0][0]->_entries_remaining << endl;
  print_cf_mean_error();

  if (_config->_algo == gcn || _config->_domain == tree)
  {
    cout << "Created fine first buffer tasks: " << _stats->_stat_tot_created_edge_tasks << " created second-buffer: " << _stats->_stat_tot_created_edge_second_buffer_tasks
         << " created first-coarse tasks: " << _stats->_stat_tot_created_data_parallel_tasks << " created coarse second-buffer: " << _stats->_stat_tot_created_double_buffer_data_parallel_tasks << endl;

    cout << "Finished fine first buffer tasks: " << _stats->_stat_tot_finished_first_buffer_edge_tasks << " finished second-buffer: " << _stats->_stat_tot_finished_second_buffer_edge_tasks
         << " finished first-coarse tasks: " << _stats->_stat_tot_finished_first_buffer_data_parallel_tasks << " finished coarse second-buffer: " << _stats->_stat_tot_finished_second_buffer_data_parallel_tasks << endl;
  }

  // TODO: duplicate: move to print_local_iteration_stats()
  if (_cur_cycle > 100000 && _stats->_stat_prev_finished_edges == _stats->_stat_tot_finished_edges)
  {
    // it checked all the data, how could it do anything else??
    cout << "Number of edges done in the previous round is 0\n";
    bool done = false;
    cout << "Checking accelerator cores\n";
    for (int c = 0; c < core_cnt; ++c)
    {
      done = _asic_cores[c]->pipeline_inactive(true);
      done = _mult_cores[c]->pipeline_inactive(true);
    }
    cout << "Checking network\n";
    done = _network->buffers_not_empty(true);
    cout << "Let's break now\n";
    // exit(0);
  }
  _stats->_stat_prev_finished_edges = _stats->_stat_tot_finished_edges;

  /*for(int c=0; c<core_cnt; ++c) {
    cout << "Core: " << c << " matrix: " << _mult_cores[c]->_stat_vec_mat_local << " ";
  }
  cout << endl;
  */
  /*
  for(int c=0; c<core_cnt; ++c) {
    cout << "Core:" << c << "finished_edges: " << _stats->_stat_finished_edges_per_core[c] << " ";
  }
  cout << endl;
  */

  if (_config->_cache_hit_aware_sched == 1 && _config->_domain == graphs)
  {
    for (int c = 0; c < core_cnt; ++c)
    {
      cout << "Core: " << c << " ";
      cout << "Occupancy priority-hit-queue " << _stats->_stat_tot_priority_hit_updates_occupancy[c]
           << " hit-queue " << _stats->_stat_tot_hit_updates_occupancy[c]
           << " miss-queue " << _stats->_stat_tot_miss_updates_occupancy[c]
           << " prefetch-process " << _stats->_stat_tot_prefetch_updates_occupancy[c] << " ";
      cout << "Served priority-hit-queue " << _stats->_stat_tot_priority_hit_updates_served[c]
           << " hit-queue " << _stats->_stat_tot_hit_updates_served[c]
           << " miss-queue " << _stats->_stat_tot_miss_updates_served[c] << endl;
    }
    _stats->reset_cache_hit_aware_stats();
  }

  // TODO: for sync implementation that have some sort of barrier, we do not want to execute other things
  // apply homogeneity at varying times..
  // Should not go there during barrier
  /*if(_config->is_sync()) {
    return;
  }*/

  if (1)
  { // _cur_cycle%100000==0) {
    // cout << "Cur Cycle: " << _cur_cycle << " and finished coarse tasks: " << _stats->_stat_finished_coarse_tasks << " with finished edges: " << _stats->_stat_tot_finished_edges << " and online tasks: " << _stats->_stat_online_tasks << " pushed to pending tasks: " << _stats->_stat_pushed_to_ps << " copy vertex updates: " << _stats->_stat_tot_num_copy_updates << " pending cache misses: " << _stats->_stat_pending_cache_misses << " pending coalesces entries: " << _stats->_stat_pending_colaesces << " free cb entries: " << _compl_buf[0]->_entries_remaining << " coarse-grained cb entries: " << _coarse_compl_buf[0]->_entries_remaining << " coarse-reorder buffer entries: " << _coarse_reorder_buf[0]->_entries_remaining  << endl;
    /*for(int i=0; i<core_cnt; ++i) {
    cout << " core  i: " << i << " entries remaining: " << _compl_buf[i]->_entries_remaining << endl;
  }*/
    int agg_oflow_entries = 0;
    for (int i = 0; i < core_cnt; ++i)
    {
      agg_oflow_entries += _asic_cores[i]->_fifo_task_queue.size();
    }
    cout << "Task queue overflow entries at core 0: " << agg_oflow_entries << " and active network packets: " << _network->_active_net_packets << " direction-optimizing benefit: " << _stats->_stat_edges_dropped_with_break << endl;
  }
  // cout << "Task queue overflow entries at core 0: " << _asic_cores[0]->_fifo_task_queue.size() << " and active network packets: " << _network->_active_net_packets << " direction-optimizing benefit: " << _stats->_stat_edges_dropped_with_break << endl;
  // when it is called at this time, stats is lost somewhere...
  // print_local_iteration_stats(_cur_cycle/100000, 0);
  print_local_iteration_stats(_cur_cycle / STATS_INTERVAL, 0);

  _stats->_stat_tot_created_edge_tasks = 0;
  _stats->_stat_tot_created_edge_second_buffer_tasks = 0;
  _stats->_stat_tot_created_data_parallel_tasks = 0;
  _stats->_stat_tot_created_double_buffer_data_parallel_tasks = 0;

  /*cout << "Tasks: ";
  for(int i=0; i<core_cnt; ++i) {
    cout << ":Core: " << i << ":tasks: " << _stats->_stat_dist_tasks_dequed[i];
    _stats->_stat_dist_tasks_dequed[i]=0;
  }
  cout << endl;

  cout << "Edges:";
  for(int i=0; i<core_cnt; ++i) {
    cout << ":Core: " << i << ":edges: " << _stats->_stat_dist_edges_dequed[i];
    _stats->_stat_dist_edges_dequed[i]=0;
  }
  cout << endl;
  */
  /*for(int c=0; c<core_cnt; ++c) {
    bool x = _asic_cores[c]->pipeline_inactive(true);
    bool y = _mult_cores[c]->pipeline_inactive(true);
    bool z = _network->buffers_not_empty(true);
  }*/
}

void asic::check_async_slice_switch()
{
  bool go_in = false;
#if SGU_SLICING == 1
  if (!_switched_cache)
  {
    go_in = true;
  }
#endif
  if (_switched_async && !_switched_cache)
    go_in = true;
  if (go_in)
  {
    // Oh, then next cycle it will again not meet its goals
    // if(_stats->_stat_tot_finished_edges > _slicing_threshold && !_finishing_phase) {
    // TODO: I think we should try copy updates per vertex...
    // if(_stats->_stat_tot_num_copy_updates > _graph_vertices*0.5/_slice_count && !_finishing_phase) {
    // if(_stats->_stat_tot_num_copy_updates > _graph_vertices*0.8/_slice_count && !_finishing_phase) {
#if GCN == 0
    // if(_stats->_stat_tot_num_copy_updates > SLICE_ITER*_num_boundary_nodes_per_slice[_current_slice] && !_sgu_finishing_phase) {
    float cons = SLICE_ITER * _scratch_ctrl->_num_boundary_nodes_per_slice[_current_slice];
    if (cons == 0)
    { // if slice_iter should be 0.25
      cons = _scratch_ctrl->_num_boundary_nodes_per_slice[_current_slice] * 0.25;
    }
    if (cons == 0)
    {
      assert(_scratch_ctrl->_num_boundary_nodes_per_slice[_current_slice] == 0 && "case when no outgoing edges");
      cons = INF;
    }
    // but still reset? there should have been entries in the task queue to
    // execute? -- tasks copied: meaning there were entries in the task
    // queue... so then why is term flag=true?
    // oh it crossed the maximum number of edges...
#if SLICE_HRST == 0
    if (_stats->_stat_tot_num_copy_updates > cons && !_async_finishing_phase)
    {
#elif SLICE_HRST == 1
    cons = SLICE_ITER * _scratch_ctrl->_slice_size * (E / _graph_vertices);
    if (cons == 0)
    {
      cons = 0.25 * _scratch_ctrl->_slice_size * (E / _graph_vertices);
    }
    if (_stats->_stat_tot_finished_edges > cons && !_async_finishing_phase)
    {
#else
    if (0)
    {
#endif
      // if(_stats->_stat_tot_finished_edges > _slicing_threshold && !_async_finishing_phase) {
      // if(0) {
#else
    if (0)
    {
#endif
      cout << "Passed the slicing threshold with finished edges: " << _stats->_stat_tot_finished_edges << " and copy vertex updates: " << _stats->_stat_tot_num_copy_updates << " and num boundary vertices per slice: " << _scratch_ctrl->_num_boundary_nodes_per_slice[_current_slice] << "\n";
      _async_finishing_phase = true;
      print_local_iteration_stats(_current_slice, 0);
      _task_ctrl->reset_task_queues();
      _stats->_stat_global_num_copy_updates += _stats->_stat_tot_num_copy_updates;
      _stats->_stat_tot_num_copy_updates = 0;
      _stats->_stat_online_tasks = 0;
    }
  }
}

bool asic::should_spawn_sync_task(int vid, int reuse_factor)
{
#if GCN == 1
  // oh this is total local iterations
  // return ((_global_iteration-1)*reuse_factor+reuse<GCN_LAYERS-1);
  // FIXME: check if it is done correctly!!
  // FIXME: also keep track of which task to be executed now based on
  // the flags (also put assert for completely non-dedicated)
  return (_local_iteration < GCN_LAYERS - 1);
#elif CF == 1
#if TC == 1
  return false;
#else
  return (should_cf_spawn_task(vid));
#endif
#elif PR == 1
  return should_spawn_task(_vertex_data[vid], _scratch[vid]);
#else
  return (_vertex_data[vid] != _scratch[vid]);
#endif
}

// TODO: seems like only difference is (slice_id) and (push_in_worklist) --
// merge into one
int asic::task_recreation_during_sync(int slice_id, bool push_in_worklist, int reuse_factor)
{
  if (_config->is_async())
    return 0;
  _stats->_stat_barrier_count++;
  cout << "Came here with stat barrier count: " << _stats->_stat_barrier_count << endl;
  int tasks_produced = 0;
  for (int vid = slice_id * _scratch_ctrl->_slice_size; vid < (slice_id + 1) * _scratch_ctrl->_slice_size && vid < _graph_vertices; ++vid)
  {
    // bool should_spawn = should_spawn_sync_task(vid, reuse_factor);
    bool spawn = true;
    bool mult_spawn = true;
    switch (_config->_algo)
    {
    case gcn:
      spawn = _stats->_stat_barrier_count < GCN_LAYERS;
      mult_spawn = _stats->_stat_barrier_count <= GCN_LAYERS;
      break;
    case cf:
      spawn = should_cf_spawn_task(vid);
      break;
    case tc:
      spawn = false;
      break;
    case pr:
      spawn = should_spawn_task(_vertex_data[vid], _scratch[vid]);
      break;
    default:
      spawn = _scratch[vid] < _vertex_data[vid];
      break;
    };

    int core = _scratch_ctrl->get_local_scratch_id(vid);
    if (spawn)
    {
      if (!_config->is_vector())
      {
        _grad[slice_id] += abs(_vertex_data[vid] - _scratch[vid]);
        _prev_gradient += abs(_correct_vertex_data[vid] - _scratch[vid]);
        _vertex_data[vid] = _scratch[vid];
      }
      /*DTYPE diff = abs(_vertex_data[vid]-_scratch[vid]);
      if(_vertex_data[vid]==MAX_TIMESTAMP) {
        // diff = 1; // _asic->_scratch[cur_tuple.dst_id];
        diff = -_scratch[vid];
      }*/
      // _prev_gradient += diff;

      tasks_produced++;
      task_entry new_task(vid, _offset[vid]);
      _stats->_tot_activated_vertices++;
      if (push_in_worklist)
      {
        _task_ctrl->push_task_into_worklist(slice_id, 0, new_task); // aggregation task
      }
      else
      {
        // _task_ctrl->insert_new_task(0, 0, core, 0, new_task);
        _task_ctrl->insert_new_task(_asic_cores[core]->_agg_push_index, 0, core, priority, new_task);
        _asic_cores[core]->_agg_push_index += 1;
        _asic_cores[core]->_agg_push_index %= NUM_TQ_PER_CORE;
      }
    }
    if (mult_spawn)
    {
      // For GCN, I want to see which task to produce.
      if (_config->_algo == gcn && _config->_agg_mult_type == global)
      {
        // produce mult tasks for the previous layer
        ++tasks_produced;
        _task_ctrl->insert_local_coarse_grained_task(core, vid, 0, false); // matrix task
      }
    }
  }
  cout << "Tasks produced in phase 2 is: " << tasks_produced << endl;
  _start_active_vertices = tasks_produced;
  return tasks_produced;
}

int asic::task_recreation_during_sync()
{ // for non-sliced execution
  _stats->_stat_barrier_count++;
#if LADIES_GCN == 1 // oh this is for sampling or creating new graph
  // finished comp per cycle
  float hw_util = _stats->_stat_tot_finished_edges * FEAT_LEN / (float)_cur_cycle;
  hw_util = hw_util / (float)(core_cnt * VEC_LEN);

  cout << "edges: " << _stats->_stat_tot_finished_edges << endl;
  // cout << "Hardware util: " << _cur_cycle << endl;

  cout << "Completed " << _stats->_stat_barrier_count << " GCN layers at cycle: " << _cur_cycle << " with h/w util: " << hw_util << "\n";
  // print_stats();
  // exit(0);

  for (int i = 0; i < LADIES_SAMPLE; ++i)
    _correct_vertex_data[i] = 0;

  _offset[0] = 0;
  for (int i = 0; i < LADIES_SAMPLE; ++i)
  {
    _offset[i + 1] = _sampled_offset[_stats->_stat_barrier_count][i];
    for (int j = _sampled_offset[_stats->_stat_barrier_count][i]; j < _sampled_offset[_stats->_stat_barrier_count][i + 1]; ++j)
    {
      _neighbor[j] = _sampled_neighbor[_stats->_stat_barrier_count][j];
      _neighbor[j].src_id = i;
      _neighbor[j].wgt = 1;
      _correct_vertex_data[_neighbor[j].dst_id] += 1;
    }
  }
  _offset[LADIES_SAMPLE] = _sampled_offset[_stats->_stat_barrier_count][LADIES_SAMPLE - 1];
#endif

  // FIXME: somehow not creation new tasks

  // Now insert new tasks
  int task_count = 0;
  int tot_vertices = _config->_algo == ladies_gcn ? LADIES_SAMPLE : _graph_vertices;
  _scratch_ctrl->_is_visited.reset(); // to be used for direction-optimizing BFS
  cout << "Came here for activating new vertices\n";
  for (int i = 0; i < tot_vertices; ++i)
  {
    if (_config->is_non_frontier() && _config->_algo != cf && _offset[i] == _offset[i + 1])
      continue;

    bool spawn = true;
    // bool mult_spawn=true;
    switch (_config->_algo)
    {
    case gcn:
      spawn = _stats->_stat_barrier_count <= GCN_LAYERS;
      // mult_spawn = _stats->_stat_barrier_count<=GCN_LAYERS;
      break;
    case cf:
      spawn = should_cf_spawn_task(i);
      break;
    case pr:
      spawn = should_spawn_task(_vertex_data[i], _scratch[i]);
      break;
    default:
      spawn = _scratch[i] < _vertex_data[i];
      break;
    };

    if (!_config->is_vector())
    {
      /*DTYPE diff = abs(_vertex_data[i]-_scratch[i]);
      if(_vertex_data[i]==MAX_TIMESTAMP && spawn) {
        // diff = 1;
        diff = -_scratch[i];
      }*/
      // _prev_gradient += diff;
      if (spawn)
      {
        _prev_gradient += abs(_correct_vertex_data[i] - _scratch[i]);
      }
      _vertex_data[i] = _scratch[i];
    }

    if (spawn)
    {
      task_entry new_task(i, _offset[i]);
      int core = _scratch_ctrl->get_local_scratch_id(new_task.vid);
      if (_config->_algo == gcn)
      {
        if (_config->_agg_mult_type == global)
        {
          // produce mult tasks for the previous layer
          _task_ctrl->insert_local_coarse_grained_task(core, new_task.vid, 0, false);
          task_count++;
        }
        if (_stats->_stat_agg_barrier >= GCN_LAYERS - 1)
        {
          continue;
        }
      }
      _scratch_ctrl->_is_visited.set(i);
      DTYPE priority = 0;
      if (!_config->_cf)
      {
        priority = apply(_vertex_data[i], i);
      }
      _task_ctrl->insert_new_task(_asic_cores[core]->_agg_push_index, 0, core, priority, new_task);
      _asic_cores[core]->_agg_push_index += 1;
      _asic_cores[core]->_agg_push_index %= NUM_TQ_PER_CORE;
      task_count++;
    }
  }
  if (_config->_algo == gcn && _stats->_stat_agg_barrier < GCN_LAYERS - 1)
  {
    ++_stats->_stat_agg_barrier;
    cout << "Next layer of: " << _stats->_stat_barrier_count << " aggregation tasks via barrier at cycle: " << _cur_cycle << "\n";
  }
  print_cf_mean_error();
  // cout << "Barrier at cycle: " << _cur_cycle << " and cumulative gradient: " << _prev_gradient/V << endl;
  _task_ctrl->_present_in_queue.reset();
  // calc_and_print_training_error();
  cout << "Barrier at cycle: " << _cur_cycle << " and cumulative gradient: " << _prev_gradient / _unique_vertices << endl;
  _prev_gradient = 0;
  cout << "Tasks produced in phase 2 of sync: " << task_count << endl;
  _start_active_vertices = task_count;
  return task_count;
}

void asic::schedule_tasks(float &edges_served, int &max_edges_served, float &lbfactor)
{
  // check if all queues are still empty (network controller should check it
  // right?)

  int count = 0;
  if (_config->no_swarm())
  {
    _elem_dispatch = 0;
    count = 0;
    int max_elem_dispatch = mem_bw / 4;
    if (_config->_dramsim_enabled)
    {
      max_elem_dispatch = 512 / 4;
      if (_config->_prac == 0)
      {
        max_elem_dispatch = 51200;
      }
    }
    assert(max_elem_dispatch > 0);
    /*cout << "cur cycle: " << _cur_cycle << " started at core: " << _which_disp_ptr;
      if(!_asic_cores[1]->_task_queue[0][0].empty()) {
        cout << " task at core 1"; // oh task did not go to core 1 
      }
      cout << endl;*/
    // TODO: total should be less than 128 and each core should be less than 16
    while (_elem_dispatch < max_elem_dispatch && count < core_cnt)
    {
      _max_elem_dispatch_per_core = _elem_dispatch + max(process_thr, 16);

      // serves for infinite throughput
      int max_serves = NUM_TQ_PER_CORE;
      int start_ptr = 0;
      int served_left = max_serves;

      // bool limited_tq=false; // infinite throughput
      // if priority ordering is true -- then limit the max_serves
      if (_config->_prac && (!_config->_fifo || _switched_async))
      {
        max_serves = NUM_TQ_PER_CORE / 2;
        if (max_serves == 0)
          max_serves = _cur_cycle % 2; // this is for 0.5
        served_left = max_serves;
        start_ptr = _asic_cores[_which_disp_ptr]->_agg_pop_index;
        _asic_cores[_which_disp_ptr]->_agg_pop_index += 1;
        _asic_cores[_which_disp_ptr]->_agg_pop_index %= NUM_TQ_PER_CORE;
      }

      int tqid = start_ptr;
      int other_tqid = (tqid + 1) % NUM_TQ_PER_CORE;
      // logic to ensure that the tasks are strictly ordered and are not
      // interleaved
      // TODO: currently only attempt for pull implementation
      if (_config->_pull_mode && !_switched_sync && other_tqid != tqid && (!_asic_cores[_which_disp_ptr]->_task_queue[other_tqid][0].empty() || _asic_cores[_which_disp_ptr]->_pending_task[other_tqid].vid != -1))
      {
        // auto ita = _asic_cores[_which_disp_ptr]->_task_queue[other_tqid][0].begin();
        // task_entry cur_task = (ita->second).front();
        // cout << "vid: " << cur_task.vid << " offset: " << _offset[cur_task.vid] << " and start offset: " << cur_task.start_offset << " other tqid: " << other_tqid <<  endl;
        // if(cur_task.start_offset!=_offset[cur_task.vid]) {
        if (_asic_cores[_which_disp_ptr]->_pending_task[other_tqid].vid != -1)
        {
          // assert that the current tqid is alright
          if (!_asic_cores[_which_disp_ptr]->_task_queue[tqid][0].empty())
          {
            auto itb = _asic_cores[_which_disp_ptr]->_task_queue[tqid][0].begin();
            task_entry old_task = (itb->second).front();
            assert(old_task.start_offset == _offset[old_task.vid] && "only one task should be active at a time");
          }
          tqid = other_tqid;
          _asic_cores[_which_disp_ptr]->_agg_pop_index += 1;
          _asic_cores[_which_disp_ptr]->_agg_pop_index %= NUM_TQ_PER_CORE;
        }
      }
      // fill it with indices of smallest priorities
      // if prac==0, we would like to call it multiple times
      int call_time = 1;
      /*if(_config->_prac==0) {
          call_time = process_thr/16;
        }*/
      if (_config->_heter_cores == 0)
      {
        call_time = _fine_alloc[_which_disp_ptr];
      }
      // for(int call=0; call<call_time && _elem_dispatch<_max_elem_dispatch_per_core; ++call) {
      for (int call = 0; call < call_time && _elem_dispatch < max_elem_dispatch; ++call)
      {
        served_left = max_serves;
        for (int sample = 0; sample < max_serves && served_left != 0; ++sample)
        { // TODO: that periodic access should be applied here

          // cout << "checking for tqid: " << tqid << endl;
          if (!_asic_cores[_which_disp_ptr]->_task_queue[tqid][0].empty() || _asic_cores[_which_disp_ptr]->_pending_task[tqid].vid != -1)
          {
            // auto ita = _asic_cores[_which_disp_ptr]->_task_queue[tqid][0].begin();

            // cout << "Cycle: " << _cur_cycle << " call: " << call << " max_call: " << call_time << " elem dispatch: " << _elem_dispatch
            // << " max: " << _max_elem_dispatch_per_core << " cb entries: " << _compl_buf[_which_disp_ptr]->_entries_remaining << endl;

            // if(_elem_dispatch<_max_elem_dispatch_per_core) { //  && ita->first < _avg_timestamp) // allow to go only if we have timestamp larger than average
            if (_elem_dispatch < max_elem_dispatch)
            {
              served_left--;
              bool can_serve = true;

              // TODO: checking whether completion buffer has empty slots
              if (_config->_dramsim_enabled)
              {
                int cb_deadlock_barrier = 1;
                if (_config->is_sync())
                {
                  cb_deadlock_barrier *= 2;
                }
                // FIXME: checking: remove
                // cb_deadlock_barrier += 3072;
                /*if(_config->is_vector()) { 
                  cb_deadlock_barrier=FEAT_LEN*message_size/line_size;
                  cb_deadlock_barrier += 2; // FIXME: adapt it to serve less edges at a time as well?
                }*/
                can_serve = _compl_buf[_which_disp_ptr]->_entries_remaining > cb_deadlock_barrier;
              }

              if (can_serve)
              {
                /*if(_which_disp_ptr==0) {
                  cout << "Element dispatch before: " << _elem_dispatch;
                }*/
                // cout << "Calling dispatch for core: " << _which_disp_ptr << " cycle: " << _cur_cycle << endl;
                _asic_cores[_which_disp_ptr]->dispatch(tqid, 0);
                /*if(_which_disp_ptr==0) {
                  cout << " Element dispatched: " << _elem_dispatch << " cycle: " << _cur_cycle << endl;
                }*/
                // cout << "Core: " << c << " full-tq1: " << (TASK_QUEUE_SIZE-_task_ctrl->_remaining_task_entries[0][0]) << " full-tq2 "
                // << (TASK_QUEUE_SIZE-_task_ctrl->_remaining_task_entries[1][0]) << " fifo: " << _asic_cores[c]->_fifo_task_queue.size()
                // << " cb-entry: " << (COMPLETION_BUFFER_SIZE-_compl_buf[c]->_entries_remaining) << endl;
              }
              else
              {
                _asic_cores[_which_disp_ptr]->_cb_full++;
              }
            }
            else
            {
              _asic_cores[_which_disp_ptr]->_no_slot_for_disp++;
            }
          }
          else
          {
            _asic_cores[_which_disp_ptr]->_tq_empty++;
            /*if(_config->_heter_cores==0) {
              _asic_cores[_which_disp_ptr]->_fine_grain_throughput=0;
              _mult_cores[_which_disp_ptr]->_data_parallel_throughput=2;
              // _mult_cores[_which_disp_ptr]->cycle();
              // cout << "At cycle: " << _cur_cycle << " core: " << _which_disp_ptr << " was run again\n";
            }*/
            if (_config->_dyn_load_bal == 1)
            {
              if (_task_ctrl->_remaining_task_entries[_which_disp_ptr][tqid] >= HIGH_PRIO_RESERVE && _task_ctrl->can_pull_from_overflow(_which_disp_ptr))
              {
                _task_ctrl->pull_tasks_from_fifo_to_tq(_which_disp_ptr, tqid);
              }
            }
          }
          tqid = (tqid + 1) % NUM_TQ_PER_CORE;
          _asic_cores[_which_disp_ptr]->_agg_pop_index = tqid;
        }
      }

      // sum the edges served (Since tasks may take some latency, they may not have entry every time)
      edges_served += _stats->_stat_fine_gran_lb_factor[_which_disp_ptr];
      if (_stats->_stat_fine_gran_lb_factor[_which_disp_ptr] > max_edges_served)
      {
        max_edges_served = _stats->_stat_fine_gran_lb_factor[_which_disp_ptr];
      }

      count++;
      _which_disp_ptr++;
      _which_disp_ptr = (_which_disp_ptr < core_cnt) ? _which_disp_ptr : 0;
    }
    if (edges_served != 0)
    {
      assert(max_edges_served != 0);
      lbfactor += max_edges_served / (float)(edges_served / (float)core_cnt);
    }
    else
    {
      lbfactor += 1;
    }
  }
  else
  {
    int x = core_cnt * LANE_WIDTH * 4;

    while (_elem_dispatch < x && count < core_cnt)
    {                                  // to prevent infinite loop
      _max_elem_dispatch_per_core = x; // _max_elem_mem_load;
      for (int i = 0; i < LANE_WIDTH; ++i)
      {
        if (_asic_cores[_which_disp_ptr]->_in_order_flag[i] && (!_asic_cores[_which_disp_ptr]->_task_queue[i][0].empty()) && !_asic_cores[_which_disp_ptr]->_swarm_pending_request[i])
        {
          _asic_cores[_which_disp_ptr]->dispatch(0, i);
        }
      }

      count++;
      _which_disp_ptr++;
      _which_disp_ptr = (_which_disp_ptr < core_cnt) ? _which_disp_ptr : 0;
    }
  }
  _elem_dispatch = 0;
}

void asic::simulate()
{
  if (_config->_domain != tree)
  {
    initialize_simulation();
  }

  bool term_flag = true;
  bool fine_term_flag = true;
  // todo: check for termination condition here

  float lbfactor = 0;
  float lbfactor_atomic = 0;
  int pushed_tree_tasks = 0;
  while (_cur_cycle < _max_iter)
  {

    // cout << "Size of the outstanding memory request queue: " << _outstanding_mem_req.size() << endl;

    if (_config->_heter_cores == 1)
    {
      for (int c = 0; c < core_cnt; ++c)
      {
        assert(_fine_alloc[c] == 1 && "fine alloc should always be 1");
        assert(_coarse_alloc[c] == 1 && "coarse alloc should always be 1");
      }
    }

    // 28 issued in a cycle?? (332 in between; latency of 19 cycles??)

    // FIXME: what if created at a different core -- we cannot reason about locality? (At least a couple of cores)
    // FIXME: Let me just prioritized higher depth tasks
    int recurrence_delay = 1;
    // this is unknown due to memory accesses to the tree that is on-chip (TODO: oh i should switch edge access to scratch)
    if (_config->_domain == tree && pushed_tree_tasks < NUM_KNN_QUERIES && _cur_cycle % recurrence_delay == 0)
    {
      // should not be pushed every cycle -- 4*process_thr every 4 cycles
      for (int i = 0; i < (recurrence_delay * process_thr * 2) && pushed_tree_tasks < NUM_KNN_QUERIES; ++i)
      {
        task_entry start_task(_src_vid, _offset[_src_vid]);
        start_task.dist = 0;
        start_task.tid = pushed_tree_tasks;
        _task_ctrl->insert_new_task(0, 0, 0, 0, start_task);
        ++pushed_tree_tasks;
      }
      /*for(int i=0; i<(2*process_thr) && pushed_tree_tasks<NUM_KNN_QUERIES; ++i) {
        task_entry start_task(_src_vid, _offset[_src_vid]);
        start_task.tid=pushed_tree_tasks;
        _task_ctrl->insert_new_task(0,0,0,0,start_task);
        ++pushed_tree_tasks;
      }*/
    }

    // Queue occupancy
    // if(_cur_cycle>10000) return;
    /*for(int c=0; c<core_cnt; ++c) {
      cout << "Core: " << c << " full-tq1: " << (TASK_QUEUE_SIZE-_task_ctrl->_remaining_task_entries[c][0]) << " full-tq2 "
      << (TASK_QUEUE_SIZE-_task_ctrl->_remaining_task_entries[c][1]) << " fifo: " << _asic_cores[c]->_fifo_task_queue.size()
      << " cb-entry: " << (COMPLETION_BUFFER_SIZE-_compl_buf[c]->_entries_remaining) << endl;
    }*/
    float edges_served = 0;
    int max_edges_served = 0;
    float edges_served_atomic = 0;
    int max_edges_served_atomic = 0;

    update_stats_per_cycle();

    // global task scheduling???
    if (_config->_sgu == 0 && _config->_fifo == 0)
    {
      _task_ctrl->central_task_schedule();
    }

    if (_cur_cycle % STATS_INTERVAL == 0)
    {
      print_simulation_status();
    }
    term_flag = true;
    fine_term_flag = true;

    // Step 1: task distribution?
#if WORK_STEALING == 1
    // task_queue_stealing();
#else
#if PRAC == 0
    _task_ctrl->distribute_prio();
#else
    // push in task queue only 1 element, or multiple?
    // _task_ctrl->distribute_one_task_per_core();
    _task_ctrl->distribute_lb();
#endif
#endif

#if DIJKSTRA == 1 || SWARM == 1 || ESPRESSO == 1
    // _asic_cores[0]->commit();
    // FIXME: what to do here?
    for (int i = 0; i < core_cnt * LANE_WIDTH; ++i)
    {
      _asic_cores[0]->commit();
    }
#endif
    check_async_slice_switch();

    // serve requests associated with each scratchpad bank
    for (int b = 0; b < core_cnt; ++b)
    {
      for (int a = 0; a < _fine_alloc[b]; ++a)
      {
        if ((_config->is_async() || _switched_async || !_config->no_swarm()) && !_switched_sync)
        {
          _asic_cores[b]->spec_serve_atomic_requests();
        }
        else
        {
          _asic_cores[b]->serve_atomic_requests();
        }
        if (_config->_net == crossbar)
          break;
      }
    }

    cycle_pending_comp();
    _transactions_this_cycle = 0;

    // real simulation...
    /*if(_config->_numa_mem) {
        for(int c=0; c<core_cnt; ++c) {
          _mem_ctrl->_numa_mem[c]->update();
        }
      } else {
      }*/
    _mem_ctrl->_mem->update();

    // It should only see the output after cores have updated...
    if (_config->_algo == gcn || _config->_domain == tree)
    {
      // for(int i=_mult_cores[0]->_last_dispatch_core, j=0; j<core_cnt; ++j, i=(i+1)%core_cnt) {
      for (int i = _last_dispatch_core, j = 0; j < core_cnt; ++j, i = (i + 1) % core_cnt)
      {
        assert(_coarse_alloc[i] > 0 && "currently we do not allow less than 1 dfg to multiplication");
        for (int a = 0; a < _coarse_alloc[i]; ++a)
        {
          if (_mult_cores[i]->cycle(a))
          { // continue if any core was working
            term_flag = false;
          }
          // _mult_cores[i]->_data_parallel_throughput=1;
        }
      }
    }
    // TODO: do for schedule tasks and serve_atomic_updates as well!!
    for (int i = 0; i < core_cnt; ++i)
    {
      for (int a = 0; a < _fine_alloc[i]; ++a)
      {
        if (_asic_cores[i]->cycle())
        { // continue if any core was working
          // cout << "At cycle: " << _cur_cycle << " core: " << i
          // <<  " prefetch_process: " << _asic_cores[i]->_prefetch_process[0].size()
          // << " and task_queue_size: " << _asic_cores[i]->_task_queue[0][0].size() << endl;
          if (_asic_cores[i]->_fine_grain_throughput == 0)
          {
            assert(_asic_cores[i]->_prefetch_process[0].size() == 0 && "pending updates from now should go to other cores");
            assert(_asic_cores[i]->_task_queue[0][0].size() == 0 && "pending updates from now should go to other cores");
          }
          term_flag = false;
          fine_term_flag = false;
        }
        // _asic_cores[i]->_fine_grain_throughput=1*_config->_process_thr;
        /*edges_served_atomic += _stats->_stat_fine_gran_lb_factor_atomic[i];
          if (_stats->_stat_fine_gran_lb_factor_atomic[i] > max_edges_served_atomic) {
              max_edges_served_atomic = _stats->_stat_fine_gran_lb_factor_atomic[i];
          }*/
      }
    }
    for (int i = 0; i < core_cnt; ++i)
    {
      if (!_asic_cores[i]->pipeline_inactive(false))
      { // continue if any core was working
        term_flag = false;
      }
      if (!_mult_cores[i]->pipeline_inactive(false))
      { // continue if any core was working
        term_flag = false;
      }
    }
    /*cout << "Cycle: " << _cur_cycle << endl;
    for(int c=0; c<core_cnt; ++c) {
      bool x = _asic_cores[c]->pipeline_inactive(true);
      bool y = _mult_cores[c]->pipeline_inactive(true);
    }*/
    /*cout << "after core\n";
    for(int c=0; c<core_cnt; ++c) {
      _asic_cores[c]->pipeline_inactive(true);
    }*/
    if (_network->cycle())
    {
      term_flag = false;
      fine_term_flag = false;
    }
    /*cout << "after network\n";
    for(int c=0; c<core_cnt; ++c) {
      _asic_cores[c]->pipeline_inactive(true);
    }*/

    // stats collection?
    if (edges_served_atomic != 0)
    {
      assert(max_edges_served_atomic != 0);
      lbfactor_atomic += max_edges_served_atomic / (float)(edges_served_atomic / (float)core_cnt);
    }
    else
    {
      lbfactor_atomic += 1;
    }

    _mem_ctrl->empty_delay_buffer();
    schedule_tasks(edges_served, max_edges_served, lbfactor);
    /*for(int a=0; a<_fine_alloc; ++a) {
    }*/
    // cout << "Memory requests sent at this cycle: " << _cur_cycle << " are: " << _mem_ctrl->_mem_reqs_this_cycle << endl;
    // cout << "Scratch requests sent at this cycle: " << _cur_cycle << " are: " << _scratch_ctrl->_scratch_reqs_this_cycle << endl;
    _mem_ctrl->_mem_reqs_this_cycle = 0;
    _scratch_ctrl->_scratch_reqs_this_cycle = 0;
    _cur_cycle++;

    // Check delay_buffer only if term_flag is true
    /*if(term_flag) {
      for(int i=0; i<=CACHE_LATENCY; ++i) {
        if(!_mem_ctrl->_l2cache_delay_buffer[i].empty()) {
        ++_mem_requests_this_slice;
          term_flag=true;
          break;
         }
       }
     }*/

    bool is_switch = term_flag;
    if (_config->_algo == gcn)
    {
      is_switch &= (_stats->_stat_barrier_count < (GCN_LAYERS - 1));
    }
    // for the end of the pipeline, where multiplication will be unique.
    if (term_flag)
    { // why at cycle 1?
      cout << "Barrier count: " << _stats->_stat_barrier_count << " at cycle: " << _cur_cycle << endl;
    }
    // A new barrier would be when both tasks are completed. (should be alternate with the original count)
    /*if(_config->_mult_agg_type==global && !term_flag && fine_term_flag && _stats->_stat_agg_barrier<_stats->_stat_barrier_count+1 && _stats->_stat_agg_barrier<GCN_LAYERS-1 &&  _config->_agg_mult_type!=global) {
        // here create aggregation tasks for the next layer and increase the barrier.
        for(int vid=0; vid<_graph_vertices; ++vid) {
            task_entry next_task(vid, _offset[vid]);
            next_task.second_buffer=true;
            // TODO: send a second buffer task
            _task_ctrl->insert_local_task(0, 0, _scratch_ctrl->get_local_scratch_id(vid), vid, next_task);
        }
        // FIXME: this is wrong, need to keep another copy
        for(int i=0; i<_graph_vertices; ++i) {
            _correct_vertex_data_double_buffer[i] = (_scratch_ctrl->_in_degree[i])*FEAT_LEN*message_size/bus_width;
        }
        ++_stats->_stat_agg_barrier;
        cout << "Next layer of: " << _stats->_stat_barrier_count << " aggregation tasks are already pushed at cycle: " << _cur_cycle << "\n";
    }*/
    // should be put in the simulation side
    if (_config->_graphmat_slicing == 0 && _config->_algo == gcn && term_flag && (_stats->_stat_barrier_count == (GCN_LAYERS - 1)) && _config->_agg_mult_type == global)
    {
      assert(!is_switch && "last layer do not need to produce multiplication");
      int tasks_pushed = 0;
      for (int vid = 0; vid < _graph_vertices; ++vid)
      {
        tasks_pushed++;
        // produce mult tasks for the previous layer
        // if full, it should push in the overflow task queue but this can be full anyways
        _task_ctrl->insert_local_coarse_grained_task(_scratch_ctrl->get_local_scratch_id(vid), vid, 0, false);
      }
      term_flag = false;
      _stats->_stat_barrier_count++;
      for (int c = 0; c < core_cnt; ++c)
      {
        _fine_alloc[c] = 1;
        _coarse_alloc[c] = 1;
        _mult_cores[c]->_dfg_latency = DFG_LENGTH;
      }
      cout << "Multiplication tasks pushed for the end of the pipeline with tasks pushed: " << tasks_pushed << "\n";
    }
    // FIXME: there should not be a barrier when both are async (or barrier count should have been 1)
    if (is_switch && _config->_graphmat_slicing == 0 && (_config->is_sync() || _switched_sync) && (_config->_slice_count == 1 || _switched_cache) && !(_config->_agg_mult_type != global && _config->_mult_agg_type != global))
    {
      int task_count = task_recreation_during_sync();
      term_flag = (task_count == 0);
      for (int c = 0; c < core_cnt; ++c)
      {
        _fine_alloc[c] = 1;
        _coarse_alloc[c] = 1;
        _mult_cores[c]->_dfg_latency = DFG_LENGTH;
      }
      // fix the correct vertex data
      if (!term_flag && _config->_algo == gcn && _config->_agg_mult_type != global)
      {
        for (int i = 0; i < _graph_vertices; ++i)
        {
          _correct_vertex_data[i] = (_scratch_ctrl->_in_degree[i]) * FEAT_LEN * message_size / bus_width;
        }
      }
    }

#if GRAPHLAB == 1
    if (term_flag)
    {
      for (int i = 0; i < _graph_vertices; ++i)
      {
        _vertex_data[i] = _scratch[i];
      }
    }
#endif

    // break if all the cores are free
    if (term_flag)
    {
      break;
    }
  }

  if (_cur_cycle == _max_iter)
  {
    assert(0 && "Infinite simulation");
  }

  // if it reaches here without passing through slicing threshold, resetting
  // has to be done here
  bool go_in = false;
#if SGU_SLICING == 1
  if (!_switched_cache)
    go_in = true;
#endif
  if (_switched_async && !_switched_cache)
    go_in = true;
  if (go_in)
  {
    cout << "Fine-grained LB factor: " << lbfactor / (float)_cur_cycle << endl;
    cout << "Fine-grained LB factor atomic: " << lbfactor_atomic / (float)_cur_cycle << endl;
    print_local_iteration_stats(_current_slice, 0);
    _task_ctrl->reset_task_queues();
    return;
  }
  if ((_config->_exec_model == sync_slicing || _config->_exec_model == blocked_async) && !_switched_async)
  {
    return;
  }

  cout << "Fine-grained LB factor: " << lbfactor / (float)_cur_cycle << endl;
  cout << "Fine-grained LB factor atomic: " << lbfactor_atomic / (float)_cur_cycle << endl;
  print_stats();
  finish_simulation();
}

void asic::finish_simulation()
{
  if (_cur_cycle == _max_iter)
  {
    cout << "infinite loop\n";
    for (int i = 0; i < core_cnt; ++i)
    {
      _asic_cores[i]->pipeline_inactive(true);
    }
    cout << "Are network buffers not empty? " << _network->buffers_not_empty(false) << endl;
  }
  else
  {
    for (int c = 0; c < core_cnt; ++c)
    {
      bool x = _asic_cores[c]->pipeline_inactive(true);
      assert(x);
    }
    assert(!_network->buffers_not_empty(false));
    if (correctness_check())
    {
      cout << "Correct answer!"
           << "\n";
    }
    else
    {
      cout << "Incorrect answer!"
           << "\n";
    }
  }
}

void asic::perform_algorithm_switching(int slice_id, int mem_eff)
{

// TODO: we should be able to switch in a decoupled way...<async, scratch>
#if (GRAPHMAT_SLICING == 1 || SGU_SLICING == 1) && HYBRID == 1 // && SLICE_COUNT>1
  // if(_cur_cycle>100000 && !_switched_async && mem_eff<128 && slice_id!=-1) {
  // TODO: if no such global iteration count, how does it know when to switch to cache?
  // SOMEHOW IT IS NOT COMING HERE FOR SLICE_COUNT=1 (oh maybe earlier)

  // TODO: Just evaluating switch to cache...
  // FIXME: it is switching back and forth? (it should give a penalty to switch)
#if HEURISTIC == 1 // active vert
  // for those weird phases when it says active vertices was 1?
  cout << "Checking for hybrid heuristic at cycle: " << _cur_cycle << " and cycles per iteration: " << _cycles_per_iteration << " initial active vertices: " << _start_active_vertices << endl;
  // if(_cycles_per_iteration>=1000) {
  if (_cycles_per_iteration >= 10)
  {
    //  }_active_vert_last_iteration!=0 && _start_active_vertices!=0) {
    // float change1 = _start_active_vertices/(float)_active_vert_last_iteration;
    // float change2 = _active_vert_last_iteration/(float)_start_active_vertices;
    if (_start_active_vertices < DYN_THRES)
    { // if new active vertices are more than 10x, then we should switch back
      _cache_tipping_point = _global_iteration;
      /*if(_config->_graphmat) { // Let me skip that also?
         _async_tipping_point=_global_iteration;
       }*/
      cout << "Current active vertex: " << _start_active_vertices << endl;
      cout << "Previous active vertex: " << _active_vert_last_iteration << endl;
      cout << "Based on active vertices, identified executing in caching mode at cycle: " << _cur_cycle << "\n";
    }
    else
    {
      if (_switched_cache)
      {
        _switched_cache = false;
        _cache_tipping_point = -1;
        // FIXME: do not need that?
        if (_config->_graphmat)
        {
          _switched_async = false;
          _async_tipping_point = -1;
        }
        // no need if it alreayd graphmat
#if GRAPHMAT == 0
        _sync_tipping_point = _global_iteration;
#endif
        cout << "Current active vertex: " << _start_active_vertices << endl;
        cout << "Previous active vertex: " << _active_vert_last_iteration << endl;
        cout << "Identified Switching to scratch mode at cycle: " << _cur_cycle << "\n";
        // move current off-slice tasks to the worklists
        _task_ctrl->reset_task_queues();
        simulate_slice();
      }
      // now my simulate_*_slice should execute instead of simulate() right?
      // could have moved tasks to worklist
    }
    _active_vert_last_iteration = _start_active_vertices;
  }

#elif HEURISTIC == 2 // mem bw
  if (_cycles_per_iteration >= 1000)
  {
    if (1)
    { // }_mem_eff_last_iteration!=0 && mem_eff!=0) {
      // float change1 = mem_eff/(float)_mem_eff_last_iteration;
      // float change2 = _mem_eff_last_iteration/(float)mem_eff;
      // if(change1>1.25) {
      if (mem_eff < 128)
      {
        cout << "Current active vertex: " << mem_eff << endl;
        cout << "Previous active vertex: " << _mem_eff_last_iteration << endl;
        cout << "Identified Switching to caching mode at cycle: " << _cur_cycle << "\n";
        _cache_tipping_point = _global_iteration;
#if GRAPHMAT == 1
        _async_tipping_point = _global_iteration;
#endif
      }
      if (mem_eff > 128)
      { // if it improves beyond a certain point?
        _switched_cache = false;
        _cache_tipping_point = -1;
#if GRAPHMAT == 1
        _switched_async = false;
        _async_tipping_point = -1;
#endif
        cout << "Current active vertex: " << _start_active_vertices << endl;
        cout << "Previous active vertex: " << _active_vert_last_iteration << endl;
        cout << "Identified Switching to scratch mode at cycle: " << _cur_cycle << "\n";
#if GRAPHMAT == 1
        simulate_graphmat_slice();
#elif SGU == 1
        simulate_sgu_slice();
#endif
      }
    }
    _mem_eff_last_iteration = mem_eff;
  }

#elif HEURISTIC == 3 // comparison to pred thr
  // this should be at the later iterations
  if (_cycles_per_iteration >= 1000 && _cur_cycle > 100000 && mem_eff < 0.9 * PRED_THR)
  { // PRED_THR for sp.lj.non_sliced is 18*message_size

    cout << "Current mem eff: " << mem_eff << endl;
    cout << "Previous active vertex: " << PRED_THR << endl;
    cout << "Identified Switching to caching mode at cycle: " << _cur_cycle << "\n";
    _cache_tipping_point = _global_iteration;
#if GRAPHMAT == 1
    _async_tipping_point = _global_iteration;
#endif
  }
  if (mem_eff > 0.8 * PRED_THR)
  { // if it improves beyond a certain point?
    _switched_cache = false;
    _cache_tipping_point = -1;
#if GRAPHMAT == 1
    _switched_async = false;
    _async_tipping_point = -1;
#endif
    cout << "Current active vertex: " << _start_active_vertices << endl;
    cout << "Previous active vertex: " << _active_vert_last_iteration << endl;
    cout << "Identified Switching to scratch mode at cycle: " << _cur_cycle << "\n";
#if GRAPHMAT == 1
    simulate_graphmat_slice();
#elif SGU == 1
    simulate_sgu_slice();
#endif
  }
#endif

  // NOW LOGIC TO CHECK WHETHER THE TIPPING POINT HAS ARRIVED
#if CACHE_NO_SWITCH == 0
  if ((_global_iteration == _cache_tipping_point || slice_id == _cache_tipping_point) && slice_id != -1 && !_switched_cache)
  {
    /*if(slice_id!=0 && slice_id==_cache_tipping_point) {
      cout << "slice: " << slice_id << " cache tipping point: " << _cache_tipping_point << endl;
      assert(_switched_async && "should have already switched to async");
    }*/
    _switched_cache = true;
    cout << "SWITCHED TO CACHE MODE at cycle: " << _cur_cycle << "\n";
    // If async is not true, should we push all the data in worklists to the task queues?
    // Of course, this is switching to caches with non-tiled execution
    // Because after this everything is in task queue, now it should run normal simulate();
    // this should change sync->scratch -> sync->cache as well

    int tot_tasks_copied = 0;
    for (int slice = 0; slice < SLICE_COUNT; ++slice)
    {
      _task_ctrl->move_tasks_from_wklist_to_tq(slice);
    }
    cout << "Total tasks copied: " << tot_tasks_copied << endl;

    if (!(_global_iteration == _async_tipping_point && slice_id != -1 && !_switched_async))
    {
      _async_finishing_phase = false;
      simulate_slice(); // so that produces tasks again after returning from simulate.
      cout << "Came out of simulate at cycle: " << _cur_cycle << endl;
      for (int slice = 0; slice < SLICE_COUNT; ++slice)
      {
        assert(_task_ctrl->_worklists[slice].empty() && "all worklists should be empty after non-sliced execution");
      }
      for (int i = 0; i < _graph_vertices; ++i)
      {
        assert(_scratch[i] <= _vertex_data[i]);
        _vertex_data[i] = _scratch[i];
      }
    }
    else
    {
      simulate(); // trying a fix for async-cache
    }
  }
#endif
#if SYNC_NO_SWITCH == 0
  if (_global_iteration == _sync_tipping_point && slice_id != -1 && !_switched_sync)
  {
    // if(_asic->_cur_cycle==200000) {
    // if(_prev_gradient/V>1.05 && !_switched_sync && !_async_finishing_phase) {
    // if(_prev_gradient/V<-2.0 && !_switched_sync && !_async_finishing_phase) {
    // if(_prev_error<=50 && !_switched_sync && !_async_finishing_phase) {
    // push all pending tasks in the task queue
    // prac has to be somehow 1?
    if (_switched_cache)
    {
      for (int slice = 0; slice < SLICE_COUNT; ++slice)
      {
        _task_ctrl->move_tasks_from_wklist_to_tq(slice);
      }
    }
    // This vertex should have been already made active I guess??
    if (_switched_cache)
    {
      cout << "Calling cache simulate\n";
      simulate();
    }
    else
    {
      /*for(int i=0; i<_graph_vertices; ++i) {
      _vertex_data[i]=_scratch[i];
    }*/
      cout << "SWITCHED TO SYNC EXECUTION\n";
      // TODO: are all tasks already active??
      _switched_sync = true; // active tasks would remain the same
      _config->_fifo = 1;
      // NEXT ROUND
      // _task_ctrl->reset_task_queues();
      simulate_slice(); // this should consider condition of switched_sync()
      // simulate_graphmat_slice(); // this should consider condition of switched_sync()
    }
    // this is for correctness check??
    for (int i = 0; i < _graph_vertices; ++i)
    {
      assert(_scratch[i] <= _vertex_data[i]);
      _vertex_data[i] = _scratch[i];
    }
  }
#endif
#if ASYNC_NO_SWITCH == 0
  // assert(SGU_SLICING==0 && "cannot switch to async from async");
  if (_global_iteration == _async_tipping_point && slice_id != -1 && !_switched_async)
  {
    // push all pending tasks in the task queue
    cout << "SWITCH TO ASYNC EXECUTION\n";
    // TODO: SGU should not consider cross-slice vertices differently in extreme iterations
    // seems like it won't be true as sgu_slicing is off
    _switched_async = true;
    int tot_tasks_copied = 0;
    for (int reuse = 0; reuse < REUSE; ++reuse)
    {
      for (unsigned i = 0; i < _delta_updates[reuse].size(); ++i)
      {
        int src_id = _delta_updates[reuse][i].first;
        int old_dist = _vertex_data[src_id];
        assert(old_dist == _scratch[src_id]);
        _vertex_data[src_id] = reduce(_vertex_data[src_id], _delta_updates[reuse][i].second);
        if (old_dist != _vertex_data[src_id])
        {
          _scratch[src_id] = _vertex_data[src_id];
          task_entry new_task(src_id, _offset[src_id]);
          new_task.dist = _scratch[src_id];
          DTYPE prio = apply(_scratch[src_id], src_id);
          if (_switched_cache)
          {
            _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(src_id), prio, new_task);
          }
          else
          {
            _task_ctrl->push_task_into_worklist(_scratch_ctrl->get_slice_num(src_id), prio, new_task);
          }
          tot_tasks_copied++;
        }
      }
      _delta_updates[reuse].clear();
    }
    // prac has to be somehow 1?
    if (_switched_cache)
    {
      for (int slice = 0; slice < SLICE_COUNT; ++slice)
      {
        _task_ctrl->move_tasks_from_wklist_to_tq(slice);
      }
    }
    for (int i = 0; i < _graph_vertices; ++i)
    {
      if (_vertex_data[i] != _scratch[i])
      {
        _vertex_data[i] = _scratch[i];
        task_entry new_task(i, _offset[i]);
        new_task.dist = _scratch[i];
        tot_tasks_copied++;
        DTYPE prio = apply(_scratch[new_task.vid], new_task.vid);
        if (_switched_cache)
        {
          _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(i), prio, new_task);
        }
        else
        {
          _task_ctrl->push_task_into_worklist(_scratch_ctrl->get_slice_num(i), prio, new_task);
        }
      }
    }
    for (int i = 0; i < _graph_vertices; ++i)
    {
      _scratch[i] = _vertex_data[i];
    }
    cout << "Total tasks copied: " << tot_tasks_copied << endl;
    if (_switched_cache)
    {
      cout << "Calling cache simulate\n";
      simulate();
    }
    else
    {
      // FIXME: it should push all pending tasks in the worklists?
      _async_finishing_phase = true;
      simulate();
      cout << "Done accumulating initial tasks\n";
      _async_finishing_phase = false;
      simulate_sgu_slice(); // the tasks should be copied to worklists in this case
    }
#if SLICE_COUNT == 2
    // Shouldn't this be 0?
    cout << "Worklist size: " << _task_ctrl->_worklists[0].size() << " and second: " << _task_ctrl->_worklists[1].size() << endl;
#endif
    for (int i = 0; i < _graph_vertices; ++i)
    {
      assert(_scratch[i] <= _vertex_data[i]);
      _vertex_data[i] = _scratch[i];
    }
  }
#endif
#endif
}

void asic::calc_and_print_training_error()
{
  float error = 0;
  for (int i = 0; i < V; ++i)
  {
    DTYPE diff = abs(_correct_vertex_data[i] - _scratch[i]);
    if (_scratch[i] == MAX_TIMESTAMP)
    { // just because the initial random is too bad here..
      /*if(i==4) {
        cout << "diff: " << diff << endl;
      }*/
      diff = 256 * diff / MAX_TIMESTAMP;
    }
    error += diff;
    /*if(i==4) {
      cout << "Correct vertex data: " << _correct_vertex_data[i] << " scratch: " << _scratch[i] << " diff: " << diff << endl;
    }*/
  }
  _prev_error = error / V;
  cout << "Cycles: " << _cur_cycle << " error: " << _prev_error << endl;
}

// FIXME: it needs to move the current tasks to active cores;
// for any intermediate data in the buffers, they should be flushed out for a few cycles
// Let me run a phase for the reconfigured cores where all ready tasks are pushed to the
// waiting buffers; other pipeline stages are completed.
int asic::reconfigure_fine_core(int core_id, int cores_to_mult)
{
  // FIXME: checking: remove
  // if(core_id==0) return;
  cout << "Resetting core_id: " << core_id << " for reconfiguration at cycle: " << _cur_cycle << endl;
  // In my current implementation, this should be in the same core -- completion buffer do not record waiting addresses
  // queue<task_entry> half_tasks;
  int tasks_moved = 0;
  int allot_core = 0;
  // TODO: empty its fifo task queue as well?
  while (!_asic_cores[core_id]->_fifo_task_queue.empty())
  {
    task_entry cur_task = _asic_cores[core_id]->_fifo_task_queue.front();
    ++_total_tasks_moved_during_reconfig;
    ++tasks_moved;
    // FIXME: this should definitely not be the current core (keep another flag)
    // TODO: should not have been random for locality...
    int start_core = rand() % core_cnt;
    for (int x = start_core, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
    {
      if (!_fine_inactive[x])
      {
        allot_core = x;
        break;
      }
    }
    _task_ctrl->insert_local_task(0, 0, 0, allot_core, cur_task);
    _asic_cores[core_id]->_fifo_task_queue.pop();
  }
  for (auto it = _asic_cores[core_id]->_task_queue[0][0].begin(); it != _asic_cores[core_id]->_task_queue[0][0].end();)
  {
    bool empty = false;
    for (auto it2 = (it->second).begin(); it2 != (it->second).end();)
    {
      task_entry cur_task = *it2; // (it->second).front();

      if (cur_task.start_offset != _offset[cur_task.vid])
      {
        ++it2;
        continue;
      }

      // TODO: should not have been random for locality...
      int start_core = rand() % core_cnt;
      for (int x = start_core, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
      {
        if (!_fine_inactive[x])
        {
          allot_core = x;
          break;
        }
      }
      _task_ctrl->insert_local_task(0, 0, 0, allot_core, cur_task);
      ++_total_tasks_moved_during_reconfig;
      ++tasks_moved;
      ++_task_ctrl->_remaining_task_entries[core_id][0];

      // _task_ctrl->insert_local_task(0, 0, 0, allot_core, cur_task);
      // (it->second).pop_front();
      it2 = (it->second).erase(it2); // FIXME:: free
      if ((it->second).empty())
      {
        // _asic_cores[core_id]->_task_queue[0][0].erase(it->first);
        empty = true;
        it = _asic_cores[core_id]->_task_queue[0][0].erase(it);
        break;
        // it = _asic_cores[core_id]->_task_queue[0][0].erase(it->first);
      }
    }
    if (!empty && !(it->second).empty())
      ++it;
    // it = _asic_cores[core_id]->_task_queue[0][0].begin();
  }
  cout << "Tasks moved while reconfiguring core: " << core_id << " is: " << tasks_moved << endl;
  bool term_flag = false;
  tasks_moved = 0;
  // flushing the pipeline -- something cannot be emptied...
  // hygcn so should not push any task dynamically
  // that should have been in parallel; it should push to destination cores
  _reconfig_flushing_phase = true;
  while (!term_flag)
  {
    ++_cur_cycle;
    term_flag = true;
    // _asic_cores[core_id]->serve_atomic_requests();
    _mem_ctrl->_mem->update();
    _asic_cores[core_id]->cycle(); // max CB entries + fifo_size (32+32)*8+many edges
    // TODO: instead replace with a pending task buffer in replacement for stream controller
    if (_asic_cores[core_id]->_task_queue[0][0].size() > 0 && _compl_buf[core_id]->_entries_remaining > 2)
    {
      /*if(_cur_cycle>=1000 && core_id==11) {
        auto it = _asic_cores[core_id]->_task_queue[0][0].begin();
        int vid = (it->second).front().vid;
        cout << " start_offset: " << (it->second).front().start_offset << " original offset: " << _offset[vid] << " end offset: " << _offset[vid+1] << endl;
      }*/
      _asic_cores[core_id]->dispatch(0, 0);
    }
    _network->cycle(); // to prevent the network deadlock (should only consume current packets inside the current core because their destination is set)
    // TODO: we need to check at each core??
    if (!_asic_cores[core_id]->local_pipeline_inactive(false))
    {
      term_flag = false;
    }
    // _asic_cores[core_id]->local_pipeline_inactive(true);
    ++tasks_moved;
    // ++_cur_cycle;
  }
  _reconfig_flushing_phase = false;
  // much less cycles -- also should be done in parallel
  cout << "Done resetting core_id: " << core_id << " at cycle: " << _cur_cycle << " in cycles: " << tasks_moved << endl;

  _cur_cycle -= tasks_moved;
  // TODO: pull some multiplication tasks from an active core?
  // randomly select a core to pull tasks from -- this is work stealing.
  /*allot_core = -1;
  int start_core = rand()%core_cnt;
  for(int x=start_core, i=0; i<core_cnt; x=(x+1)%core_cnt, ++i) {
    if(_mult_cores[x]->_local_coarse_task_queue.size()>0 && _mult_cores[x]->_local_coarse_task_queue.front().weight_rows_done==0) {
    // if(!_fine_inactive[x] && _mult_cores[x]->_local_coarse_task_queue.front().weight_rows_done==0) {
      allot_core=x;
      break;
    }
  }*/

  // is stealing half tasks correct?
  /*allot_core = _task_ctrl->find_max_coarse_task_core();

  // TODO: maybe they are all waiting for their turn in the linear prefetch queue
  // TODO: I should limit that..?? (with the size of the buffer, not just while)
  if(allot_core!=-1) {
    int mult_tasks_moved=0;
    // TODO: 8 entries are corresponding to a single multiplication; can only pop from scratch_process in groups
    int size = _mult_cores[allot_core]->_local_coarse_task_queue.size();
    for(int i=0; i<size/2; ++i) {
      mult_task t = _mult_cores[allot_core]->_local_coarse_task_queue.front();
      _task_ctrl->insert_local_coarse_grained_task(core_id, t.row, t.weight_rows_done, t.second_buffer);
      _mult_cores[allot_core]->_local_coarse_task_queue.pop();
      ++tasks_moved;
    }
  }
  cout << "multiplication tasks moved: " << tasks_moved << endl;
  for(int c=0; c<core_cnt; ++c) {
    cout << "Live tasks: " << _mult_cores[c]->get_live_tasks_per_dfg(0);
  }*/
  return tasks_moved;
}

// TODO: while setting; we should also flush out the multiplication pipeline
// Somehow during this phase, this is flushed...
// flushing the second dfg
void asic::flush_multiplication_pipeline(int core_id)
{
  if (_config->_prefer_tq_latency)
  {
    return;
  }
  // FIXME: checking: remove
  bool term_flag = false;
  cout << "Starting flush of the multiplication pipeline at cycle: " << _cur_cycle << "\n";
  int cycles_taken = 0;
  // DFG-ID=2 should be flushed; task queue is common so no problem
  while (!term_flag)
  {
    term_flag = true;
    ++_cur_cycle;
    ++_stats->_cycles_in_flushing_reconfiguration;

    // TODO: need to flush the scratch reorder buffer as well (how does it get
    // the data served?)
    for (int c = 0; c < core_cnt; ++c)
    {
      if (_cur_cycle % 10 == 0)
      {
        // _mult_cores[c]->fill_updates_from_overflow_buffer();
        _local_coalescer[c]->fill_updates_from_overflow_buffer();
        _remote_coalescer[c]->fill_updates_from_overflow_buffer();
      }
      _mult_cores[c]->serve_local_hats_requests();
    }
    _network->cycle();

    // dispatch any task whose weight rows done is not 0
    // if entries are not empty, then what do we do?? (we should do cb_to_lsq())
    // Note that this could be any dfg_id
    /*if((_mult_cores[core_id]->_pending_task.weight_rows_done>0)
    && (_mult_cores[core_id]->_pending_task.dfg_id==1)
    && (_coarse_reorder_buf[core_id][1]->_entries_remaining>0)) { // (FEAT_LEN-_mult_cores[core_id]->_local_coarse_task_queue.front().weight_rows_done))) {
      // mult_task t = _mult_cores[core_id]->_local_coarse_task_queue.front();
      // _mult_cores[core_id]->_local_coarse_task_queue.pop();
      mult_task t = _mult_cores[core_id]->_pending_task;
      vector<int> rows; rows.push_back(t.row); vector<int> weight_rows_done; weight_rows_done.push_back(t.weight_rows_done);
      vector<bool> second_buffer; second_buffer.push_back(t.second_buffer);
      _mult_cores[core_id]->dispatch(1, rows, weight_rows_done, second_buffer);
    }*/
    _mult_cores[core_id]->reduce(1);
    _mult_cores[core_id]->process(1);

    // if done in later cycles, then??
    _mult_cores[core_id]->push_scratch_data_to_pipeline(1);
    while (!_coarse_reorder_buf[core_id][1]->_pref_lsq.empty() && _mult_cores[core_id]->can_push_in_scratch_process(1))
    {
      _mult_cores[core_id]->push_scratch_data(1, _coarse_reorder_buf[core_id][1]->receive_lsq_responses());
    }
    _coarse_reorder_buf[core_id][1]->cb_to_lsq();

    // TODO: other stages are also required!!!
    // TODO: we need to check at each core??
    if (!_mult_cores[core_id]->_process_reduce[1].empty())
      term_flag = false;
    if (!_mult_cores[core_id]->_lsq_process[1].empty())
      term_flag = false;
    if (!_mult_cores[core_id]->_scratch_process[1].empty())
      term_flag = false;
    // if(_mult_cores[core_id]->_pending_task.weight_rows_done==0) term_flag=true;
    if (!_coarse_reorder_buf[core_id][1]->pipeline_inactive(false))
      term_flag = false;
    ++cycles_taken;
  }
  cout << "Flushing multiplication pipeline for core_id: " << core_id << " in cycles: " << cycles_taken << endl;
  /*if(_config->_prefer_tq_latency) {
    assert(cycles_taken==1 && "for tq latency, there should not be any data in the second dfg");
  }*/
}

// TODO: apply our memory/throughput logic here...
void asic::update_heterogeneous_core_throughput()
{
  /*if(_config->_prefer_tq_latency) {
    return;
  }*/
  if (_config->_core_cnt == 1)
  {                                                                          // ideal scenario
    _asic_cores[0]->_fine_grain_throughput = 16 * _config->_process_thr * 2; // 2;
    _mult_cores[0]->_data_parallel_throughput = 16 * 2;                      // 2; are its independent dfg unbalanced?
    cout << "Assuming an idealized scenario of a single core -- thus llocated 16 to agg and 32 to mult\n";
    return;
  }
  // this didn't impact performance somehow; most likely it is unable to find a new task?? (we should print average throughput like in PRAC)
  if (_config->_domain == tree)
  {
    for (int c = 0; c < core_cnt; ++c)
    {
      _mult_cores[c]->_data_parallel_throughput = MAX_DFGS; // for independent lanes
      // _coarse_alloc[c]=2; // for reduction in latency
    }
    return;
  }
  // Let's keep it same as the previous iteration
  // FIXME: OR we should consider dynamic tasks created??

  // _coarse_created_last_iteration = _stats->_stat_tot_created_data_parallel_tasks - _coarse_created_last_iteration;
  // _edges_created_last_iteration = _stats->_stat_tot_created_edge_tasks - _edges_created_last_iteration;
  _coarse_created_last_iteration = _stats->_stat_tot_created_data_parallel_tasks;
  _edges_created_last_iteration = _stats->_stat_tot_created_edge_tasks;

  int predicted_agg_flops = _edges_last_iteration * FEAT_LEN / core_cnt;   // 0 or 1
  int predicted_mult_flops = _coarse_tasks_last_iteration * 16 / core_cnt; // 1 or 2

  // FIXME: When we came to this, it executed more edges than earlier (3.97x)
  // And also more multiplications...
  // 1 reason: it moved more tasks; 2nd reason: it created earlier so more tasks are created
  // predicted_agg_flops = 2*_edges_created_last_iteration*E/V;
  // predicted_mult_flops = _coarse_created_last_iteration*FEAT_LEN;

  // // active tasks in the task queue/prefetch_process queue
  int degree = E / V;
  predicted_agg_flops = _task_ctrl->get_live_fine_tasks() * degree;
  predicted_mult_flops = _task_ctrl->get_live_coarse_tasks() * FEAT_LEN;
  cout << "Edges created last iteration: " << _edges_created_last_iteration << " coarse-tasks: " << _coarse_created_last_iteration << endl;
  cout << "Predicted agg flops: " << predicted_agg_flops << " predicted_mult_flops: " << predicted_mult_flops << endl;

  // predicted_agg_flops *= 8;
  predicted_agg_flops += (_task_ctrl->get_live_coarse_tasks() * degree); // if each mult will create agg.
  predicted_mult_flops += (_task_ctrl->get_live_fine_tasks() * FEAT_LEN);
  // THIS GIVES POOR PERFORMANCE
  /*if(_stats->_stat_barrier_count < GCN_LAYERS-2 && _task_ctrl->get_live_fine_tasks()<(STATS_INTERVAL*core_cnt*0.5)) {
    predicted_agg_flops += (_task_ctrl->get_live_coarse_tasks()*degree); // if each mult will create agg.
  }
  if(_task_ctrl->get_live_coarse_tasks()<(STATS_INTERVAL*core_cnt*1.5)) {
    predicted_mult_flops += (_task_ctrl->get_live_fine_tasks()*FEAT_LEN);
  }*/

  // TODO: need to put as ratio
  float ratio = 1;
  if (predicted_agg_flops != 0)
  {
    ratio = predicted_mult_flops / predicted_agg_flops; // 1.5x
    cout << "Original ratio: " << ratio << " mult tasks: " << _coarse_tasks_last_iteration << " agg tasks: " << _edges_last_iteration << endl;
    ratio = ratio / (1 + ratio);
  }

  // if last iterations -- allocate most cores to aggregation
  bool end_iter = false;
  if (_stats->_stat_tot_finished_edges > (0.95 * GCN_LAYERS * E * FEAT_LEN / 16))
  {
    end_iter = true;
    ratio = 1;
  }

  // TODO: predicted throughput depends on the previous phase and also if the previous phase
  // had static tasks; we should consider it less.
  // if(ratio>1) ratio=1; // if 4x, then agg should get 1/5 cores; ratio=4/5
  int cores_to_agg = (1 - ratio) * 2 * core_cnt;       // do not need to be more than that
  cores_to_agg = std::min(core_cnt, cores_to_agg + 2); // std::max(cores_to_agg, 2);
  if (cores_to_agg == 2 && !end_iter)
  {
    cores_to_agg = 4;
  }
  // int cores_to_mult = 2*core_cnt - cores_to_agg;
  // int extra_cores_to_mult = cores_to_mult - core_cnt;
  int extra_cores_to_mult = core_cnt - cores_to_agg;
  int duplicate_cores_to_mult = extra_cores_to_mult;
  int duplicate_cores_to_agg2 = cores_to_agg;
  int duplicate_cores_to_agg = cores_to_agg;
  if (cores_to_agg <= 8)
  {
    // cores_to_agg -= 1;
    duplicate_cores_to_agg2 += 1; // even 1 less core matters a lot.
  }
  cout << "Cores to agg: " << cores_to_agg << " extra_cores_to_mult: " << extra_cores_to_mult << endl;

  _dfg_to_agg = cores_to_agg;
  _dfg_to_mult = core_cnt + extra_cores_to_mult;

  for (int c = 0; c < core_cnt; ++c)
  {
    if (_config->_prefer_tq_latency)
    {
      _fine_alloc[c] = 1;
      _coarse_alloc[c] = 1;
      _mult_cores[c]->_dfg_latency = DFG_LENGTH;
    }
    else
    {
      _asic_cores[c]->_fine_grain_throughput = 1 * _config->_process_thr;
      _mult_cores[c]->_data_parallel_throughput = 1;
    }
  }
  // this should traverse in alternate order
  for (int c = 0; c < core_cnt; ++c)
    _fine_inactive[c] = true;
  for (int c = 0; c < core_cnt; c += 2)
  {
    if (cores_to_agg > 0 || c < 2)
    {
      _fine_inactive[c] = false;
      --duplicate_cores_to_agg;
    }
  }
  for (int c = 1; c < core_cnt; c += 2)
  {
    if (cores_to_agg > 0 || c < 2)
    {
      _fine_inactive[c] = false;
      --duplicate_cores_to_agg;
    }
  }

  int max_flush_cycles = 0;

  // for even cores
  for (int c = 0; c < core_cnt; c += 2)
  {
    if (cores_to_agg > 0 || c < 2)
    {
      if (c > 1)
        flush_multiplication_pipeline(c);
      _asic_cores[c]->_fine_grain_throughput = 1 * _config->_process_thr;
      --cores_to_agg;
    }
    else if (extra_cores_to_mult > 0)
    {
      int flush_cycles = reconfigure_fine_core(c, duplicate_cores_to_mult);
      if (flush_cycles > max_flush_cycles)
      {
        max_flush_cycles = flush_cycles;
      }
      if (_config->_prefer_tq_latency)
      {
        _fine_alloc[c] = 0;
        _coarse_alloc[c] = 2;
        _mult_cores[c]->_dfg_latency = DFG_LENGTH; // /2;
      }
      else
      {
        _asic_cores[c]->_fine_grain_throughput = 0;
        _mult_cores[c]->_data_parallel_throughput = 2;
      }

      --extra_cores_to_mult;
    }
    cout << "Cycle: " << _cur_cycle << " at core: " << c
         << " fine throughput: " << _asic_cores[c]->_fine_grain_throughput
         << " and coarse throughput: " << _mult_cores[c]->_data_parallel_throughput
         << " ratio was: " << ratio << endl;
    if (_config->_prefer_tq_latency)
    {
      cout << "Coarse alloc: " << _coarse_alloc[c] << " fine alloc: " << _fine_alloc[c] << endl;
    }
  }

  // for odd cores
  for (int c = 1; c < core_cnt; c += 2)
  {
    if (cores_to_agg > 0 || c < 2)
    {
      if (c > 1)
        flush_multiplication_pipeline(c);
      _asic_cores[c]->_fine_grain_throughput = 1 * _config->_process_thr;
      --cores_to_agg;
    }
    else if (extra_cores_to_mult > 0)
    {
      int flush_cycles = reconfigure_fine_core(c, duplicate_cores_to_mult);
      if (flush_cycles > max_flush_cycles)
      {
        max_flush_cycles = flush_cycles;
      }
      if (_config->_prefer_tq_latency)
      {
        _fine_alloc[c] = 0;
        _coarse_alloc[c] = 2;
        _mult_cores[c]->_dfg_latency = DFG_LENGTH; // /2;
      }
      else
      {
        _asic_cores[c]->_fine_grain_throughput = 0;
        _mult_cores[c]->_data_parallel_throughput = 2;
      }

      --extra_cores_to_mult;
    }
    cout << "Cycle: " << _cur_cycle << " at core: " << c
         << " fine throughput: " << _asic_cores[c]->_fine_grain_throughput
         << " and coarse throughput: " << _mult_cores[c]->_data_parallel_throughput
         << " ratio was: " << ratio << endl;
    if (_config->_prefer_tq_latency)
    {
      cout << "Coarse alloc: " << _coarse_alloc[c] << " fine alloc: " << _fine_alloc[c] << endl;
    }
  }

  cout << "Incremented flush cycles of: " << max_flush_cycles << " at cycle: " << _cur_cycle << endl;
  _cur_cycle += max_flush_cycles;
  _stats->_cycles_in_flushing_reconfiguration += max_flush_cycles;

  // cout << "initial cores to agg: " << duplicate_cores_to_agg2 << endl;
  for (int c = 0; c < core_cnt; ++c)
  {
    bool cond = false;
    if (_config->_prefer_tq_latency)
    {
      cond = (_fine_alloc[c] == 1);
    }
    else
    {
      cond = (_asic_cores[c]->_fine_grain_throughput == _config->_process_thr);
    }
    if (cond)
    {
      // cout << "condition true for core: " << c << endl;
      --duplicate_cores_to_agg2;
    }
  }
  assert(duplicate_cores_to_agg2 == 0 && "should have allocated same cores as decided");

  // FIXME: checking: remove (if variable, it will need to flush)
  // _mult_cores[15]->_data_parallel_throughput=32; // (32-cores_to_agg); // 2;
  /*_asic_cores[0]->_fine_grain_throughput=16*_config->_process_thr*2; // (32-cores_to_agg); // 2;
  for(int c=1; c<core_cnt; ++c) {
    _asic_cores[c]->_fine_grain_throughput=0;
  }*/

  /*for(int c=0; c<core_cnt; ++c) {

    // TODO: in this scenario, aggregation tasks are also being produced online. So we need to
    // predict aggregation tasks from the previous cycle

    // here most tasks are stalled by computation (check prefetch_process and lsq_process instead)
    // Okay, at this cycle; these queues may be empty at this cycle but they will be filled during other cycles
    // FIXME: but the pending aggregation tasks should be somewhere...
    // cout << "Cycle: " << _cur_cycle << " core: " << c 
    // << " fine task_queue_size: " << _asic_cores[c]->_task_queue[0][0].size()
    // << " coarse task queue size: " << _mult_cores[c]->_local_coarse_task_queue.size() << endl;
    // cout << "pref_lsq size: " << _asic_cores[c]->_pref_lsq[0].size()
    // // << " cgra_event_queue: " << _asic_cores[c]->_cgra_event_queue.size()
    // << " lsq_process_size: " << _mult_cores[c]->_lsq_process.size() << endl;
    // if(_asic_cores[c]->_task_queue[0][0].size()==0) {
    // if(_mult_cores[c]->_local_coarse_task_queue.size()==0) {
    if(predicted_agg_flops==0) {
      _asic_cores[c]->_fine_grain_throughput=0;
      _mult_cores[c]->_data_parallel_throughput=2*_config->_process_thr;
      cout << "Setting core: " << c << " to all multiplication at cycle: " << _cur_cycle << endl;   
    } else if(predicted_mult_flops==0) {
      _asic_cores[c]->_fine_grain_throughput=2*_config->_process_thr;
      _mult_cores[c]->_data_parallel_throughput=0;
      cout << "Setting core: " << c << " to all aggregation at cycle: " << _cur_cycle << endl;   
    } else {
      _asic_cores[c]->_fine_grain_throughput=1*_config->_process_thr;
      _mult_cores[c]->_data_parallel_throughput=1;
      cout << "Setting core: " << c << " to both at cycle: " << _cur_cycle << endl;   
    }

  }*/
}

/*
1. the number of start vertices
2. the number of copy vertices
3. the number of edges executed
4. memory efficiency => total memory loads (including scratch/cache)/cycles.
*/
void asic::print_local_iteration_stats(int slice_id, int reuse)
{
  if (_config->_dyn_graph == 1 && _config->_reuse_stats == 0)
  { // TODO: turning off for now
    return;
  }

  // TODO: For GCN, I would like to print occupancy of each kind of task queue
  // -- write functions

  // TODO: change fine and coarse-grain throughput of each core
  /*if(_config->_prefer_tq_latency) {
    if(_config->_algo==gcn && _config->_heter_cores==0) {
       // TODO: we should set a ratio: currently we only support 50%, 100%. We could also support 25% such that
       // in odd cycles, it is 2,2 and in even cycles, it is 1,1
       // at barrier, both sould be set to 1
       // fine tasks for this phase are done (so now do aggrefation tasks only)
       int reqd_agg = min(((_stats->_stat_barrier_count+1)*_graph_edges), GCN_LAYERS*_graph_edges);
       if(_stats->_stat_tot_finished_edges>=reqd_agg && _stats->_stat_tot_finished_edges<(1.05*reqd_agg) && _stats->_stat_barrier_count>0) {
         _coarse_alloc=2;
         _fine_alloc=2-_coarse_alloc;
       }
       // coarse tasks for this phase are done or the first layer (generally this should be true only for the first layer)
       int reqd_mult = ((_stats->_stat_barrier_count)*_graph_vertices*FEAT_LEN);
       if(_stats->_stat_finished_coarse_tasks==reqd_mult || (_stats->_stat_barrier_count==0)) {
           _fine_alloc = 2;
           _coarse_alloc = 2-_fine_alloc;
       }
       cout << "Cycle: " << _cur_cycle << " fine tasks: " << _task_ctrl->get_live_fine_tasks() << " coarse tasks: " << _task_ctrl->get_live_coarse_tasks();
       cout << " barrier: " << _stats->_stat_barrier_count;
       cout << " Fine core throughput: " << _fine_alloc << " coarse core throughput: " << _coarse_alloc << endl;
    }
  }*/

  _coarse_tasks_last_iteration = (_stats->_stat_finished_coarse_tasks - _coarse_tasks_last_iteration);
  _edges_last_iteration = _stats->_stat_tot_finished_edges - _edges_last_iteration;
  if (_config->_algo == gcn)
  {
    assert(_edges_last_iteration <= (core_cnt * STATS_INTERVAL) && "edges should not exceed maximum throughput");
  }
  _tot_mem_reqs_per_iteration += (_edges_last_iteration * message_size);
  _cycles_per_iteration = (_cur_cycle - _cycles_last_iteration);
  // cout << "Barrier at cycle: " << _cur_cycle << " and cumulative gradient: " << _prev_gradient/V << endl;
  _task_ctrl->_present_in_queue.reset();
  // calc_and_print_training_error();
  cout << "Cyclic stat selection at cycle: " << _cur_cycle << " and cumulative gradient: " << _prev_gradient / _unique_vertices << " and global iteration: " << _global_iteration << endl;
  _prev_gradient = 0;

  // if(_config->_algo==gcn && _config->_heter_cores==0 && _cur_cycle>0) {
  if (_config->_heter_cores == 0)
  {
    assert((_config->_algo == gcn || _config->_domain == tree) && "we only support dynamic homogeneity ofor tree and gcn");
    if (_config->_domain == tree || _cur_cycle > 0)
    {
      update_heterogeneous_core_throughput();
    }
  }
  _coarse_tasks_last_iteration = _stats->_stat_finished_coarse_tasks;

#if 0 // SGU_SLICING==1 && WORKING_CACHE==0
   if(_cycles_per_iteration>=100000) {
     assert(_edges_last_iteration>1000000 && "possible bug, throughput is too low");
   }
#endif
  int barrier_cycles = (_graph_vertices * 8 / 512) / _slice_count;
#if SLICE_COUNT > 1 && WORKING_CACHE == 0
  if (!_switched_cache)
  {
    if (slice_id == -1)
    {
#if SYNC_SCR == 1
      _cycles_per_iteration += barrier_cycles;
      // _cur_cycle += barrier_cycles;
#endif
    } /*else {
     _cycles_per_iteration += barrier_cycles;
    // _cur_cycle += barrier_cycles;
   }*/
  }
#endif
  double mem_eff = _tot_mem_reqs_per_iteration / (double)_cycles_per_iteration;
  double work_eff = _tot_useful_edges_per_iteration / (double)_edges_last_iteration;
  _tot_useful_edges_per_iteration = 0;
  cout << "Slice: " << slice_id << ":reuse: " << reuse << ":cycles_per_iteration:" << _cycles_per_iteration << ":memory efficiency (overall mem bw in GB/s):" << mem_eff << " dram cache accesses: " << _stats->_stat_dram_cache_accesses << " thr inside iteration: " << _edges_last_iteration / (double)_cycles_per_iteration << " work efficiency: " << work_eff << endl;

  // cout << "Phase-wise atomic update load information: ";
  int sum = 0, max = 0;
  int end = core_cnt * num_banks;
  for (int i = 0; i < end; ++i)
  {
    // cout << _gcn_updates[i] << " ";
    int cur_updates = _gcn_updates[i] - _prev_gcn_updates[i];
    _prev_gcn_updates[i] = _gcn_updates[i];
    sum += cur_updates;
    if (cur_updates > max)
      max = cur_updates;
  }
  // cout << endl;
  cout << "Phase-wise load imbalance loss: " << max / (float)(sum / (float)end) << " maximum were: " << max << endl;

  /*
#if SLICE_COUNT>1 && WORKING_CACHE==0
   if(!_switched_cache) {
   if(slice_id==-1) {
#if SYNC_SCR==1
    _cycles_per_iteration -= (_graph_vertices*8/512)/_slice_count;
#endif
   } else {
     _cycles_per_iteration -= (_graph_vertices*8/512)/_slice_count;
   }
   }
#endif
   */

  bool go_in = _switched_async;
#if SGU == 1
  go_in = true;
#endif
  if (go_in)
  {
    for (int c = 0; c < core_cnt; ++c)
    {
      _start_active_vertices += _asic_cores[c]->_fifo_task_queue.size();
      for (int t = 0; t < NUM_TQ_PER_CORE; ++t)
      {
        _start_active_vertices += (TASK_QUEUE_SIZE - _task_ctrl->_remaining_task_entries[c][t]);
      }
    }
  }

  cout << "Stats for dynamic switch hint (global), edges executed: " << _edges_last_iteration << ":initial active vertices: " << _start_active_vertices << ":initial copy vertices: " << _start_copy_vertex_update << endl;

  perform_algorithm_switching(slice_id, mem_eff);

  // both would have achieved in that ratio only
  // But if new vertices are requested only after edges, cycle should be same
  // and this should be normal bandwidth in the ratio of requested data...
  // float bw_owned_vertices=_owned_vertex_reqs_per_iterations/(_last_vertex_req_cycle-_cycles_last_iteration);
  // float bw_edges_and_cross-slice_vertices=(_edges_last_iteration*message_size)/(_last_edge_req_cycle-_cycles_last_iteration); // cross-slice vertices matter only for async
  // float req_ratio = (_edges_last_iteration*message_size)/(float)_tot_mem_reqs_per_iteration;

  // TODO: we need it only for a single iteration
  if (_config->_reuse_stats == 1 && _edges_last_iteration > 0)
  {
    _stats->print_freq_stats();
    _stats->reset_freq_stats();
  }
  _edges_last_iteration = _stats->_stat_tot_finished_edges;
  _cycles_last_iteration = _cur_cycle;
  _tot_mem_reqs_per_iteration = 0;

  // TODO: print load imbalance stats
  if (_config->_algo == gcn)
  {
    cout << "Multiply tasks: ";
    for (int c = 0; c < core_cnt; ++c)
    {
      for (int i = 0; i < 2; ++i)
      {
        int load = _mult_cores[c]->get_live_tasks_per_dfg(i);
        if (load < 0)
        {
          cout << "Detect an error breakpoint\n";
        }
        cout << "Core: " << c << " dfg_id: " << i << " load: " << load << " ";
      }
    }
    cout << endl;
  }
}

void asic::print_stats()
{
  if (_already_printed_stats)
    return;
  _already_printed_stats = true;
  if (_config->_detailed_stats == 0)
  {
    cout << "Cycles: " << _cur_cycle << endl;
    finish_simulation();
    return;
  }
  if (_config->_hats == 1)
  {
    cout << "Total issue: " << _stats->_num_spatial_issue << " batched: " << _stats->_num_tasks_batched << " spatial average: " << _stats->_num_tasks_batched / (float)_stats->_num_spatial_issue << endl;
    cout << "temporal average: " << _stats->_num_spatial_issue / (float)_stats->_num_temporal_issue << endl;
    assert((_config->_domain == graphs || _stats->_num_tasks_batched - 1 == NUM_KNN_QUERIES) && "total batching should be equal to knn queries");
  }
  if (_config->_domain == tree)
  {
    cout << "Queries: " << NUM_KNN_QUERIES << " data per query: " << NUM_DATA_PER_LEAF << endl;
    cout << "process_thr of a core: " << process_thr << endl;
    float ideal_hit_rate = (1 - (_unique_leaves / (double)NUM_KNN_QUERIES));
    // Oh I did not consider memory bandwidth correctly
    float ideal_throughput = ideal_hit_rate * core_cnt * process_thr + (1 - ideal_hit_rate) * (512 / 4); // 128;
    int num_cache_lines = NUM_DATA_PER_LEAF / (line_size / message_size);
    int ideal_cycles = (NUM_KNN_QUERIES * (NUM_DATA_PER_LEAF + kdtree_depth)) / ideal_throughput;
    int ideal_mem_cycles = (_unique_leaves * NUM_DATA_PER_LEAF * message_size) / 512;
    ideal_mem_cycles *= ((NUM_KNN_QUERIES / num_kdtree_leaves) / (MULTICAST_BATCH * MAX_DFGS)); // 20000/400 = 50/2 = 25

    // cout << "Cycles: " << _cur_cycle << " (" << ideal_cycles << ") " << endl;
    // cout << "Ideal memory cycles: " << ideal_mem_cycles << endl;
    cout << "Cycles: " << _cur_cycle << " (" << ideal_mem_cycles << ") " << endl;
    cout << "Misses in cache-hit: " << _stats->_stats_misses_in_cache_hit << " accesses: " << _stats->_stats_accesses_in_cache_hit
         << " coalesces in cache-hit: " << _stats->_stats_coalesces_in_cache_hit << endl;
    cout << "Data-parallel times: " << _stats->_stat_vec_mat / num_cache_lines << endl;
    cout << "Delayed cycles: " << _stats->_stat_delayed_batch_cycles / core_cnt << " batched tasks: " << _stats->_stat_batched_tasks << " cycles active: " << _stats->_stat_cycles_batched << endl;
    for (int c = 0; c < core_cnt; ++c)
    {
      cout << "Cycles stalled for completion buffer: " << _stats->_stat_mem_stall_thr[c] << " for no-requests: " << _stats->_stat_stall_thr[c] << endl;
    }
    // cout << "IDEAL CYCLES: " << (NUM_KNN_QUERIES*(NUM_DATA_PER_LEAF+KDTREE_DEPTH))/256 << endl;
    cout << "Main memory cache line accesses using dramsim: " << _stats->_stat_dram_cache_accesses / (float)_cur_cycle << endl;
    // cout << "INHERENT RESUE: " << (1-(_unique_leaves/(double)NUM_KNN_QUERIES)) << endl;
    cout << "L2 hits: " << _mem_ctrl->_l2hits << " hit rate: " << _mem_ctrl->_l2hits / (double)_mem_ctrl->_l2accesses << " (" << ideal_hit_rate << ") " << endl;
    cout << "Per-core-hit-rate: ";
    for (int c = 0; c < core_cnt; ++c)
    {
      cout << " c: " << c << " hit-rate: " << _mem_ctrl->_l2hits_per_core[c] / (float)_mem_ctrl->_l2accesses_per_core[c];
    }
    cout << endl;
    cout << "Per-core-accesses: ";
    for (int c = 0; c < core_cnt; ++c)
    {
      cout << " c: " << c << " access: " << _mem_ctrl->_l2accesses_per_core[c];
    }
    cout << endl;

    cout << "L2 accesses: " << _mem_ctrl->_l2accesses << " l2 coalesces: " << _mem_ctrl->_l2coalesces << " cache size: " << L2SIZE << endl;
    cout << "Received mem reqs: " << _stats->_stat_received_mem << " coalesced: " << _stats->_stat_coalesced_mem
         << " unique memory reqs: " << (_stats->_stat_received_mem - _stats->_stat_coalesced_mem) << endl;
    cout << "Finished edges: " << _stats->_stat_global_finished_edges << endl;
    int zero_banks = 0;
    cout << "Total requests per bank: ";
    for (int i = 0; i < num_banks; ++i)
    {
      cout << _stats->_stat_num_req_per_bank[i] << " ";
      if (_stats->_stat_num_req_per_bank[i] == 0)
      {
        zero_banks++;
      }
    }
    return;
  }
  if (_config->_update_coalesce && _config->_cache_hit_aware_sched)
  {
    for (int i = 0; i < V; ++i)
    {
      assert(_task_ctrl->_present_in_miss_gcn_queue[i] == 0 && "all entries should have been served");
    }
  }

  cout << "Fine-grained stats collection interval: " << STATS_INTERVAL << endl;
  if (_config->_cache_hit_aware_sched == 1)
  {
    cout << "Number of tasks delayed with cache hit aware schedule: " << _stats->_stat_delayed_cache_hit_tasks << endl;
    cout << "Number of tasks being pushed through cache hit aware schedule: " << _stats->_stat_allowed_cache_hit_tasks << endl;
    cout << "Number of tasks correctly checked cache behavior : " << _stats->_stat_correct_cache_hit_tasks << endl;
    cout << "Number of tasks wrongly checked cache behavior : " << _stats->_stat_wrong_cache_hit_tasks << endl;
    cout << "Cycles for hit queue : " << _stats->_stat_cycles_hit_queue_is_chosen << " and miss queue: " << _stats->_stat_cycles_miss_queue_is_chosen << endl;
  }
  /*int tot_mult_send=0;
  for(int i=0; i<core_cnt; ++i) {
    cout << "BW of matrix multi saved: " << _mult_cores[i]->_stat_exp_bw_save << endl;
    tot_mult_send += _mult_cores[i]->_num_wgt_reqs_really_sent;
  }
  cout << "Total unique weight matrices sent: " << tot_mult_send << endl;
   */
  cout << "Number of coalesces at banks: " << _stat_bank_coalesced << endl;
  cout << "Number of edges extra done during pull async: " << _stat_extra_edges_during_pull_hack << endl;
  cout << "Edges served: " << _edges_served << " total edges: " << E << endl;
  cout << "Percentage of tasks moved: " << _task_ctrl->_tasks_moved / (float)_task_ctrl->_tasks_inserted << endl;
  cout << "********* DEBUG **********\n";

  cout << "Async tipping point for hybrid execution: " << _async_tipping_point << endl;
  cout << "Cache tipping point for hybrid execution: " << _cache_tipping_point << endl;
  cout << "slice count: " << _slice_count << " iter: " << SLICE_ITER << endl;
  cout << "crossbar bw: " << CROSSBAR_BW << " and used: " << _xbar_bw << " model net delay: " << MODEL_NET_DELAY << " crossbar latency: " << CROSSBAR_LAT << endl;
  cout << "Number of task queues: " << NUM_TQ_PER_CORE << " with size: " << TASK_QUEUE_SIZE << endl;
  cout << "epsilon for PR: " << epsilon << endl;
  cout << "Bus width: " << bus_width << endl;
  cout << "Entries pushed to ps: " << _stats->_stat_pushed_to_ps << " with max CB SIZE: " << COMPLETION_BUFFER_SIZE << endl;
  cout << "Extra traffic in graphmat: " << _graphmat_extra_traffic << endl;
  cout << "FEAT_LEN: " << FEAT_LEN << " VEC_LEN: " << VEC_LEN << endl;
  cout << "Required traffic in graphmat: " << _graphmat_required_traffic << endl;
  cout << "message size: " << message_size << endl;
  int barrier_cycles = _stats->_stat_barrier_count * _graph_vertices * message_size * 2 / 512;
  float ovhd = (_graphmat_required_traffic) / (float)(_graphmat_required_traffic + _graphmat_extra_traffic) * _cur_cycle;
  cout << "Predicted slowdown in graphmat: " << ovhd << endl;
  cout << "Predicted slowdown in graphmat: " << _cur_cycle / (float)(barrier_cycles + _cur_cycle) << endl;
  if (_config->_algo == gcn)
  {
    _stats->_stat_tot_finished_edges /= (FEAT_LEN * message_size / VEC_LEN);
  }
  cout << "Total completed tasks: " << _stats->_stat_tot_completed_tasks << endl;
  cout << "Finished edges: " << _stats->_stat_tot_finished_edges << endl;
  // if coarse-grained task on??
  // cout << "Matrix mult: " << _stats->_stat_vec_mat << endl;
  int tot_matrix_mult = _stats->_stat_vec_mat * 16 / (FEAT_LEN * FEAT_LEN);
  if (_config->_algo == gcn)
  {
    cout << "Matrix mult: " << tot_matrix_mult << endl;
    if (_config->_heter_cores == 0)
    {
      cout << "Total tasks moved duing reconfiguration: " << _total_tasks_moved_during_reconfig << endl;
    }
  }
  cout << "Number of copy vertex updates: " << _stats->_stat_global_num_copy_updates << endl;
  cout << "Number of copy vertex updates duplicated: " << _stats->_stat_num_dupl_copy_vertex_updates << endl;
  cout << "Number of owned tasks copied: " << _stats->_stat_transition_tasks << endl;

  cout << "Number of tasks stored in overflow task queue: " << _stats->_stat_overflow_tasks << endl;
  cout << "Maximum storage of fifo: " << _stats->_stat_max_fifo_storage << endl;
  cout << "Local: " << _debug_local << " remote: " << _debug_remote << " ratio: " << _debug_remote / (float)_debug_local << endl;
  for (int c = 0; c < core_cnt; ++c)
  {
    cout << "Number of busy cycles per core: " << _asic_cores[c]->_busy_cycle / (double)_cur_cycle << " tq empty cycles: " << _asic_cores[c]->_tq_empty / (double)_cur_cycle << " cb full: " << _asic_cores[c]->_cb_full / (double)_cur_cycle << " no slot for dispatch: " << _asic_cores[c]->_no_slot_for_disp / (double)_cur_cycle << endl;
  }

  if (_config->_pull_mode)
  {
    for (int c = 0; c < core_cnt; ++c)
    {
      cout << "Memory edge empty: " << _asic_cores[c]->_cycles_mem_edge_empty / (double)_cur_cycle << " fine reorder full cycles: " << _asic_cores[c]->_cycles_reorder_full / (double)_cur_cycle << " network push: " << _asic_cores[c]->_cycles_net_full / (double)_cur_cycle << endl;
    }
  }
  // TODO: I think getting maximum capacity of the overflow queue would be more
  // critical (fifo_tq + local_tq)

  cout << "********* STATS **********\n";

  int final_cycles = _cur_cycle;
  cout << "Total finished edges: " << _stats->_stat_tot_finished_edges << endl;
  if (_config->_tc)
  {
    cout << "Total triangles in TC: " << _stats->_stat_tot_triangles_found << endl;
    cout << "Agile study metric for million triangles per sec per core: " << (_stats->_stat_tot_triangles_found * 1000) / (float)(core_cnt * final_cycles) << endl;
    int number_of_two_paths = 0;
    for (int vid = 0; vid < _graph_vertices; ++vid)
    {
      int degree = _offset[vid + 1] - _offset[vid];
      number_of_two_paths += (degree * (degree - 1) / 2);
    }
    cout << "Total two-paths in TC: " << number_of_two_paths << endl;
  }
  cout << "Total duplicate edges: " << _stats->_stat_duplicate << endl;
  // number of vertices not visited at all
  int dangling = 0;
  for (int i = 0; i < LADIES_SAMPLE; ++i)
  {
    if (_is_dest_visited.test(i) == 0)
      dangling++;
  }
  cout << "Total dangling vertices: " << dangling << endl;

  cout << "Received mem reqs: " << _stats->_stat_received_mem << " coalesced: " << _stats->_stat_coalesced_mem
       << " unique memory reqs: " << (_stats->_stat_received_mem - _stats->_stat_coalesced_mem) << endl;
  int sum = 0;
  int max = 0;
  cout << "GCN load balance: ";
  if (_config->_tc)
  {
    for (int i = 0, end = MAX_DFGS; i < end; ++i)
    {
      cout << _gcn_updates[i] << " ";
      sum += _gcn_updates[i];
      if (_gcn_updates[i] > max)
        max = _gcn_updates[i];
    }
  }
  else
  {
    for (int i = 0, end = core_cnt * num_banks; i < end; ++i)
    {
      cout << _gcn_updates[i] << " ";
      sum += _gcn_updates[i];
      if (_gcn_updates[i] > max)
        max = _gcn_updates[i];
    }
  }
  cout << endl;
  cout << "Atomic update load balance factor: " << max / (float)(sum / (float)core_cnt) << endl;

  cout << "Average enqueues per core: ";
  for (int c = 0; c < core_cnt; ++c)
  {
    // cout << _stats->_stat_tot_enq_per_core[c]/(double)_cur_cycle << " ";
    cout << _stats->_stat_tot_enq_per_core[c] / (double)_tot_active_cycles[c] << " ";
  }
  cout << endl;
  //  + (_barrier_count*_graph_vertices*sizeof(int)/mem_bw); -- can prefetch actually, won't be that much
#if CF == 0
  float x = _cur_cycle; // process_thr*_cur_cycle;
  for (int c = 0; c < core_cnt; ++c)
  {
    // cout << "core: " << c << " : " << _stats->_stat_correct_edges[c]/(float)x << " " <<  _stats->_stat_incorrect_edges[c]/(float)x << " " <<_stats->_stat_stall_thr[c]/(float)x << " " << endl;
    cout << _stats->_stat_correct_edges[c] / (float)x << " " << _stats->_stat_incorrect_edges[c] / (float)x << " " << _stats->_stat_mem_stall_thr[c] / (float)x << " " << _stats->_stat_stall_thr[c] / (float)x << " " << endl;
  }
#endif
  /*for(int c=0; c<core_cnt; ++c) {
    cout << "percentage cycles when no task in queue: " << _stats->_stat_mem_stall_thr[c]/(float)_cur_cycle << endl;
    cout << "percentage cycles when conflict: " << _stats->_stat_incorrect_edges[c]/(float)_cur_cycle << endl;
    cout << "percentage cycles when no conflict: " << _stats->_stat_correct_edges[c]/(float)_cur_cycle << endl;
  }*/
  int add_on = (_stats->_stat_barrier_count * _graph_vertices * sizeof(int) / mem_bw);
  cout << "Add on cycles: " << add_on << endl;
  cout << "Cycles/task: " << _avg_task_time / (float)_stats->_stat_tot_tasks_dequed << endl;
  cout << "number of high priority tasks: " << _stats->_stat_high_prio_task << endl;
  cout << "Main memory accesses: " << _stats->_stat_tot_tasks_dequed << endl;
  cout << "Main memory cache line accesses using dramsim: " << _stats->_stat_dram_cache_accesses / (float)_cur_cycle << endl;
  cout << "Main memory bandwidth utilization: " << _stats->_stat_dram_cache_accesses * line_size / (float)(_cur_cycle * 512) << endl;
  cout << "Avg cache line util: " << _stats->_stat_cache_line_util / (float)_stats->_stat_dram_cache_accesses << endl;
  cout << "Extra main memory accesses due to slcing: " << _stats->_stat_extra_mem_access_reqd << endl;
  cout << "Total conflicts in CF: " << _stats->_stat_blocked_with_conflicts << endl;
  print_cf_mean_error();
  cout << "Cycles: " << final_cycles << endl;
  cout << "Flush cycles: " << _stats->_cycles_in_flushing_reconfiguration << endl;
  // TODO: with feature length of 128, this is similar to kNN where aggregation phase is too small
  // so probably overlap potential is lower?
  if (_config->_algo == gcn)
  {
    // int matrix_comp = (tot_matrix_mult*FEAT_LEN*FEAT_LEN);
    long matrix_comp = (V * GCN_LAYERS * FEAT_LEN * FEAT_LEN);
    long agg_comp = (E * GCN_LAYERS * FEAT_LEN);
    int ideal_hetro_cycles = std::max(agg_comp, matrix_comp) / (core_cnt * 16 * MAX_DFGS / 2); // 16 is the default throughput of data-parallel cores
    int ideal_homo_cycles = (matrix_comp + agg_comp) / (core_cnt * 16 * MAX_DFGS);
    cout << "Agg comp: " << agg_comp << " mult comp: " << matrix_comp << endl;
    cout << "Ideal hetro cycles: " << ideal_hetro_cycles << " ideal homo cycles: " << ideal_homo_cycles << endl;
  }
  cout << "Boundary cycles: " << _bdary_cycles << endl;
  cout << "Total cycles: " << (final_cycles + (_global_iteration * _graph_vertices * 8 / 512)) << endl;
  _mem_ctrl->print_mem_ctrl_stats();
  cout << "tasks created online: " << _stats->_stat_online_tasks << endl;
  float time = final_cycles / 1; // 1.25; // in ns
  cout << "Time: " << time << " ns" << endl;
  float gteps = _stats->_stat_tot_finished_edges / time;
#if SWARM == 1
  assert(gteps <= core_cnt * LANE_WIDTH && "it should be much lesser than this");
  /*
#if CF_SWARM==1
  assert(gteps<=core_cnt*LANE_WIDTH/FEAT_LEN && "it should be much lesser than this");
#endif
  */
#endif
  cout << "GTEPS: " << gteps << endl;
  cout << "Real GTEPS: " << (float)E / time << endl;
  cout << "Entries cached: " << _stats->_stat_entry_cacheable << endl;
  cout << "Entries uncached: " << _stats->_stat_entry_uncacheable << endl;
  cout << "Cache hits: " << _stats->_stat_cache_hits << endl;
  cout << "Cache hit ratio: " << _stats->_stat_cache_hits / (float)_stats->_stat_tot_finished_edges << endl;
  cout << "Total unused lines (for cache pollution): " << _stats->_stat_tot_unused_line << endl;
  cout << "Edge compulsory miss ratio: " << _stats->_stat_num_compulsory_misses / (float)_stats->_stat_tot_finished_edges << endl;
  cout << "Edge temporal hit ratio: " << _stats->_stat_temporal_hits / (float)_stats->_stat_tot_finished_edges << endl;
  cout << "Cache hit ratio of high degree vertices: " << _stats->_stat_high_deg_hit / (float)_stats->_stat_high_deg_access << endl;
  cout << "Total high degree vertices: " << _stats->_stat_high_deg_access << endl;
  cout << "Cache hit ratio of low degree vertices: " << _stats->_stat_low_deg_hit / (float)_stats->_stat_low_deg_access << endl;
  cout << "Total low degree vertices: " << _stats->_stat_low_deg_access << endl;
  cout << "Cases when edge thing might help: " << _stats->_stat_first_time << endl;
  // cout << "Reorder width: " << _stats->_stat_reorder_width << endl;
  // we will get minimum from dijkstra numbers
  // cout << "Total extra edges: " << _stats->_stat_extra_finished_edges << endl;
  if (_config->_algo == gcn && _config->_reuse == 1)
  {
    assert(_stats->_stat_tot_finished_edges <= (E * GCN_LAYERS) && "should not do more than required edges");
  }
  cout << "Work efficiency: " << _stats->_stat_tot_finished_edges / (double)E << endl;
  cout << "Work efficiency of SCC: " << _stats->_stat_tot_finished_edges / (double)_scratch_ctrl->_edges_visited << endl;
  cout << "Total vertices bypassed: " << _stats->_stat_bypass_count << endl;
  cout << "Total aborted edges: " << _stats->_stat_tot_aborted_edges << endl;
  cout << "Deleted before pushing into network: " << _stats->_stat_deleted_before_net << endl;
  cout << "Aborted due to invalid: " << _stats->_stat_abort_with_invalid << endl;
  cout << "Aborted due to distance: " << _stats->_stat_abort_with_distance << endl;
  cout << "Number of times that it could not find in local dep queue: " << _stats->_stat_not_found_in_local_dep_queue << endl;
  cout << "Total deleted task entry: " << _stats->_stat_deleted_task_entry << endl;
  cout << "Total deleted task entry in agg buffer: " << _stats->_stat_deleted_agg_task_entry << endl;
  cout << "Total aborted edges which were executing: " << _stats->_stat_executing << endl;
  cout << "Total aborted edges not found in task queue: " << _stats->_stat_not_found_in_task_queue << endl;
  cout << "Total abort packets sent on the network: " << _stats->_stat_abort_packets_sent << endl;
  cout << "Number of dependent tasks erased: " << _stats->_stat_dependent_task_erased << endl;
  cout << "Compute throughput: " << (float)_stats->_stat_tot_finished_edges / time << endl;
  cout << "Average edge tasks dequeued: " << (float)_stats->_stat_tot_tasks_dequed / (float)final_cycles << endl;
  cout << "Average memory bandwidth consumed: " << (float)_stats->_stat_tot_tasks_dequed * 2 * sizeof(int) / (mem_bw * final_cycles) << endl;
  cout << "Global barrier count: " << _stats->_stat_barrier_count << endl;
  // cout << "Percentage mis-speculations: " << ((float)_stats->_stat_num_misspeculations/(float)_stats->_stat_tot_finished_edges) << endl;
  cout << "Exploitable parallelism: " << (float)_stats->_stat_tot_vert_tasks / (float)final_cycles << endl;
  cout << "Total network packets: " << _network->_stat_tot_packets_transferred << endl;
  sum = 0;
  cout << "Average packets to each router: ";
  for (int i = 0; i < core_cnt; ++i)
  {
    sum += _network->_stat_packets_to_router[i];
    cout << (double)_network->_stat_packets_to_router[i] << " ";
    // cout << (double)_network>_stat_packets_to_router[i]/(double)_network->_stat_tot_packets_transferred << " ";
  }
  cout << " total packets: " << sum << endl;
  cout << "Local updates: " << _network->_stat_local_updates << endl;
  cout << "Remote updates: " << _network->_stat_remote_updates << endl;
  cout << "Remote/local: " << _network->_stat_remote_updates / (double)_network->_stat_local_updates << endl;
  cout << "Number of stolen tasks: " << _stats->_stat_task_stolen << endl;
  cout << "Network bandwidth utilization: " << _network->_stat_tot_bw_utilization / (double)final_cycles << endl;
  cout << "Network bandwidth consumed: " << _network->_stat_tot_bw_utilization << endl;
  cout << "Compulsory miss rate: " << (float)_stats->_stat_num_compulsory_misses / (float)_stats->_stat_num_accesses << endl;
  cout << "Conflict queue hit rate: " << (float)_stats->_stat_conf_hit / (float)(_stats->_stat_conf_hit + _stats->_stat_conf_miss) << endl;
  cout << "Conflict queue hits: " << _stats->_stat_conf_hit << endl;
  cout << "Conflict queue misses: " << _stats->_stat_conf_miss << endl;
  // they sum this up in each round...
  // cout << "Dynamic load imbalance: " << (float)_stats->_stat_tot_max_load/(float)_stats->_stat_tot_mean_load << endl;
  float avg = 0;
  max = 0;
  cout << "Load per core: ";
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += _stats->_stat_dist_tasks_dequed[i];
    if (_stats->_stat_dist_tasks_dequed[i] > max)
      max = _stats->_stat_dist_tasks_dequed[i];
    cout << _stats->_stat_dist_tasks_dequed[i] << " ";
  }
  cout << "Avg load balance factor: " << max / (float)(avg / (float)core_cnt);
  cout << "Total tuples pushed per core: ";
  for (int i = 0; i < core_cnt; ++i)
  {
    cout << _stats->_stat_avg_reduce_tasks[i] << " ";
  }

  cout << endl;
  finish_simulation();
  int zero_banks = 0;
  cout << "Total requests per bank: ";
  for (int i = 0; i < num_banks; ++i)
  {
    cout << _stats->_stat_num_req_per_bank[i] << " ";
    if (_stats->_stat_num_req_per_bank[i] == 0)
    {
      zero_banks++;
    }
  }
  cout << endl;
  // assert(zero_banks<8 && "there are a lot of banks with 0 access requests, may be performance error");
  /*

  // ofstream edge_stats("roadny-edge");
  ofstream edge_stats("roadny-edge-64");
  // ofstream edge_stats("fla-edge-64");
  // ofstream edge_stats("fb-edge");
  int count_z=0, count_t=0;
  if(edge_stats.is_open()) {
    for(int i=0; i<E; ++i) {
      // edge_stats << _stats->_stat_extra_finished_edges[i] << " " << _neighbor[i].hops << endl;
      // edge_stats << _stats->_stat_extra_finished_edges[i] << endl;
      // want to store src and dist as well
      int out_deg_dest = _offset[_neighbor[i].dst_id+1] - _offset[_neighbor[i].dst_id];
      // We don't know in degree -- how to know?
      edge_stats << _neighbor[i].src_id << " " << _neighbor[i].dst_id << " " << _stats->_stat_extra_finished_edges[i] << " " << out_deg_dest << endl;
      // if(_stats->_stat_extra_finished_edges[i]==0) count_z++;
      // if(_stats->_stat_extra_finished_edges[i]>1) count_t += (_stats->_stat_extra_finished_edges[i]-1);
    }
  }
  edge_stats.close();
  cout << "Edges not even executed once: " << count_z/(double)E << endl;
  cout << "Edges executed extra: " << count_t/(double)E << endl;
  */

  // save_reuse_info();

  /*
  print_sensitive_subgraph();
  */
}

void asic::save_reuse_info()
{
  /*
  ofstream reuse("pr_reuse_info");

  if(reuse.is_open()) {
    for(int i=0; i<E; ++i) {
      float avg_dist = -1;
      if(_stats->_stat_extra_finished_edges[i]!=0) avg_dist = _stats->_stat_reuse[i]/(float)_stats->_stat_extra_finished_edges[i];
        reuse << avg_dist << " " << _first_access_cycle[i] << " " << _stats->_stat_extra_finished_edges[i] << endl;
    }
  }
  reuse.close();
  */
}

void asic::print_sensitive_subgraph()
{
  ofstream subg("roadny-subgraph");
  if (subg.is_open())
  {
    for (int i = 0; i < _graph_edges; ++i)
    {
      if (_stats->_stat_extra_finished_edges[i] > 50)
      {
        // subg << _neighbor[i].src_id << " " << _neighbor[i].dst_id << " " << _vertex_data[_neighbor[i].src_id] << " " << _vertex_data[_neighbor[i].dst_id] << _neighbor[i].wgt << endl;
        subg << _neighbor[i].src_id << " -> " << _neighbor[i].dst_id << "[label=\"" << _neighbor[i].wgt << "\",weight=\"" << _neighbor[i].wgt << "\"];" << endl;
      }
    }
  }
  subg.close();
  /*
    cout << "_graph_verticesariation in edge sensitivity\n";
    for(int i=0; i<_graph_vertices; ++i) {
      if(_num_spec[i]>50) { // in timestamp
        for(int j=0; j<_incoming_edgeid[i].size(); ++j) {
          cout << _stats->_stat_extra_finished_edges[_incoming_edgeid[i][j]] << " ";
        }
      cout << endl;
      }
   }*/
}

bool asic::no_update()
{
  for (int i = 0; i < _graph_vertices; ++i)
  {
    if (_update[i])
      return false;
  }
  return true;
}

// need to adapt to memory controller access
void asic::push_dummy_packet(int src_core, int dest_core, bool two_sided)
{
  if (_config->_network == 0)
  {
    return;
  }
  if (src_core == dest_core)
  {
    _network->_stat_local_updates++;
    return;
  }
  net_packet pref_tuple;
  if (two_sided)
  {
    pref_tuple.req_core_id = src_core;
  }
  else
  {
    pref_tuple.req_core_id = -1;
  }
  pref_tuple.packet_type = dummy;
  pref_tuple.dest_core_id = dest_core;
  // dest_core_id is going to be derived from here
  pref_tuple.dst_id = dest_core; // this would have always been 0
  // for(int i=0; i<64/LINK_BW; ++i) {
  for (int i = 0; i < 1; ++i)
  {
    _network->push_net_packet(src_core, pref_tuple);
  }
}

DTYPE asic::cf_update(int src_id, int dst_id, DTYPE edge_wgt)
{
  bool spawn = false;
  DTYPE sum = 0, lambda = 0; // 0.000001;
  int feat_len = FEAT_LEN;
  for (int i = 0; i < feat_len; ++i)
  {
    // cout << "Before prod: " << _asic->_vertex_data[cur_vid][i] << " src: " << _asic->_vertex_data[cur_tuple.src_id][i] << " ";
    if (_config->is_sync())
    {
      sum += (_vertex_data_vec[i][src_id] * _vertex_data_vec[i][dst_id]);
    }
    if (_config->is_async() || _config->_blocked_async)
    {
      sum += (_scratch_vec[i][src_id] * _scratch_vec[i][dst_id]);
    }
  }
  // cout << "src_id: " << cur_tuple.src_id << " dst_id: " << cur_tuple.edge.dst_id << " dot_prod: " << sum << " edge_wgt: " << cur_tuple.edge.wgt << endl;
  sum = edge_wgt - sum;
  DTYPE error = 0;

  for (int i = 0; i < feat_len; ++i)
  {
    if (_config->is_sync())
    {
      DTYPE temp_f = (sum * _vertex_data_vec[i][src_id]) - (lambda * _vertex_data_vec[i][dst_id]);
      _scratch_vec[i][dst_id] += temp_f;
    }
    else
    {
      DTYPE temp_f = (sum * _scratch_vec[i][src_id]) - (lambda * _scratch_vec[i][dst_id]);
      temp_f *= _gama;
      _scratch_vec[i][dst_id] += temp_f;
      error += abs(temp_f);
    }
  }
  return error;
}

int asic::get_update_operation_latency()
{
  if (!_config->is_vector())
  {
    return 1;
  }
  else if (MAX_DFGS > 1)
  {
    return DFG_LENGTH;
  }
  else
  {
    return DFG_LENGTH;
  }
  return 0;
}

int asic::get_update_operation_throughput(int src_id, int dst_id)
{
  if (!_config->is_vector())
  {
    return 1;
  }
  else
  {
    if (_config->_algo == cf || _config->_algo == gcn)
    {
      return bus_width / VEC_LEN; // this is limit??
    }
    else
    { // triangle counting
      int num_iters = 0;
      // We really need aborting because high variance in edge list sizes.
      // Starting two very coarse-grained conditions do not help.
      if (_offset[src_id + 1] > 0 && (_neighbor[_offset[src_id + 1] - 1].dst_id < _neighbor[_offset[dst_id]].dst_id))
      {
        num_iters = 1;
      }
      else if (_offset[dst_id + 1] > 0 && (_neighbor[_offset[dst_id + 1] - 1].dst_id < _neighbor[_offset[src_id]].dst_id))
      {
        num_iters = 1;
      }
      else
      {
        // TODO: could do binary search here if we still want to save work.
        int index1 = _offset[src_id];
        int index2 = _offset[dst_id];
        // while(index1<_offset[src_id+1] && _neighbor[index1].dst_id<=src_id) {
        //   ++index1;
        // }
        // while(index2<_offset[dst_id+1] && _neighbor[index2].dst_id<=dst_id) {
        //   ++index2;
        // }
        // slight difference when we check online because it is really finding
        // when both index1 and index2 are past max(src_id, dst_id) because
        // otherwise it is not relevant...
        while (index1 < _offset[src_id + 1] && index2 < _offset[dst_id + 1])
        {
          // it should only start counting when both index1 and index2 are
          // greater than their vids to save replication
          // assert(_neighbor[index1].dst_id>src_id && _neighbor[index2].dst_id>dst_id);
          // ++num_iters;
          bool allowed = (_neighbor[index1].dst_id > src_id && _neighbor[index2].dst_id > dst_id);
          if (allowed)
          {
            ++num_iters;
          }
          if (_neighbor[index1].dst_id == _neighbor[index2].dst_id)
          {
            ++index1;
            ++index2;
            if (allowed)
            {
              ++_stats->_stat_tot_triangles_found;
            }
          }
          else if (_neighbor[index1].dst_id < _neighbor[index2].dst_id)
          {
            ++index1;
          }
          else
          {
            ++index2;
          }
        }
      }
      if (MAX_DFGS > 1)
      {
        // cout << "edges in source: " << (_offset[src_id+1]-_offset[src_id]) << " edges in destination: " << (_offset[dst_id+1]-_offset[dst_id]) << " num iters in triangle counting: " << num_iters << endl;
        // " vector length per multiplication: " << VEC_LEN << endl;
        return num_iters;
      }
      else
      {
        return std::ceil(num_iters / (float)VEC_LEN);
      }
    }
  }
  return 0;
}

// SSSP for graphmat
// analogy (dist==residual) vertex data which defines convergence , PR extra
// vertex data to maintained
DTYPE asic::process(DTYPE edge_wgt, DTYPE dist)
{ // (edge_wgt, dist)
  if (_config->_algo == cf)
  {
    assert(0 && "should not call scalar process for cf/gcn");
    return dist;
  }
  if (_config->_algo == pr)
  {
    assert(dist >= 0 && " why is source distance in process less than 0");
    // cout << "dist incoming: " << dist << " alpha: " << alpha << " edge wgt: " << edge_wgt << endl;
    DTYPE x = dist * alpha * edge_wgt;
    /*if(isnan(x)) {
      cout << "returning 0, wgt: " << a << " src residual: " << b << endl;
    }*/
    // cout << "New value sent: " << x << endl;
    // assert(x>=0);
    /*if(x==0) {
      cout << "returning 0, wgt: " << a << " src residual: " << b << endl;
    }*/
    return x;
  }
  else
  { // same for bfs,sssp,astar
    if(_config->_algo==cc) {
      return dist;
    } else {
      return edge_wgt + dist;
    }
  }
}

DTYPE asic::reduce(DTYPE old_value, DTYPE new_value)
{
  if (_config->_algo == cf)
  {
    assert(0 && "should not call scalar reduce for cf/gcn");
  }
#if PR == 1
  // assert((a+b)>=0);
  // cout << "Final updated value: " << a << " " << b << " " << (a+b) << endl;
  return old_value + new_value;
#else
  return min(old_value, new_value);
#endif
}

DTYPE asic::apply(DTYPE new_dist, int updated_vid)
{
  if (_config->_algo == cf)
  {
    assert(0 && "should not call scalar process for cf/gcn");
  }
  if (_config->_algo == pr)
  {
    // return 1/a;
    if (new_dist == 0)
    {
      return 10000; // TODO: it should not come here -- fix!!!
    }
    int degree = _offset[updated_vid + 1] - _offset[updated_vid];
    DTYPE x = degree / (DTYPE)new_dist;
    assert(x >= 0);
    return x;
  }
  else
  {
#if ABCD == 0
    return new_dist;
#else
    return abs(new_dist - _scratch[updated_vid]);
#endif
  }
}

bool asic::should_break_in_pull(int parent_id)
{
  return false;
  // if n was active during the synchronization...
  if (_graph_vertices == 3598623)
  {
    return false;
  }
  if (_config->is_async())
  {
    return false;
  }
  else
  {
    return (_scratch_ctrl->_is_visited[parent_id]);
  }
}

bool asic::should_spawn_task(DTYPE old_dist, DTYPE new_dist)
{
  if (_config->_algo == cf)
  {
    assert(0 && "should not call scalar process for cf/gcn");
  }
  if (_config->_algo == pr)
  {
    // cout << "old residual: " << old_dist << " new residual: " << new_dist << endl;
    assert(old_dist >= 0 && new_dist >= 0);
    bool should_spawn = new_dist >= epsilon && old_dist < epsilon;
    if (_config->is_sync() || _config->_blocked_async)
    {
      should_spawn = new_dist >= epsilon;
    }
    if (_config->_pull_mode)
    {
      should_spawn = abs(old_dist - new_dist) >= epsilon;
    }
    // cout << "new dist: " << new_dist << " old dist: " << old_dist << " epsilon: " << epsilon <<" should spawn: " << should_spawn << endl;
    return should_spawn;
    // return new_dist>epsilon;
    // assert(b>=a); // monotonicity property
    // wait, is residual monotonic? it is increasing monotonically
    // return ((b<a) && (b>epsilon)); // residual should decrease
    // return a<epsilon;
    // return b>epsilon;
    // return ((b>a) && (b>epsilon));
  }
  else
  {
    return (new_dist < old_dist);
  }
}

bool asic::should_abort_first(DTYPE a, DTYPE b)
{
#if PR == 1
  return a <= b;
#else
  return a >= b;
#endif
}

/* Observations:
 * Result mapping heavily depends on what algorithm is followed (FIFO, ABORT)
 * It works when reuse depends on the correlation between vertices
 * (but if it depends on the order and timing, this may be hard or the pattern is still same?)
 * When eviction is done in round-robin manner, load balance is pretty high while locality is little poor
 * too much stealing hurts!!! (not required in uniform graphs)
  // slightly changing thief count had about 30\% impact
  // with perfect network, load balance is also ideal (as being closer to the static case) -- these are actually correlated
*/
void asic::simulate_ideal()
{
  // push initial tasks
  task_entry cur_task(_src_vid, _offset[_src_vid]);
  _task_ctrl->insert_new_task(0, 0, 0, 0, cur_task);

  // initialize mapping of each vertex to a core
  for (int i = 0; i < _graph_vertices; ++i)
    _map_vertex_to_core[i] = -1;
  for (int i = 0; i < core_cnt; ++i)
    _stats->_stat_dist_tasks_dequed[i] = 0;
  for (int i = 0; i < core_cnt; ++i)
    _stats->_stat_dist_edges_dequed[i] = 0;

  // _slice_size = _graph_vertices/core_cnt;
  // This ceil is not working?
  _scratch_ctrl->_slice_size = ceil((float)_stats->_stat_vertices_done / (float)core_cnt);
  _scratch_ctrl->_slice_size += 1;

  // call ideal for core each cycle
  bool term_flag = true;
  int tot_vertices_mapped = 0;
  float lbfactor = 0;
  while (_cur_cycle < _max_iter)
  {

    if (_cur_cycle % 10000 == 0)
    {
      cout << "Cycles done during profiling: " << _cur_cycle
           << " total vertices mapped: " << tot_vertices_mapped << endl;
    }

    _cur_cycle++;
    term_flag = true;
    tot_vertices_mapped = 0;

    float edges_served = 0;
    int max_edges_served = 0;
    for (int c = 0; c < core_cnt; ++c)
    {
      _asic_cores[c]->automatic_partition_profile();
      if (!_asic_cores[c]->pipeline_inactive(false))
        term_flag = false;
      tot_vertices_mapped += _stats->_stat_dist_tasks_dequed[c];

      // sum the edges served
      edges_served += _stats->_stat_fine_gran_lb_factor[c];
      if (_stats->_stat_fine_gran_lb_factor[c] > max_edges_served)
      {
        max_edges_served = _stats->_stat_fine_gran_lb_factor[c];
      }

      /*if(tot_vertices_mapped==54512) {
        cout << "Core: " << c << endl;
        _asic_cores[c]->pipeline_inactive(true);
      }*/
    }
    lbfactor += max_edges_served / (float)(edges_served / (float)core_cnt);
    // FIXME: actually this should be the vertices visited in SSSP
    // if(tot_vertices_mapped==_stats->_stat_vertices_done) term_flag=true;
    if (term_flag)
      break;
    /*if(_cur_cycle%10000==0) {
      cout << "vertices mapped: " << tot_vertices_mapped << " reqd: " << _stats->_stat_vertices_done << endl;
    }*/
  }
  // TODO: I need to add this functionality to the real simulation (no need here)
  // this could probably be considered as work-stealing is doing its job right
  cout << "Fine-grained LB factor: " << lbfactor / (float)_cur_cycle << endl;
  if (tot_vertices_mapped != _stats->_stat_vertices_done)
  {
    cout << "total vertices mapped: " << tot_vertices_mapped << " vertices done: " << _stats->_stat_vertices_done << endl;
  }

  // assert(tot_vertices_mapped==_stats->_stat_vertices_done && "all vertices should be mapped");
  if (_cur_cycle == _max_iter)
  {
    cout << "Didn't complete; vertices mapped: " << tot_vertices_mapped << " total vertices done: " << _stats->_stat_vertices_done << "\n";
    return;
  }
  else
    cout << "Program done in cycles: " << _cur_cycle << endl;

  int degree_per_core[core_cnt];
  for (int i = 0; i < core_cnt; ++i)
    degree_per_core[i] = 0;

  for (int i = 0; i < core_cnt; ++i)
  {
    // cout << "Vertices allotted to this core: " << _stats->_stat_dist_tasks_dequed[i] << endl;
    _stats->_stat_dist_tasks_dequed[i] = 0;
  }

  // I should print the partitioning here...
  // Locality test:
  // 82% edges are remote (what is the average links traversed?)
  int tot_links_traversed = 0;
  int remote = 0, local = 0;
  for (int v = 0; v < _graph_vertices; ++v)
  {
    int src_core = _map_vertex_to_core[v];
    for (int e = _offset[v]; e < _offset[v + 1]; ++e)
    {
      int dst_core = _map_vertex_to_core[_neighbor[e].dst_id];
      degree_per_core[dst_core]++;
      if (src_core != dst_core)
      {
        tot_links_traversed += abs(src_core - dst_core); // _network->get_dist(src_core, dst_core);
        remote++;
      }
      // else local++;
    }
  }

  cout << "Absolute remote/local accesses: " << remote / (float)E << endl;
  cout << "Average links traversed: " << tot_links_traversed / (float)E << endl;
  // Note: this could be better in practical scenario because there is irregular reuse on different edges
  // based on their weight

  // In my opinion, incoming freq is the affinity of the task to this core.
  int cur_max_freq = -1, expected_core = -1;
  int correct_mapping = 0, wrong_mapping = 0;
  int sum_affinity = 0; // should be equal to the edges executed...
  // FIXME: hint, max is many times double of the mapped (What does that mean?)
  // may be it got double by just the edge freq...
  // like the vertex was sitting idle, not evicted because of its high affinity
  // but the parent got evicted, its affinity is directly reduced without it doing anything
  // its affiity to the new core should be increased...
  for (int i = 0; i < _graph_vertices; ++i)
  {
    if (_map_vertex_to_core[i] == -1)
      continue;
    cur_max_freq = 0;
    expected_core = -1;
    for (int c = 0; c < core_cnt; ++c)
    {
      sum_affinity += _inc_freq[c][i];
      if (_inc_freq[c][i] > cur_max_freq)
      {
        cur_max_freq = _inc_freq[c][i];
        expected_core = c;
      }
    }
    assert(expected_core != -1 && "to be mapped, it should have freq to be at least 1");
    int mapped_core = _map_vertex_to_core[i];
    if (expected_core != mapped_core && _inc_freq[expected_core][i] != _inc_freq[mapped_core][i])
    {
      // cout << "Max freq: " << cur_max_freq << " mapped freq: " << _inc_freq[mapped_core][i] << endl;
      wrong_mapping++;
    }
    else
      correct_mapping++;
  }

  cout << "Wrong mapping ratio: " << wrong_mapping / (float)(wrong_mapping + correct_mapping) << endl;
  // This is not a exact match, maybe because of time differences -- I can debug..
  if (sum_affinity != _stats->_stat_tot_finished_edges)
  {
    cout << "Affinity: " << sum_affinity << " and finished edges: " << _stats->_stat_tot_finished_edges << endl;
  }
  // assert(sum_affinity==_stats->_stat_tot_finished_edges && "affinity is associated with dynamic number of dependencies associated");

  // FIXME: make sure we identified critical edges correctly
  // averaged out by weight...

  // Load balance test: (total incoming edges at each core)
  float avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }
  cout << " LB factor: " << max / (float)(avg / (float)core_cnt) << endl;
  cout << "Work-efficiency during profiling: " << _stats->_stat_tot_finished_edges / (float)E << endl;
  cout << "Total tasks stolen: " << _work_steal_depth / (float)_stats->_stat_tot_tasks_dequed << endl;
  cout << "Ping-pong vertices due to overfill (no place for new vertex): " << _stats->_stat_ping_pong_vertices / (float)_graph_vertices << endl;
  cout << "Ping-pong vertices due to shared (new vertex was already mapped to a lower freq core): " << _stats->_stat_low_freq_vertices / (float)_graph_vertices << endl;

  // map_grp_to_core();

  // I want to compare edge-freq
  ofstream initial_freq("edge-freq-ideal");
  if (initial_freq.is_open())
  {
    for (int e = 0; e < _graph_edges; ++e)
    {
      initial_freq << "e: " << e << " freq: " << _edge_freq[e] << endl;
      _edge_freq[e] = 0;
    }
  }
  initial_freq.close();

  // TODO: actually I should store this mapping in a file to avoid calculating
  // multiple times

  // Step 1: Check how much the factors calculated above correlated with the original execution
  // Step 2: test for different vertices
  _stats->_stat_tot_finished_edges = 0;
  _stats->_stat_tot_tasks_dequed = 0;
  _cur_cycle = 0;
  init_vertex_data();
  _src_vid = _graph_vertices / 4; // To test if it works for the other case...

  /*for(int i=0; i<_graph_vertices; ++i) {
    _map_vertex_to_core[i] = i%core_cnt;
  }*/

#if SGU_SLICING == 1
  simulate_sgu_slice();
#else
  simulate();
#endif
  ofstream prac_freq("edge-freq-prac");
  if (prac_freq.is_open())
  {
    for (int e = 0; e < _graph_edges; ++e)
    {
      prac_freq << "e: " << e << " freq: " << _edge_freq[e] << endl;
    }
  }
  prac_freq.close();
}

// Let's test by allotting multiple tasks to each core and see how many cycles
// it takes -- how close is it to my calculation using gem5 (actually it was
// compute bound)
void asic::test_mult_task()
{
  cout << "Came in to test the multiplication task\n";
  // push mult task
  for (int vid = 0; vid < FEAT_LEN; ++vid)
  {
    _task_ctrl->insert_coarse_grained_task(vid);
  }
  cout << "Pushed a dummy multiplication task\n";
  int max_iter = 100000;
  while (_cur_cycle < max_iter)
  {
    ++_cur_cycle;
    _mem_ctrl->_mem->update();
    bool term_flag = true;
    for (int c = 0; c < _config->_core_cnt; ++c)
    {
      if (_mult_cores[c]->cycle(0))
      { // continue if any core was working
        term_flag = false;
      }
    }

    if (term_flag)
      break;
  }
  if (_cur_cycle < max_iter)
  {
    cout << "Completed computation at cycle: " << _cur_cycle << endl;
  }
  else
  {
    cout << "Infinite simulation" << endl;
    for (int c = 0; c < _config->_core_cnt; ++c)
    {
      _mult_cores[c]->pipeline_inactive(false);
    }
  }
  exit(0);
}

int main()
{
#if GCN == 1
  assert(DFG_LENGTH >= (FEAT_LEN1 / VEC_LEN) && "pipeline latency cannot be less");
  assert(bus_width == VEC_LEN && "currently we do not complete those");
  // assert(MINIBATCH_SIZE<=_graph_vertices);
#endif
  assert(num_rows * num_rows == core_cnt && "currently we only support mesh networks");
  asic *model = new asic();

  // TESTING A NEW ALGORITHM
  if (model->_config->_hash_join == 1)
  {
    default_random_engine generator;
    // 1kB = 16 cache lines; 1 MB => 16,384 cache lines
    normal_distribution<double> distribution(100000.0, 10000.0);
    const int MAX_RANGE = 6096; // 5120; // 16384; // limiting the footprint
    bitset<MAX_RANGE> is_miss_avail;
    int access_count[MAX_RANGE], accesses_left = 0;
    int prev_access_index = 0;
    if (model->_config->_hats == 1)
    {
      for (int i = 0; i < MAX_RANGE; ++i)
      {
        access_count[i] = 0;
      }
    }
    is_miss_avail.reset();

    bool term_flag = false;
    const int unique_cache_lines_per_slice = (L2SIZE * 1024 / line_size);
    while (model->_cur_cycle < model->_max_iter)
    {
      int hits_this_cycle = 0;
      // TODO 1: Generating memory requests every cycle
      pref_tuple cur_tuple;
      if (model->_cur_cycle < 10000)
      { // 1600k $-lines out of 16k => Reuse=100 times
        // model->generate_memory_accesses(generator, distribution);
        for (int c = 0; c < core_cnt; ++c)
        {
          uint64_t paddr = distribution(generator); // %1000; // want to skewed for natural data
          if (model->_cur_cycle < 1000)
          {
            paddr = paddr % 4096;
          }
          else if (model->_cur_cycle < 1250)
          {
            paddr = paddr % 6096;
          }
          else if (model->_cur_cycle < 1750)
          {
            paddr = paddr % 4096;
          }
          else
          {
            paddr = 4096 + paddr % 2000;
          }
          // TODO: for HATS, go and increment accesses to this element by 1 in the array

          paddr *= line_size;
          // cout << "generated address: " << paddr; // << endl; // << endl;
          // bool l2hit = model->_mem_ctrl->is_cache_hit(c, paddr, ACCESS_LOAD, true, 0);
          bool l2hit = model->_mem_ctrl->_l2_cache->LookupCache(paddr, ACCESS_LOAD, true, 0); // check cache statistics
          if (model->_config->_hats == 1)
          {
            int index = paddr / line_size;
            ++access_count[index];
            ++accesses_left;
            l2hit = false;
          }
          if (l2hit)
          {
            // cout << " hits at cycle: " << model->_cur_cycle << endl;
            model->_mem_ctrl->_l2hits++;
            ++hits_this_cycle;
            // #if CACHE_HIT_AWARE_SCHED==1
            //            model->_hit_addresses.push_back(paddr);
            // #endif
          }
          else
          {
            // _asic_cores[c]->_miss_gcn_updates.push_back(cur_tuple);
#if CACHE_HIT_AWARE_SCHED == 1
            // cout << " miss pushed in the queue: " << model->_cur_cycle; // << endl;
            if (is_miss_avail.test(paddr / line_size) == 1)
            {
              ++model->_mem_ctrl->_l2hits;
              ++model->_mem_ctrl->_l2coalesces;
            }
            else
            {
              model->_miss_addresses.push(paddr);
              is_miss_avail.set(paddr / line_size);
            }
#elif HATS == 0
            bool sent_request = model->_mem_ctrl->send_mem_request(false, paddr, cur_tuple, VERTEX_SID);
            if (!sent_request)
            {
              // cout << " coalesced\n";
              model->_mem_ctrl->_l2hits++;
              model->_mem_ctrl->_l2coalesces++;
            }
            else
            {
              // cout << " miss\n";
            }
            model->_stats->_stat_pending_cache_misses++;
            model->_mem_ctrl->_num_pending_cache_reqs++;
#endif
          }
          model->_mem_ctrl->_l2accesses++;
        }
      }

#if AGG_HATS == 1
      int count = 0;
      // cout << "Size of miss queue: " << model->_miss_addresses.size() << endl;
      // I did not give time to accumulate
      while (model->_cur_cycle > 100 && accesses_left > 0 && (count < core_cnt))
      {
        while (accesses_left > 0 && access_count[prev_access_index] == 0)
        {
          prev_access_index = (prev_access_index + 1) % MAX_RANGE;
        }
        --accesses_left;
        ++count;
        uint64_t paddr = prev_access_index;
        paddr *= line_size;
        --access_count[prev_access_index];
        // cout << "index served: " << prev_access_index << " access count left: " << access_count[prev_access_index] << endl;

        bool l2hit = model->_mem_ctrl->_l2_cache->LookupCache(paddr, ACCESS_LOAD, true, 0); // check cache statistics
        if (l2hit)
        {
          model->_mem_ctrl->_l2hits++;
        }
        else
        {
          bool sent_request = model->_mem_ctrl->send_mem_request(false, paddr, cur_tuple, VERTEX_SID);
          if (!sent_request)
          {
            // cout << " coalesced\n";
            model->_mem_ctrl->_l2hits++;
            model->_mem_ctrl->_l2coalesces++;
          }
          model->_stats->_stat_pending_cache_misses++;
          model->_mem_ctrl->_num_pending_cache_reqs++;
        }
      }
#endif

      // Step 2: If we want, serve from here
#if CACHE_HIT_AWARE_SCHED == 1
      int count = 0;
      // cout << "Size of miss queue: " << model->_miss_addresses.size() << endl;
      while ((model->_unique_mem_reqs_this_phase < unique_cache_lines_per_slice) && (count < core_cnt) && (!model->_miss_addresses.empty()))
      {
        ++count;
        // TODO: this could also be ah hit now!!
        uint64_t paddr = model->_miss_addresses.front();
        model->_miss_addresses.pop();
        is_miss_avail.reset(paddr / line_size);
        cout << "popped address: " << paddr;
        bool sent_request = model->_mem_ctrl->send_mem_request(false, paddr, cur_tuple, VERTEX_SID);
        if (!sent_request)
        {
          cout << " coalesced\n";
          model->_mem_ctrl->_l2hits++;
          model->_mem_ctrl->_l2coalesces++;
        }
        else
        {
          ++model->_unique_mem_reqs_this_phase;
          cout << " miss\n";
          cout << "Cycle: " << model->_cur_cycle << " unique memory reqs: " << model->_unique_mem_reqs_this_phase << " with unique lines: " << unique_cache_lines_per_slice << endl;
          if (model->_unique_mem_reqs_this_phase > unique_cache_lines_per_slice)
          {
            cout << "Confusing point; with unique lines: " << unique_cache_lines_per_slice << endl;
          }
        }
        assert(model->_unique_mem_reqs_this_phase <= unique_cache_lines_per_slice && "cannot access more than max cache lines per slice");
        model->_stats->_stat_pending_cache_misses++;
        model->_mem_ctrl->_num_pending_cache_reqs++;
      }
      ++model->_cycles_this_phase;

      // TODO: want to reset phase when new hits are not present...
      // hits or coalesced is 0...
      // Okay misses are sufficient but this condition is never met...
      if (hits_this_cycle == 0 && model->_cycles_this_phase > 100 && (model->_unique_mem_reqs_this_phase > (0.9 * unique_cache_lines_per_slice)))
      { // this is the reuse distance..
        cout << "Cycles in this above phase: " << model->_cycles_this_phase << " with number of misses served: " << model->_unique_mem_reqs_this_phase << endl;
        // TODO: flush the cache so that it can be filled again (or it will switch by itself?)
        model->_unique_mem_reqs_this_phase = 0;
        model->_cycles_this_phase = 0;
      }
#endif

      term_flag = true;
      for (int c = 0; c < core_cnt; ++c)
      {
        if (!model->_asic_cores[c]->pipeline_inactive(false))
          term_flag = false;
      }
      if (!model->_hit_addresses.empty())
        term_flag = false;
      if (!model->_miss_addresses.empty())
        term_flag = false;
      if (model->_cur_cycle > 0 && model->_cur_cycle % 10000 == 0)
      {
        cout << "Cur Cycle: " << model->_cur_cycle << " memory requests: " << model->_stats->_stat_dram_cache_accesses << endl;
        // exit(0);
      }
      model->_mem_ctrl->_mem->update();
      if (model->_cur_cycle < 10000)
        term_flag = false;
      if (term_flag)
        break;
      ++model->_cur_cycle;
    }
    cout << "Cycles: " << model->_cur_cycle << endl;
    cout << "L2 size: " << L2SIZE << " hit rate: " << model->_mem_ctrl->_l2hits / (double)model->_mem_ctrl->_l2accesses << endl;
    cout << "L2 coalesces: " << model->_mem_ctrl->_l2coalesces << " l2 hits only: " << (model->_mem_ctrl->_l2hits - model->_mem_ctrl->_l2coalesces) << " l2 accesses: " << model->_mem_ctrl->_l2accesses << endl;
    return 0;
  }

  // TESTING A NEW TASK TYPE
  // model->test_mult_task();

#if PULL == 0
  // string file_str(csr_file);
#if DEFAULT_DATASET
  string file_str = default_dataset_path + string(csr_file);
#else
  string file_str = string(csr_file);
#endif // DEFAULT_DATASET
  model->read_graph_structure(file_str, model->_offset, model->_neighbor, true, false);
#else
  // string file_str(csc_file);
#if DEFAULT_DATASET
  string file_str = default_dataset_path + string(csc_file);
#else
  string file_str = string(csc_file);
#endif // DEFAULT_DATASET
  model->read_graph_structure(file_str, model->_offset, model->_neighbor, true, true);
  string file_csr(csr_file);
  model->read_graph_structure(file_csr, model->_csr_offset, model->_csr_neighbor, false, false);
#endif
#if PROFILING == 1
  int edges_traversed = 0;
  while (edges_traversed < (E / 2))
  {
    model->_src_vid = rand() % _graph_vertices;
    model->init_vertex_data();
    edges_traversed = model->fill_correct_vertex_data();
  }
  cout << "Selected src id: " << model->_src_vid << endl;
  model->init_vertex_data();
  // model->fill_correct_vertex_data();
  for (int delay = 16; delay < 17; ++delay)
  {
    // for(int e=1; e<16; ++e) {
    for (int e = 16; e > 1; --e)
    {
      model->init_vertex_data();
      model->scalability_test(pow(2, e), delay);
    }
  }

  // model->simulate_ideal();
  // exit(0);
#endif
  // model->perform_linear_load_balancing();
  // model->perform_modulo_load_balancing();
  // model->perform_load_balancing();
  // model->generate_metis_format();
  // exit(0);
#if SGU_SLICING == 1 || SGU == 1
  // TODO: both use slice count for this, but I should probably allow <= granularity
  model->_scratch_ctrl->read_metis_slicing();       // just fill in vids that belong to a slice
  model->_scratch_ctrl->get_slice_boundary_nodes(); // cacluated basic entries + update graph
#if ANAL_MODE == 1
  exit(0);
#endif
#if METIS == 1 // this is local metis...
  model->_slice_count = 4;
  model->_slice_size = _graph_vertices / 4;
#endif
  // model->get_sync_boundary_nodes();
#endif
#if GRAPHMAT == 1 || BLOCKED_ASYNC == 1
  model->_scratch_ctrl->read_metis_slicing();
  model->_scratch_ctrl->get_slice_boundary_nodes(); // also update graph
  model->_scratch_ctrl->get_sync_boundary_nodes();
#endif

#if LADIES_GCN == 1
  model->ladies_sampling();
  model->common_sampled_nodes();
#endif
  // model->write_csr_file();
  // exit(0);
#if SGU_SLICING == 1
  // model->slice_init();
  // model->BFS(0);
  // model->read_mongoose_slicing();
  // model->get_slice_boundary_nodes();
#endif
  // model->read_mapping();

#if BFS_MAP == 1
  model->_scratch_ctrl->BFS(0);
  int max = 0;
  /*for(int i=0; i<model->_stats->_stat_num_clusters; ++i) {
    // cout << "Elem done: " << model->_cluster_elem_done[i] << endl;
    if(model->_cluster_elem_done[i]>max) {
      max = model->_cluster_elem_done[i];
    }
  }*/
  cout << "BFS on this graph is done with number of clusters: " << model->_stats->_stat_num_clusters << "\n";
  cout << "maximum size of the cluster is: " << max << endl;
#elif DFSMAP == 1
  // model->DFS(0);
  int start_vert = 0; // = rand()%_graph_vertices;
  // cout << "Doing dfs with the start vertex: " << start_vert << endl;
  if (_graph_vertices == 2394385)
  { // wiki
    start_vert = 2;
  }

  // run DFS in a loop until vertices visited are greater than minibatch size
#if GCN == 1
  while (model->_scratch_ctrl->_vertices_visited < MINIBATCH_SIZE)
  {
    model->_scratch_ctrl->DFS(start_vert, 0);
    for (int i = 0; i < model->_graph_vertices; ++i)
    {
      if (!model->_visited[i])
      {
        start_vert = i;
        continue;
      }
    }
  }
#else
  model->DFS(start_vert, 0);
  // set all 0 degree vertices
  // if(_graph_vertices==820878 || _graph_vertices==4847571) {
#if PR == 1
  if (1)
  { // _graph_vertices==820878 || _graph_vertices==4847571) {
    for (int i = 0; i < _graph_vertices; ++i)
    { // this is too much for some graphs
      if (!model->_visited[i])
      {
        // if(model->_offset[i]==model->_offset[i+1]) {
        if (1)
        { // model->_offset[i]==model->_offset[i+1]) {
          model->_vertices_visited++;
          model->_visited[i] = true;
          model->_mapping[i] = ++model->_global_ind;
        }
      }
    }
  }
#endif
  /*while(model->_vertices_visited<_graph_vertices) {
    for(int i=0; i<_graph_vertices; ++i) { // this is too much for some graphs
      if(!model->_visited[i]) {
        start_vert=i;
        continue;
      }
    }
    model->DFS(start_vert);
  }*/
#endif
  // model->BDFS(0);
  // model->sens_aware_part();
#else
  model->_scratch_ctrl->linear_mapping();
#endif

  // Work on the new graph and only on a given slice: so we know the vertex
  // ranges...
#if SGU == 1 && SGU_SLICING == 0
  model->_slice_count = 1;
#endif

  model->_scratch_ctrl->perform_spatial_part();

  if (model->_config->_algo == gcn)
  {
    model->_scratch_ctrl->fix_in_degree();
    model->init_vertex_data();
  }

  // model->load_balance_test();

  // #if PR==0 && ACC==0
  if (model->_config->_algo == sssp || model->_config->_algo == bfs || model->_config->_algo == cc)
  {
    model->fill_correct_vertex_data();
  }
  cout << "Average hops is: " << model->_scratch_ctrl->avg_remote_latency() << endl;
  // exit(0);

  /*for(int c=0; c<core_cnt; ++c) {
    cout << "mapping of core c: " << c << " is: " << model->_scratch_ctrl->get_hilbert_core(c) << endl;
  }*/

  // exit(0);
  model->call_simulate();

#if HYBRID_EVAL == 1
  int max_switches = model->_global_iteration;
  int stride = 1; // ceil(max_switches/(float)5);
  int max_cache = model->_cur_cycle / 100000;
  int cache_stride = ceil(max_cache / (float)4);
  cout << "Complete computation with total iterations: " << max_switches << " and stride: " << stride << endl;
  cout << "And total cycle ranges: " << max_cache << " and stride: " << cache_stride << endl;
  for (int tip = stride; tip < max_switches; tip += stride)
  {
    int cache_tip = tip;
    // for(int cache_tip=tip; cache_tip<max(tip+max_cache,tip+1); cache_tip+=cache_stride) {
    // this means that it should switch to async/cache at this point (meaning
    // corresponding flags are set, states are restored, ds do not need to be
    // done, and the execution is continued)
    model->_async_tipping_point = tip;
    model->_cache_tipping_point = cache_tip;
    model->reset_algo_switching();

    model->init_vertex_data();
    cout << "Starting computation with new async tipping point: " << tip << " and cache tipping point: " << cache_tip << endl;
    model->call_simulate();
    assert((model->_config->_exec_model == sync_slicing || model->_config->_exec_model == async_slicing) && "Unimplemented hybrid mode\n");
    max_cache = model->_cur_cycle / 100000;
    cout << "Old cache tip: " << cache_tip << " and the max: " << max(tip + max_cache, tip + 1) << endl;
    stride *= 2;
    /*if(stride>0) {
          stride *= 2;
      } else {
          stride=4;
      }*/
    // }
  }
  // cout << "At completion, stride: " << stride << " max switch: " << max_switches << " tip: " << tip << " cache tip: " << cache_tip << endl;
#endif

  return 0;
}

// TODO: I am not sure what this means
void asic::push_pending_task(DTYPE priority, task_entry cur_task)
{
  if (_config->_exec_model == async_slicing || _config->_exec_model == sync_slicing)
  {
    int wklist = _scratch_ctrl->get_slice_num(cur_task.vid);
    _task_ctrl->push_task_into_worklist(wklist, priority, cur_task);
  }
  else
  {
    _task_ctrl->insert_local_task(0, 0, _scratch_ctrl->get_local_scratch_id(cur_task.vid), priority, cur_task);
  }
}

void asic::print_cf_mean_error()
{
  if (_config->_algo != cf)
    return;
  DTYPE mean_error = 0;
  for (int e = 0; e < E; ++e)
  {
    edge_info edge = _neighbor[e];
    DTYPE sum = 0;
    for (int f = 0; f < FEAT_LEN; ++f)
    {
      if (_config->is_async())
      {
        sum += (_scratch_vec[f][edge.src_id] * _scratch_vec[f][edge.dst_id]);
      }
      else
      {
        sum += (_vertex_data_vec[f][edge.src_id] * _vertex_data_vec[f][edge.dst_id]);
      }
    }
    mean_error += pow((edge.wgt - sum), 2);
  }
  mean_error /= (float)E;
  if (mean_error > _pre_error)
  {
    _gama /= 2;
  }
  cout << "Mean error: " << mean_error << " and pre error: " << _pre_error << endl;
  _pre_error = mean_error;
}

// pass over all slices and at the end activate the correct task and update
// vertex data (only place when vertex data is used)
// TODO: it should assume all cross-slice vertices are misses? or maybe no need
void asic::simulate_blocked_async_slice()
{
  slice_init();
  while (!_task_ctrl->worklists_empty())
  {
    _global_iteration++;
    _start_active_vertices = _task_ctrl->tot_pending_tasks();

    _start_copy_vertex_update = _stats->_stat_tot_num_copy_updates;
    _stats->_stat_tot_num_copy_updates = 0;
    print_local_iteration_stats(0, 0);

    int slice_id = -1;
    slice_id = _task_ctrl->chose_new_slice(slice_id);

    int served = 0;
    while (served < _slice_count)
    {
      served++;

      _local_iteration++;
      cout << "Starting with slice: " << slice_id << " and global iteration: " << _global_iteration << endl;
      // print_local_iteration_stats(slice_id, 0);
      _current_slice = slice_id;
      cout << "Size of worklist in this round: " << _task_ctrl->_worklists[slice_id].size() << endl;

      if (_task_ctrl->_worklists[slice_id].size() == 0)
      {
        slice_id = _task_ctrl->chose_new_slice(slice_id);
        continue;
      }
      int tot_tasks = _task_ctrl->move_tasks_from_wklist_to_tq(slice_id);

      if (tot_tasks != 0)
      {
        _bdary_cycles += (_graph_vertices * 8 / (_slice_count * 512));
      }
      simulate();
      slice_id = _task_ctrl->chose_new_slice(slice_id);
    }
    // activate these vertices
    int tasks_produced = 0;
    for (int i = 0; i < _graph_vertices; ++i)
    {
      // TODO: For CF, use the condition as in task_recreation_during_async
      // in CF_reduce, carefully decide operations...
      bool spawn = false;
      if (_config->_algo == cf)
      {
        spawn = should_cf_spawn_task(i);
      }
      else
      {
        spawn = should_spawn_task(_vertex_data[i], _scratch[i]);
      }
      // it used scratch for update during iteration, but it was synchronized earlier
      if (spawn)
      {
        ++tasks_produced;
        DTYPE priority = 0;
        if (!_config->is_vector())
        {
          _vertex_data[i] = _scratch[i];
          priority = _scratch[i];
        }
        task_entry new_task(i, _offset[i]);
        _task_ctrl->push_task_into_worklist(_scratch_ctrl->get_slice_num(i), priority, new_task);
      }
    }
    // Let's check the mean error
    print_cf_mean_error();
    cout << "Tasks produced in phase 2: " << tasks_produced << endl;
    slice_id = _task_ctrl->chose_new_slice(slice_id);
  }
  print_stats();
  slicing_stats();
  finish_simulation();
}

int asic::calc_dyn_reuse()
{
#if DYN_REUSE == 0
  return REUSE;
#endif
  int reuse_factor = 1;
  // TODO: int prediction = tot_bdary*diameter;
  /* Two things:
   * Is our way of chosing lower or higher reuse factor good?
   * Is the factor that we are dropping to, good?
   */
  // int tot_bdary=0;
  // for(int slice_id=0; slice_id<_slice_count; ++slice_id) tot_bdary += _num_boundary_nodes_per_slice[slice_id];
  int graph_prop = E / _graph_vertices; // average degree
  // 9, 16, 100, 500 => 100 instead of 35, 60, 10, 2
  graph_prop = 1000 / GRAPH_DIA; // 3, 16, 8, 9 instead of 16 earlier
  // size of delta updates in the previous round
  int expected_impact = _stats->_stat_tot_num_copy_updates * graph_prop; // multiplying by branching factor/average degree
  if (expected_impact != 0)
  {
    reuse_factor = max(1, _graph_vertices / expected_impact);
    reuse_factor = min(reuse_factor, REUSE);
  }
  else
  {
    reuse_factor = max(1, 8 / graph_prop); // oh 4 came from here because copy updates is 0
  }
  cout << "Calculated reuse factor: " << reuse_factor << " graph prop: " << graph_prop << " expected impact: " << _stats->_stat_tot_num_copy_updates << endl;
  /*if(_stats->_stat_tot_num_copy_updates > SLICE_ITER*tot_bdary) {
    reuse_factor/=2; 
  }*/
  return reuse_factor;
}

void asic::update_gradient_of_dependent_slices(int slice_id)
{
#if ABCD == 1
  for (int slice = 0; slice < SLICE_COUNT; ++slice)
  {
    if (slice == slice_id)
      continue;
    _grad[slice] = 0;
    for (int i = slice * _scratch_ctrl->_slice_size; i < (slice + 1) * _scratch_ctrl->_slice_size && i < _graph_vertices; ++i)
    {
      if (_vertex_data[i] != _scratch[i])
      {
        _grad[slice] += abs(_vertex_data[i] - _scratch[i]);
        _vertex_data[i] = _scratch[i];
        int core = _scratch_ctrl->get_local_scratch_id(i);
        task_entry new_task(i, _offset[i]);
        _stats->_tot_activated_vertices++;
        _task_ctrl->push_task_into_worklist(slice, 0, new_task);
      }
    }
  }
#endif
}

// propagate all pending updates to the left over layers
// Step 1: push current layer tasks in the task queue
// Step 2: for each reuse, execute the tasks for left over layers
void asic::synchronizing_sync_reuse_lost_updates(int reuse_factor)
{
  _finishing_phase = true;
  queue<int> temp_updates[SLICE_COUNT];
  cout << "In second phase, delta updates ";
  for (int reuse = 0; reuse < reuse_factor; ++reuse)
  {
    // TODO: did sorting help?
    sort(_delta_updates[reuse].begin(), _delta_updates[reuse].end());
    cout << " for reuse: " << reuse << " is: " << _delta_updates[reuse].size();
  }
  cout << endl;

  // than current slice would be loaded from memory
  for (int reuse = 0; reuse < reuse_factor - 1; ++reuse)
  {
    // collect all prior updates
    int num_tasks_pushed = 0;
    int prev_vid = -1;
    for (unsigned j = 0; j < _delta_updates[reuse].size(); ++j)
    {
      int src_id = _delta_updates[reuse][j].first;
      assert(src_id < _graph_vertices);
      bool should_spawn = false;
      if (_config->_algo == gcn)
      {
        if (src_id == prev_vid)
          continue; // sure it will continue from innermost loop?
        prev_vid = src_id;
        should_spawn = (reuse < reuse_factor - 1); // need to apply all tasks always
      }
      else if (_config->_algo == cf)
      {
        should_spawn = (should_cf_spawn_task(src_id));
      }
      else
      {
        assert(_vertex_data[src_id] == _scratch[src_id]);
        int old_dist = _vertex_data[src_id];
        _vertex_data[src_id] = reduce(_vertex_data[src_id], _delta_updates[reuse][j].second);
        should_spawn = (old_dist != _vertex_data[src_id]);
        if (should_spawn)
          _scratch[src_id] = _vertex_data[src_id];
      }
      if (!should_spawn)
        continue;
      if (_config->_sync_scr)
      {
        temp_updates[_scratch_ctrl->get_slice_num(src_id)].push(src_id);
      }
      else
      {
        task_entry new_task(src_id, _offset[src_id]);
        _stats->_tot_activated_vertices++;
        _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(src_id), 0, new_task);
      }
      num_tasks_pushed++;
    }
    _delta_updates[reuse].clear();

    // Step 2: propagate these for "left" iterations
    for (int left = 0; left < reuse_factor - reuse - 1; ++left)
    {

      if (_config->_sync_scr)
      {
        _global_iteration++;
        for (int slice = 0; slice < SLICE_COUNT; ++slice)
        {
          _current_slice = slice;
          while (!temp_updates[slice].empty())
          {
            int src_id = temp_updates[slice].front();
            task_entry new_task(src_id, _offset[src_id]);
            _stats->_tot_activated_vertices++;
            _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(src_id), 0, new_task);
            temp_updates[slice].pop();
          }
          simulate();
        }
      }
      else
      {
        simulate(); // this will always consider all accesses as hits
      }

      // update all new vertices
      for (int i = 0; i < _graph_vertices; ++i)
      {
        bool spawn = false;
        if (_config->_algo == cf)
        {
          spawn = should_cf_spawn_task(i);
        }
        else if (_config->_algo != gcn)
        {
          spawn = _vertex_data[i] != _scratch[i];
          _vertex_data[i] = _scratch[i];
        }
        if (spawn)
        {
          task_entry new_task(i, _offset[i]);
          _stats->_tot_activated_vertices++;
          if (left == (reuse_factor - reuse - 2))
          { // last tasks should be in memory
            _task_ctrl->push_task_into_worklist(_scratch_ctrl->get_slice_num(i), 0, new_task);
          }
          else
          {
            _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(i), 0, new_task);
          }
        }
      }
    }
    _task_ctrl->reset_task_queues();
  }
  /*if(_config->_algo==gcn) {
    assert(num_tasks_pushed<_graph_vertices);
  }
  cout << "Number of tasks pushed after compute phase: " << num_tasks_pushed << endl;
  */
  while (_delta_updates[reuse_factor - 1].size() > 0)
  {
    _delta_updates[reuse_factor - 1].pop_back();
  }
}

void asic::simulate_graphmat_slice()
{
  slice_init();
  int tasks_produced = 0;

  // for(int i=0; i<SLICE_COUNT; ++i) cout << "Worklist size: " << i << " is: " << _worklists[i].size();

  // none of the slices should have active work
  while (!_task_ctrl->worklists_empty())
  {
    int total_active_tasks = _task_ctrl->tot_pending_tasks();
    _start_copy_vertex_update = _stats->_stat_tot_num_copy_updates;
    _start_active_vertices = total_active_tasks;
    _global_iteration++;
    int start = _cur_cycle;
    int reuse_factor = calc_dyn_reuse();

    // In general, required for local iteration stats
    _stats->_stat_tot_num_copy_updates = 0;
    // for(int slice_id=0; slice_id<_slice_count; ++slice_id) {
    int slice_id = -1;
    slice_id = _task_ctrl->chose_new_slice(slice_id);
    int served = 0;
    while (served < _slice_count)
    {
      served++;
#if ALLHITS == 1
      reset_compulsory_misses(); // so that it considers hot misses in the cache??
#endif

      // TODO: for each slice iteration, decide what should be the reuse factor
      // How many times do we want to execute this slice? (What is the
      // probability of high work efficiency?)
      // TODO: if the size of delta updates is too high, then break
      // What if in this way, the later slices are always skipped or executed
      // less number of times? should we check these updates per slice?
      // if(delta_updates[reuse].size() > SLICE_ITER*_num_boundary_nodes_per_slice) {

      // need to have same for all slices so that sync. condition holds true
      // Learn from the previous case

      print_local_iteration_stats(-1, -1);
      _current_slice = slice_id;
      // push tasks stored in worklists into the task queue
      cout << "Starting with slice: " << slice_id << " global iteration: " << _global_iteration << " with initial tasks: " << _task_ctrl->_worklists[slice_id].size() << endl;
      int tasks_pushed = _task_ctrl->move_tasks_from_wklist_to_tq(slice_id);
      if (tasks_pushed != 0)
      {
        _bdary_cycles += (_graph_vertices * 8 / (_slice_count * 512));
      }
      _space_allotted = 0;
      // FIXME: do we know the variable size of each slice (or maybe I guess we just drop extra elements)
      for (int reuse = 0; reuse < reuse_factor; ++reuse)
      {
        _current_reuse = reuse;
        simulate();
        // print_local_iteration_stats(slice_id, reuse);
        // TODO: keep a flag for getting these detailed stats
#if ABCD == 1
        _grad[slice_id] = 0;
#endif

        // how about boundary vertices? who will activate them?
        // TODO: should I call the common slice creation function here?
        for (int i = slice_id * _scratch_ctrl->_slice_size; i < (slice_id + 1) * _scratch_ctrl->_slice_size && i < _graph_vertices; ++i)
        {
#if GCN == 1

          if ((_global_iteration - 1) * reuse_factor + reuse < GCN_LAYERS - 1)
          {
#elif CF == 1
          if (should_cf_spawn_task(i))
          { // copy should_spawn_task logic here

#else
          if (_vertex_data[i] != _scratch[i])
          {
#if ABCD == 1
            _grad[slice_id] += abs(_vertex_data[i] - _scratch[i]);
#endif
            _vertex_data[i] = _scratch[i];
#endif
            tasks_produced++;
            int core = _scratch_ctrl->get_local_scratch_id(i);
            task_entry new_task(i, _offset[i]);
            _stats->_tot_activated_vertices++;
            if (reuse == reuse_factor - 1)
            {
              // here store in worklist because we are not using them now
              _task_ctrl->push_task_into_worklist(slice_id, 0, new_task);
            }
            else
            {
              _task_ctrl->insert_new_task(0, 0, core, 0, new_task);
            }
          }
        }
        // cout << "Tasks produced in phase 2: " << tasks_produced << " for slice: " << slice_id << " and reuse: " << reuse << endl;
        // cout << "Updated gradient of the old slice: " << slice_id << " is: " << _grad[slice_id] << endl;
        tasks_produced = 0;
      }
      // TODO: how about updates to all other slices?
#if ABCD == 1
      for (int slice = 0; slice < SLICE_COUNT; ++slice)
      {
        if (slice == slice_id)
          continue;
        _grad[slice] = 0;
        for (int i = slice * _scratch_ctrl->_slice_size; i < (slice + 1) * _scratch_ctrl->_slice_size && i < _graph_vertices; ++i)
        {
          if (_vertex_data[i] != _scratch[i])
          {
            _grad[slice] += abs(_vertex_data[i] - _scratch[i]);
            _vertex_data[i] = _scratch[i];
            int core = _scratch_ctrl->get_local_scratch_id(i);
            task_entry new_task(i, _offset[i]);
            _stats->_tot_activated_vertices++;
            _task_ctrl->push_task_into_worklist(slice, 0, new_task);
          }
        }
      }
#endif
      slice_id = _task_ctrl->chose_new_slice(slice_id); // before data is already written
    }
    print_local_iteration_stats(0, 0);

    // TODO: based on the number of outgoing updates, decide whether to overlap or not...
    // synchronize the pending tasks
#if SYNC_SIM == 0
    queue<int> temp_active_vert;
#endif
    start = _cur_cycle;
    // cout << "Start sync phase at cycle: " << _cur_cycle << endl;
    // TODO: Check the overhead with a large cache or small cache with just a scratch where all other

    // stats for the finishing phase
    _finishing_phase = true;
#if RESIZING == 1 && WORKING_CACHE == 0
    _mem_ctrl->_l2_cache = new SIMPLE_CACHE(16384 * 1024, 8, 1, 64, 0, 1);
#endif
#if SYNC_SCR == 1
    queue<int> temp_updates[SLICE_COUNT];
#endif

    cout << "In second phase, delta updates ";
    for (int reuse = 0; reuse < reuse_factor; ++reuse)
    {
      // TODO: did sorting help?
      sort(_delta_updates[reuse].begin(), _delta_updates[reuse].end());
      cout << " for reuse: " << reuse << " is: " << _delta_updates[reuse].size();
    }
    cout << endl;

    // than current slice would be loaded from memory
    for (int reuse = 0; reuse < reuse_factor - 1; ++reuse)
    {
      // collect all prior updates
      int num_tasks_pushed = 0;
      int prev_vid = -1;
      for (unsigned j = 0; j < _delta_updates[reuse].size(); ++j)
      {
        int src_id = _delta_updates[reuse][j].first;
        assert(src_id < _graph_vertices);
#if GCN == 1
        // if the vertex is same as earlier, drop
        if (src_id == prev_vid)
          continue; // sure it will continue from innermost loop?
        prev_vid = src_id;
        // cout << "New task in round reuse: " << reuse << " and the pushed vertex: " << src_id << endl;
        // TODO: apply the update to GCN (delta updates would need to copy a lot of it)
        if (reuse < reuse_factor - 1)
        { // need to apply all tasks always
#elif CF == 1
        // FIXME: THIS IS WRONG!!!
        // int old_dist = _vertex_data[src_id];
        // _vertex_data[src_id] = reduce(_vertex_data[src_id], _delta_updates[reuse][j].second);
        if (should_cf_spawn_task(src_id))
        {
#else
        assert(_vertex_data[src_id] == _scratch[src_id]);
        int old_dist = _vertex_data[src_id];
        _vertex_data[src_id] = reduce(_vertex_data[src_id], _delta_updates[reuse][j].second);
        if (old_dist != _vertex_data[src_id])
        {
          // FIXME: seems wrong (check!)
          _scratch[src_id] = _vertex_data[src_id];
#endif
          // these should be hardware tasks: can just be pushed in the hardware queue
          // later they will be moved to worklists
#if SYNC_SIM == 0
          temp_active_vert.push(src_id);
#else

#if SYNC_SCR == 1
          temp_updates[get_slice_num(src_id)].push(src_id);
#else
          task_entry new_task(src_id, _offset[src_id]);
          _stats->_tot_activated_vertices++;
          _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(src_id), 0, new_task);
#endif
#endif
          num_tasks_pushed++;
        }
      }
      // while(_delta_updates[reuse].size()>0) _delta_updates[reuse].pop_back();
      _delta_updates[reuse].clear();
#if GCN == 1 // actually task coalescing is true
      assert(num_tasks_pushed < _graph_vertices);
#endif
      cout << "Number of tasks pushed after compute phase: " << num_tasks_pushed << endl;
      // FIXME: CHECK!!
      while (_delta_updates[reuse_factor - 1].size() > 0)
        _delta_updates[reuse_factor - 1].pop_back();
      // I am not sure of the issue here
      // FIXME: if(num_tasks_pushed==0) continue;

      // propagate these for "left" iterations
      for (int left = 0; left < reuse_factor - reuse - 1; ++left)
      {

#if SYNC_SCR == 1
        _global_iteration++;
        for (int slice = 0; slice < SLICE_COUNT; ++slice)
        {
          _current_slice = slice;
          while (!temp_updates[slice].empty())
          {
            int src_id = temp_updates[slice].front();
            _stats->_tot_activated_vertices++;
            _task_ctrl->insert_new_task(0, 0, get_local_scratch_id(src_id), 0, new_task);
            temp_updates[slice].pop();
          }
          simulate();
        }
#elif SYNC_SIM == 1
        simulate(); // this will always consider all accesses as hits
#else
#if CF == 0
        int vert_to_execute = temp_active_vert.size();
        while (vert_to_execute > 0)
        {
          vert_to_execute--;
          int src_id = temp_active_vert.front();
          temp_active_vert.pop();
          for (int j = _offset[src_id]; j < _offset[src_id + 1]; ++j)
          {
            _stats->_stat_tot_finished_edges++;
            _scratch_ctrl->_edges_visited++;
            DTYPE res = _vertex_data[src_id] + _neighbor[j].wgt;
            int dest_id = _neighbor[j].dst_id;
            _scratch[dest_id] = min(_scratch[dest_id], res);
          }
        }
#endif
#endif
        // update all new vertices
        for (int i = 0; i < _graph_vertices; ++i)
        {
          bool spawn = false;
          if (_config->_algo == cf)
          {
            spawn = should_cf_spawn_task(i);
          }
          else if (_config->_algo != gcn)
          {
            spawn = _vertex_data[i] != _scratch[i];
            _vertex_data[i] = _scratch[i];
          }
          if (spawn)
          {
#if SYNC_SIM == 0
            temp_active_vert.push(i);
#else
            task_entry new_task(i, _offset[i]);
            _stats->_tot_activated_vertices++;
            if (left == (reuse_factor - reuse - 2))
            { // last tasks should be in memory
              _task_ctrl->push_task_into_worklist(_scratch_ctrl->get_slice_num(i), 0, new_task);
            }
            else
            {
              _task_ctrl->insert_new_task(0, 0, _scratch_ctrl->get_local_scratch_id(i), 0, new_task);
            }
#endif
          }
        }
      }
      // all iterations done, move temp_active_vertex to original
      // TODO: empty the task queues and push the left tasks in the worklists
#if SYNC_SIM == 1 // && GCN==0
      _task_ctrl->reset_task_queues();
#else
      while (!temp_active_vert.empty())
      {
        int src = temp_active_vert.front();
        int cur_slice = _scratch_ctrl->get_slice_num(src);
        task_entry src_task(src, _offset[src]);
        _task_ctrl->push_task_into_worklist(cur_slice, 0, src_task);
        temp_active_vert.pop();
      }
#endif
    }
    _finishing_phase = false;
#if RESIZING == 1
    _mem_ctrl->_l2_cache = new SIMPLE_CACHE(L2SIZE * 1024, 8, 1, 64, 0, 1);
#endif

    // cout << "End sync phase at cycle: " << _cur_cycle << endl;
    _add_on_cycles += (_cur_cycle - start);
    // cout << "L2 hit rate now: " << (_l2hits-_prev_l2hits)/(double)(_l2accesses-_prev_l2accesses) << endl;
    _mem_ctrl->_prev_l2hits = _mem_ctrl->_l2hits;
    _mem_ctrl->_prev_l2accesses = _mem_ctrl->_l2accesses;
  }
  _stats->_stat_global_finished_edges = _stats->_stat_tot_finished_edges;
  print_stats();
  // finish_simulation();
  cout << "Total cycles: " << _cur_cycle << " cycles in sync phase: " << _add_on_cycles << " and in extreme pahses: " << _extreme_cycles << endl;
  cout << "memory bandwidth of add-on case: " << _stats->_stat_dram_add_on_access / (float)_add_on_cycles << endl;
  cout << "Total Edges in sync phase: " << _stats->_stat_sync_finished_edges / (double)E << " and in extreme iterations: " << _stats->_stat_extreme_finished_edges / (double)E << endl;
  slicing_stats();
  finish_simulation();
}

void asic::slicing_stats()
{
  // Print overall work-efficiency numbers
  cout << "Number of global iterations: " << _global_iteration << endl;
  cout << "Extra cycles in graphdyns implementation: " << (_stats->_tot_activated_vertices * 8 / 512) << endl;
  cout << "Overall Edges executed: " << _stats->_stat_global_finished_edges / (double)E << endl;
  // for each edge, we loaded dst_id, edge_weight, (for vert updates: src/dst
  // vert prop)
  uint64_t tot_data_requested = (_stats->_stat_global_finished_edges * (message_size + 4)); // edge weight and dst id and index
  uint64_t tot_mem_accesses = 0;
#if SLICE_COUNT == 1
  tot_mem_accesses = (_stats->_stat_dram_cache_accesses * 64);
#else
  tot_mem_accesses = (_stats->_stat_dram_cache_accesses * 64) + (uint64_t)(_global_iteration * _graph_vertices * 8);
#endif
  double miss_rate = tot_mem_accesses / (double)tot_data_requested;
  cout << "Total bytes used by algorithm: " << tot_data_requested << "  total bytes accessed from memory (always 64 bytes due to caching): " << tot_mem_accesses << endl;
  // cout << "Overall hit rate: " << (tot_data_requested-tot_mem_accesses)/(double)(tot_data_requested) << endl;
  cout << "Overall hit rate: " << (1 - miss_rate) << endl;
  tot_mem_accesses = (_stats->_stat_dram_bytes_requested) + (uint64_t)(_global_iteration * _graph_vertices * 8);
  miss_rate = tot_mem_accesses / (double)tot_data_requested;
  cout << "Exact hit rate: " << (1 - miss_rate) << endl;
}

void asic::initialize_stats_per_graph_round()
{
  _start_copy_vertex_update = _stats->_stat_tot_num_copy_updates;
  _start_active_vertices = _task_ctrl->tot_pending_tasks();
  _global_iteration++;
}

void asic::simulate_slice()
{
  // initialize all worklists
  if (_cur_cycle == 0)
  {
    slice_init();
  }
  print_local_iteration_stats(-1, -1);
  // simulate until all worklists are not empty
  while (!_task_ctrl->worklists_empty() || _task_ctrl->tot_pending_tasks() > 0)
  {
    initialize_stats_per_graph_round();

    // parameters required for scheduling slices
    int start = _cur_cycle;
    const int reuse_factor = calc_dyn_reuse(); // only applicable sync versions
    int cur_reuse_count = reuse_factor;

    int slice_id = -1;
    slice_id = _task_ctrl->chose_new_slice(slice_id);
    bool outside = _config->_graphmat && !_switched_async && !_switched_cache;
    if (outside)
      print_local_iteration_stats(0, 0);

    int served = 0;
    while (served < _slice_count && _global_iteration < SLICE_NUM_ITER)
    {
      if (!outside)
        print_local_iteration_stats(0, 0);
      cur_reuse_count--;
      _current_slice = slice_id;
      served++;
      _local_iteration++;

      cout << "round: " << _global_iteration << " slice: " << slice_id << " worklist size beforehand: " << _task_ctrl->_worklists[slice_id].size() << " at cycle: " << _cur_cycle << " and remote tasks created this phase: " << _stats->_stat_remote_tasks_this_phase << " and worklist size used: " << _stats->_stat_owned_tasks_this_phase << endl;

      _stats->_stat_remote_tasks_this_phase = 0;
      _stats->_stat_owned_tasks_this_phase = 0;
      _task_ctrl->_check_vertex_done_this_phase.reset();

      // FIXME: consider duplicate (this should be reset already during reset_task_queues?)
      if (_config->_abort || _config->_entry_abort)
      {
        assert(_config->_pull_mode || _task_ctrl->_worklists[slice_id].size() <= ceil(_graph_vertices / (float)_slice_count));
        for (int a = 0; a < core_cnt; ++a)
        {
          _task_ctrl->_present_in_local_queue[a].reset();
        }
      }

      if ((_task_ctrl->_worklists[slice_id].size() + _task_ctrl->tot_pending_tasks()) == 0)
      {
        slice_id = _task_ctrl->chose_new_slice(slice_id);
        continue;
      }

      _bdary_cycles += (_graph_vertices * 8 / (_slice_count * 512));

      // load worklist into the task queues
      int tasks_pushed = _task_ctrl->move_tasks_from_wklist_to_tq(slice_id);
      _async_finishing_phase = false;
      simulate(); // this also would have created new tasks?

      _grad[slice_id] = 0; // TODO: used for ABCD meaning slice scheduling

      cout << "round, slice: " << slice_id << " worklist size left: " << _task_ctrl->_worklists[slice_id].size() << endl;
      assert(_cur_cycle < _max_iter && "TIMEOUT\n");
      assert(_global_iteration < SLICE_NUM_ITER && "TIMEOUT\n");

      // TODO: sync task creation required at the end of the slice...!!!
      int tasks_produced = task_recreation_during_sync(slice_id, (cur_reuse_count == 0), reuse_factor);
      update_gradient_of_dependent_slices(slice_id); // FIXME: not sure if we should do this...

      if (cur_reuse_count == 0)
      {
        slice_id = _task_ctrl->chose_new_slice(slice_id);
        cur_reuse_count = reuse_factor;
      }
    }
    assert(_global_iteration < SLICE_NUM_ITER && "TIMEOUT");
    cout << _global_iteration << " rounds of worklists done!\n";
    // merge pending updates here??? (another sync operation)
    if (!_config->is_async())
    {
      synchronizing_sync_reuse_lost_updates(reuse_factor);
    }
  }

  assert(_global_iteration < SLICE_NUM_ITER && "the computation has exceeded maximum iterations");
  cout << "Edges executed: " << _stats->_stat_global_finished_edges / (double)E << endl;

#if PR == 0 && CF == 0
  for (int i = 0; i < _graph_vertices; ++i)
  {
    assert(_scratch[i] <= _vertex_data[i]);
    _vertex_data[i] = _scratch[i];
  }
#endif
  print_stats();
  slicing_stats();
  finish_simulation();
}

void asic::simulate_sgu_slice()
{
  // initialize all worklists
  slice_init();
  int iter = 0;
  // simulate until all worklists are not empty
  while (!_task_ctrl->worklists_empty())
  {
    _global_iteration++;
    iter++;
    print_local_iteration_stats(0, 0);

    int slice = -1;
    slice = _task_ctrl->chose_new_slice(slice);

    int served = 0;
    while (served < _slice_count)
    {
      served++;

      // for(int slice=0; slice<_slice_count; ++slice) {
      _local_iteration++;
      _start_copy_vertex_update = _stats->_stat_tot_num_copy_updates;
      _start_active_vertices = _task_ctrl->_worklists[slice].size();
      if (iter > SLICE_NUM_ITER)
        exit(0);
      cout << "round: " << iter << " slice: " << slice << " worklist size beforehand: " << _task_ctrl->_worklists[slice].size() << " at cycle: " << _cur_cycle << " and remote tasks created this phase: " << _stats->_stat_remote_tasks_this_phase << " and worklist size used: " << _stats->_stat_owned_tasks_this_phase << endl;
      _stats->_stat_remote_tasks_this_phase = 0;
      _stats->_stat_owned_tasks_this_phase = 0;
      _task_ctrl->_check_vertex_done_this_phase.reset();
#if ABORT == 1 || ENTRY_ABORT == 1 // FIXME: consider duplicate
      assert(_task_ctrl->_worklists[slice].size() <= ceil(_graph_vertices / (float)_slice_count));
      for (int a = 0; a < core_cnt; ++a)
      {
        _task_ctrl->_present_in_local_queue[a].reset();
      }
#endif
      if (_task_ctrl->_worklists[slice].size() == 0)
      {
        slice = _task_ctrl->chose_new_slice(slice);
        continue;
      }

      _bdary_cycles += (_graph_vertices * 8 / (_slice_count * 512));

      _current_slice = slice;
      int computations = 0;
      //load worklist into the task queues
      _slicing_threshold = SLICE_ITER * _task_ctrl->_worklists[slice].size();
      // TRYING: FIXME:CHECKME
      // if(_worklists[slice].size()<2500) _slicing_threshold *= 10;
      //
      while (!_task_ctrl->_worklists[slice].empty())
      {
        // if(_worklists[slice].front().first!=0) { // don't push if priority=0
        task_entry cur_task = _task_ctrl->_worklists[slice].front().second;
        // if(cur_task.vid==4976) cout << "Pushing my focus from global queue with dist: " << cur_task.dist << endl;
#if PR == 1 && CF == 0
        if (cur_task.dist != 0)
        { // don't push if residual=0, meaning no incoming value
#else
        if (1)
        {
#endif
#if WORK_STEALING == 0
          _task_ctrl->insert_global_task(_task_ctrl->_worklists[slice].front().first, cur_task);
#else
          int target_core = _scratch_ctrl->get_local_scratch_id(cur_task.vid);
          int vid = cur_task.vid;
#if PR == 0 && ABCD == 0 && FIFO == 0 && PULL == 0
          if (_scratch[vid] > _task_ctrl->_worklists[slice].front().first)
          {
            cout << "vid: " << vid << " scratch: " << _scratch[vid] << " worklist: " << _task_ctrl->_worklists[slice].front().first << endl;
          }
          assert(_scratch[vid] <= _task_ctrl->_worklists[slice].front().first && "updated distance should be better/same than the one stored in worklist");
#endif
#if GCN == 0 && CF == 0 && PULL == 0
          // cout << "Current vertex: " << vid << " scratch: " << _scratch[vid] << endl;
          assert(_scratch[vid] < MAX_TIMESTAMP - 1);
#endif
          // cout << "Inserting a local task in tqid: " << _asic_cores[target_core]->_agg_push_index << " and vid: " << vid << " target core: " << target_core << endl;
          // cout << "degree start: " << _offset[vid] << " end: " << _offset[vid+1] << endl;
          // insert_local_task(_asic_cores[target_core]->_agg_push_index, 0, target_core, _worklists[slice].front().first, cur_task);
          DTYPE priority = 0;
#if GCN == 0 && CF == 0
          priority = apply(_scratch[vid], vid);
#endif
          if (priority != -1)
          {
            _task_ctrl->insert_local_task(_asic_cores[target_core]->_agg_push_index, 0, target_core, priority, cur_task);
          }
          _asic_cores[target_core]->_agg_push_index = (_asic_cores[target_core]->_agg_push_index + 1) % NUM_TQ_PER_CORE;
#endif
        }
        _task_ctrl->_worklists[slice].pop();
        _task_ctrl->_check_worklist_duplicate[slice].reset(cur_task.vid);
      }
      _async_finishing_phase = false;
      simulate();
      cout << "round, slice: " << slice << " worklist size left: " << _task_ctrl->_worklists[slice].size() << endl;
      if (_cur_cycle > _max_iter)
      {
        cout << "TIMEOUT\n";
        break;
      }
      slice = _task_ctrl->chose_new_slice(slice);
    }
    cout << iter << " rounds of worklists done!\n";
  }
  _global_iteration = iter;
  cout << "Edges executed: " << _stats->_stat_global_finished_edges / (double)E << endl;

#if PR == 0
  for (int i = 0; i < _graph_vertices; ++i)
  {
    assert(_scratch[i] <= _vertex_data[i]);
    _vertex_data[i] = _scratch[i];
  }
#endif
  print_stats();
  slicing_stats();
  finish_simulation();
}

bool asic::can_push_in_global_bank_queue(int bank_id)
{
  return (_bank_queues->size() < FIFO_DEP_LEN);
}

int scratch_controller::get_slice_num(int vid)
{
#if SGU_SLICING == 0 && GRAPHMAT_SLICING == 0 && BLOCKED_ASYNC == 0
  return 0;
#endif
  // return _stats->_slice_num[vid];
  int slice = vid / _slice_size;
  // if(slice==_slice_count) slice=slice-1;
  return slice;
  /*
  for(int i=0; i<_slice_count; ++i) {
    if(vid > _cum_slice_size[i]) return i;
  }
  return -1;
  */
}

void asic::slice_init()
{
#if PR == 0 // sssp/bfs
  int degree = _offset[_src_vid + 1] - _offset[_src_vid];
#if PULL == 1
  degree = _csr_offset[_src_vid + 1] - _csr_offset[_src_vid];
#endif
  assert(degree != 0 && "source vertex degree is 0, deadlock...");
  task_entry cur_task(_src_vid, _offset[_src_vid]);
  _virtual_finished_counter++;
  int wklist = _scratch_ctrl->get_slice_num(_src_vid);
  _task_ctrl->push_task_into_worklist(wklist, 0, cur_task);
  cout << "Pushed new src_vid: " << _src_vid << " to worklist: " << wklist << " with degree: " << degree << endl;
  return;
#endif
  // read slice files and initialize worklists: it will include all incoming
  // and outgoing edges (vertices corresponding to original and incoming are
  // pushed in the worklist)
  // vertex property duplicated for both cases
  // edge duplicated only for my case
  int jump = _graph_vertices / _slice_count;
  int pushed = 0;
  for (int slice = 0; slice < _slice_count; ++slice)
  {
    for (int v = slice * jump; v < (slice + 1) * jump; ++v)
    {
      // cout << "input size of worklist at i: " << slice << " is: " << _worklists[slice].size() << endl;
      task_entry cur_task(v, _offset[v]);
#if CF == 0
      cur_task.dist = _vertex_data[v];
#endif
      _virtual_finished_counter++;
      DTYPE priority = 0;
#if CF == 0
      priority = apply(_vertex_data[v], v);
      cur_task.dist = _vertex_data[v];
#endif
      // if(should_spawn_task(0, cur_task.dist)) { // it will come here only for PR
      if (1)
      {
        ++pushed;
        // cout << "priority of initial tasks: " << priority << endl;
        _task_ctrl->push_task_into_worklist(slice, priority, cur_task);
        _task_ctrl->_check_worklist_duplicate[slice].set(cur_task.vid);
      }
    }
  }
  for (int v = pushed; v < _graph_vertices; ++v)
  {
    task_entry cur_task(v, _offset[v]);
    _virtual_finished_counter++;
    _task_ctrl->push_task_into_worklist(_slice_count - 1, 0, cur_task);
  }
  // exit(0);
  // FIXME: not sure if it has to be pushed in worklist as well!

  // int mismatch_vert[_slice_count];
  // for(int i=0; i<_slice_count; ++i) mismatch_vert[i]=0;

  // I think not required with this slicing scheme
  for (int slice = 0; slice < _slice_count; ++slice)
  {
    cout << "initial size of worklist at i: " << slice << " is: " << _task_ctrl->_worklists[slice].size() << endl; // " with mismatch vertices: " << mismatch_vert[slice] << endl;
  }
  // exit(0);
}

/*
void asic::fill_edge_mapping() {
  int cc=0, index=0;
  int max_cc = ceil(line_size/6);
  // want to know for _mapping_bfs[i]
  for(int i=0; i<_graph_vertices; ++i) {
    int 


  }



}*/

void asic::write_csr_file()
{
  // string out_file("mlrat_csr");
  // ofstream csr_file(out_file.c_str());
  ofstream csr_file2("mlrat_csr2");
  if (csr_file2.is_open())
  {
    csr_file2 << _graph_vertices << " " << _graph_vertices << " " << E << endl;
    for (int i = 0; i < _graph_edges; ++i)
    {
      csr_file2 << (_neighbor[i].src_id + 1) << " " << (_neighbor[i].dst_id + 1) << " " << _neighbor[i].wgt << endl;
    }
  }
  csr_file2.close();
}

// assume graph is undirected -- hence, same layout works for both
void asic::ladies_sampling()
{

  char linetoread[50000];
  cout << "Starting reading adj matrix file\n";
  FILE *adj_new = fopen("adj_mat_cora_512.txt", "r");

  int row = 0;
  int l = 0;
  while (fgets(linetoread, 50000, adj_new) != NULL)
  {
    std::string raw(linetoread);
    std::istringstream iss(raw.c_str());
    string ignore;
    int i = -1;
    int x;
    l = row / 2;
    while (getline(iss, ignore, ' '))
    {
      if (row % 2 == 0)
      { // vertex_ptr
        _offset[++i] = atoi(ignore.c_str());
      }
      else
      { // edge list
        _neighbor[++i].dst_id = atoi(ignore.c_str());
      }
    }
    cout << "_graph_verticesalue of i after whole row: " << i << endl;
    if (row % 2 == 0)
      cout << "Total edges in layer l: " << l << " is: " << _offset[LADIES_SAMPLE] << endl;
    ++row;
  }
  fclose(adj_new);
  cout << "Done reading all files\n";

  return;

  cout << "First find laplacian matrix for the output layer\n";

  int i, j;
  // for(i=0; i<_graph_edges; ++i) assert(_neighbor[i].dst_id<_graph_vertices);

  // Get laplacian matrix P = D-1/2AD-1/2
  // d is degree of all nodes
  // 1st multiplication is 1/sqrt(outgoing degree) * (if incoming edge to it) -- _graph_verticesx_graph_vertices (oh just scaling by the corresponding factor)
  for (i = 0; i < _graph_vertices; ++i)
  { // all vertices
    int out_degree = _offset[i + 1] - _offset[i];
    for (j = _offset[i]; j < _offset[i + 1]; ++j)
    { // index of the corresponding edge
      edge_info e = _neighbor[j];
      int out_degree_of_dest = _offset[e.dst_id + 1] - _offset[e.dst_id];
      out_degree += 1;
      out_degree_of_dest += 1;
      _neighbor[j].wgt = 1 * 1 / (float)(sqrt(out_degree) * sqrt(out_degree_of_dest));
    }
  }

  for (i = 0; i < LADIES_SAMPLE; ++i)
  {
    srand(i);
    _sampled_nodes[GCN_LAYERS][i] = rand() % _graph_vertices;
  }

  cout << "Sampled nodes for the output layer\n";

  for (int l = GCN_LAYERS - 1; l >= 0; --l)
  {

    float prob[_graph_vertices]; // = {0};

    for (i = 0; i < _graph_vertices; ++i)
      prob[i] = 0;
    // Q.P (256x_graph_vertices)

    // This can have maximum value of 256
    for (i = 0; i < LADIES_SAMPLE; ++i)
    { // reuse on the graph
      int ind = _sampled_nodes[l + 1][i];
      // prob[i] = vertex_ptr[ind+1]-vertex_ptr[ind];
      for (j = _offset[ind]; j < _offset[ind + 1]; ++j)
      {
        prob[_neighbor[j].dst_id] += 1; // pow(wgt[j]*1,2);

        // cout << "i: " << i << " j: " << j << " dst: " << edge_list[j] << " and new prob: " << prob[edge_list[j]] << endl;
      }
    }

    for (i = 0; i < _graph_vertices; ++i)
    {
      if (prob[i] != 0)
        // cout << "i: " << i << " prob: " << prob[i] << endl;
        assert(prob[i] <= LADIES_SAMPLE);
    }

    // frobeneous norm = trace(A*A)
    // int fnorm = sqrt(EDGES/2); // assuming undirected graph with 1 side edges=E
    // for(i=0; i<NODES; ++i) prob[i]/=(float)fnorm;

    // int num_non_zero_values=0,
    int non_zero_index = -1;
    vector<int> candidate_vid;
    float cumsum = 0;

    // fill sample nodes using the above probabiity
    // Step1: find cumsum
    // TODO: how to do cumsum effectively using multicore?
    for (i = 1; i < _graph_vertices; ++i)
    {
      // cout << "prob at i: " << prob[i] << endl;
      cumsum = prob[i] + prob[i - 1];
      if (prob[i] != 0)
      {
        prob[++non_zero_index] = cumsum;
        // cout << "Allocated prob: " << prob[non_zero_index] << endl;
        candidate_vid.push_back(i);
      }
      prob[i] = cumsum;
      // cout << "Cumulative probability for node i: " << i << " " << prob[i] << endl;
    }

    cout << "Number of candidates: " << non_zero_index << endl;
    float range = prob[non_zero_index] - prob[0];
    // cout << "start: " << prob[0] << " range: " << range << endl;

    // FIXME: error here...can't find
    int bstart = 0, bend = non_zero_index;
    int a, mid;
    float x;

    for (i = 0; i < LADIES_SAMPLE; ++i)
    {
      srand(i);
      a = rand() % 100;
      x = (a / (float)100) * range;
      x += prob[0];
      // binary search
      bstart = 0;
      bend = non_zero_index;
      mid = 0;
      while (1)
      {
        // cout << "prob[mid]: " << prob[mid] << " x: " << x << endl;
        mid = (bstart + bend) / 2;
        if (prob[mid] > x && prob[mid - 1] <= x)
          break;
        if (prob[mid] > x)
          bend = mid - 1;
        else
          bstart = mid + 1;
      }
      _sampled_nodes[l][i] = candidate_vid[mid];
    }

    /*for(int k=0; k<LADIES_SAMPLE; ++k) {
        cout << "Node selected: " << _sampled_nodes[l][k] << endl;
      }*/
    // layer-dependent laplacian matrix
    float interm[LADIES_SAMPLE];

    for (i = 0; i < LADIES_SAMPLE; ++i)
    {
      interm[i] = prob[_sampled_nodes[l][i]];
    }

    int e = 0, ind;
    for (i = 0; i < LADIES_SAMPLE; ++i)
    { // first row of Q.P
      ind = _sampled_nodes[l + 1][i];
      _sampled_offset[l].push_back(e);
      for (j = 0; j < LADIES_SAMPLE; ++j)
      {
        int val = 0;
        for (int k = _offset[ind]; k < _offset[ind + 1]; ++k)
        {
          if (_neighbor[k].dst_id == _sampled_nodes[l][j])
          {
            val = interm[j] * _neighbor[k].wgt;
            // sampled_edge_list[l].push_back(j); // this doesn't allow parallelization
            edge_info x;
            x.dst_id = j;
            x.wgt = val;
            _sampled_neighbor[l].push_back(x);
            ++e;
          }
        }
      }
    }
    // cout << "Number of edges at layer: " << l << " is: " << e << endl;
    _sampled_offset[l].push_back(e);
  }
  assert(_sampled_offset[0].size() == LADIES_SAMPLE + 1);
  assert(_sampled_offset[0][LADIES_SAMPLE] == _sampled_neighbor[0].size());
  cout << "Edges in layer 0: " << _sampled_offset[0][LADIES_SAMPLE] << endl;

  for (int i = 0; i < LADIES_SAMPLE; ++i)
    _correct_vertex_data[i] = 0;

  for (int i = 0; i < LADIES_SAMPLE; ++i)
  {
    _offset[i] = _sampled_offset[0][i];
    for (int j = _sampled_offset[0][i]; j < _sampled_offset[0][i + 1]; ++j)
    {
      _neighbor[j] = _sampled_neighbor[0][j];
      _neighbor[j].src_id = i;
      _neighbor[j].wgt = 1;
      _correct_vertex_data[_neighbor[j].dst_id] += 1;
    }
  }
  _offset[LADIES_SAMPLE] = _sampled_offset[0][LADIES_SAMPLE];
}

void asic::common_sampled_nodes()
{
  for (int i = 0; i < GCN_LAYERS; ++i)
  {
    // get intersection between _sampled_nodes[x][i] and _sampled_nodes[x][i+1]
    int intersection = 0;
    for (int j = 0; j < LADIES_SAMPLE; ++j)
    {
      for (int k = 0; k < LADIES_SAMPLE; ++k)
      {
        if (_sampled_nodes[i][j] == _sampled_nodes[i + 1][k])
        {
          intersection++;
        }
      }
    }

    cout << "Number of intersections b/w layer i and i+1: " << intersection << endl;
  }
}

// TODO: Currently the latency is 1, can we add a flag that varies this latency?
void asic::scalability_test(int scale, int delay)
{
  assert(_config->_algo != cf && "scalability test not implemented for cf");
  delay = 8;
  int cycles = 0, num_edges = 0;
  DTYPE old_dist = 0;
  int e = 0;
  uint64_t total_edges = 0;
  task_entry new_task(_src_vid, _offset[_src_vid]);

  DTYPE prio = 0;
  auto it = _asic_cores[0]->_task_queue[0][0].find(prio);
  if (it == _asic_cores[0]->_task_queue[0][0].end())
  {
    list<task_entry> a;
    a.push_back(new_task);
    _asic_cores[0]->_task_queue[0][0].insert(make_pair(prio, a));
  }
  else
  {
    it->second.push_back(new_task);
  }

  _task_ctrl->_present_in_local_queue[0].reset();
#if ENTRY_ABORT == 1
  _task_ctrl->_present_in_local_queue[0].set(_src_vid);
#endif

  // insert_local_task(0,0,0,_scratch[_src_vid],new_task);

  queue<task_entry> new_tasks[delay + 1];
  int cur_ptr = 0;
  int pending_tasks = 0;

  while (!_asic_cores[0]->_task_queue[0][0].empty() || pending_tasks > 0)
  {
    // cout << "Size of task queue: " << _asic_cores[0]->_task_queue[0][0].size() << endl;
    num_edges = 0;
    cycles++;
    // if(cycles%1000000==0) cout << "Cycles: " << cycles << " with edges: " << total_edges << endl;

    while (num_edges < scale && !_asic_cores[0]->_task_queue[0][0].empty())
    {

      auto it = _asic_cores[0]->_task_queue[0][0].begin();
      task_entry cur_task = (it->second).front();
      if (_config->_task_sched == abcd)
      {
        assert(cur_task.delta_prio == it->first && "correct priority");
      }
#if ENTRY_ABORT == 1
      if (cur_task.start_offset == _offset[cur_task.vid])
      {
        _task_ctrl->_present_in_local_queue[0].reset(cur_task.vid);
      }
#endif

      for (e = cur_task.start_offset; e < _offset[cur_task.vid + 1] && num_edges < scale; ++e)
      {

        ++num_edges;
        ++total_edges;
        int dst_id = _neighbor[e].dst_id;

        old_dist = _scratch[dst_id];
        DTYPE new_dist = process(_neighbor[e].wgt, _scratch[cur_task.vid]);
        _scratch[dst_id] = reduce(_scratch[dst_id], new_dist);

        // cout << "old dist: " << old_dist << " new dist: " << new_dist << endl;
        // this spawn should at least be used in the next cycle? but it is still applied in the next cycle
        if (should_spawn_task(old_dist, new_dist) && _task_ctrl->_present_in_local_queue[0].test(dst_id) == 0)
        {
          task_entry new_task(dst_id, _offset[dst_id]);
          new_task.delta_prio = abs(old_dist - _scratch[dst_id]);
          new_tasks[(cur_ptr + delay) % (delay + 1)].push(new_task);
          pending_tasks++;
        }
      }

      if (e == _offset[cur_task.vid + 1])
      {
        (it->second).pop_front();
        if ((it->second).empty())
        {
          _asic_cores[0]->_task_queue[0][0].erase(it->first);
        }
      }
      else
      {
        assert(num_edges == scale);
        (it->second).front().start_offset = e;
      }
    }
    // cout << "Tasks produced this cycle: " << new_tasks.size() << endl;
    // I can also learn the impact of latency here...
    while (!new_tasks[cur_ptr].empty())
    {
      pending_tasks--;
      task_entry new_task = new_tasks[cur_ptr].front();
#if ENTRY_ABORT == 1
      _task_ctrl->_present_in_local_queue[0].set(new_task.vid);
#endif
      DTYPE prio = _scratch[new_task.vid];
      if (_config->_task_sched == abcd)
      {
        prio = new_task.delta_prio;
      }
      auto it = _asic_cores[0]->_task_queue[0][0].find(prio);
      if (it == _asic_cores[0]->_task_queue[0][0].end())
      {
        list<task_entry> a;
        a.push_back(new_task);
        _asic_cores[0]->_task_queue[0][0].insert(make_pair(prio, a));
      }
      else
      {
        it->second.push_back(new_task);
      }
      new_tasks[cur_ptr].pop();
    }
    cur_ptr = (cur_ptr + 1) % (delay + 1);
  }
  cout << "Cycles: " << cycles << " work-eff: " << (total_edges / (double)E) << " with scale: " << scale << " and delay: " << delay << endl;
  for (int i = 0; i < _graph_vertices; ++i)
    _vertex_data[i] = _scratch[i];
  correctness_check();
}

// What happens at barriers of synchronous implementations...
bool asic::should_cf_spawn_task(int vid)
{
  DTYPE error = 0; // average difference (x-y)

  for (int f = 0; f < FEAT_LEN; ++f)
  {
    DTYPE err = (gamma * _scratch_vec[f][vid]);
    if (_config->_blocked_async)
    {
      err = _scratch_vec[f][vid] - _vertex_data_vec[f][vid];
    }
    else
    {
      assert(_config->is_sync() && "shouldn't come here without sync");
      _scratch_vec[f][vid] = 0;
    }
    _vertex_data_vec[f][vid] += err;
    error += abs(err);
  }
  // cout << "error of vid: " << i << " is: " << error << endl;
  return error > epsilon;
}

// we should update the priority when it gets full, not empty
int task_controller::get_cache_hit_aware_priority(int core_id, bool copy_already_present, int pointer_into_batch, int leaf_id, int batch_width, int num_in_pointer)
{
  return 0;
  // if matrix-multiply task...
  int priority = 0;
  if (pointer_into_batch >= 0)
  {
    if (_asic->_config->_algo == gcn)
      return 0;
    priority = _asic->_mult_cores[core_id]->_prev_priority; // leaf_id; // prioritized by leaf id as in HATS
    ++_asic->_mult_cores[core_id]->_prev_priority;
    if (_asic->_config->_hybrid_hats_cache_hit == 0)
    {
      // TODO: get how much is the current bucket full
      // priority = (_asic->_mult_cores[core_id]->_batch_width-_asic->_mult_cores[core_id]->_num_batched_tasks_per_index[pointer_into_batch]);
      // FIXME: this should ideally use these from the caller
      priority = batch_width - num_in_pointer;
      if (copy_already_present && priority > 0)
      {
        priority -= 1;
      }
      return priority; // for now
    }
  }

  uint64_t line_addr = 0;
  if (_asic->_config->_algo == gcn)
  {
    line_addr = (leaf_id * message_size);
  }
  else
  {
    line_addr = _asic->_mem_ctrl->_weight_offset + (leaf_id * _asic->_mult_cores[core_id]->num_cache_lines_per_leaf * line_size);
  }
  line_addr /= line_size;
  line_addr *= line_size;

  bool is_hit = _asic->_mem_ctrl->is_cache_hit(core_id, line_addr, ACCESS_LOAD, true, 0);
  priority = (1 - is_hit);
  // priority=0; // priority among hit requests is the most critical
  priority *= 10; // too high for misses (priortize based on the size of outstanding queue)
  /*if(_asic->_mem_ctrl->_outstanding_mem_req.size()>150) {
     priority *= 10; // too high for misses (priortize based on the size of outstanding queue)
   } else {
     priority = 0;
   }*/

  // TODO: not sure if prioritize forward and backward...
  // if coalesced, kind-of hits...find victim from a cache? (for every insertion/deletion into SRAM: update the status of the cache line)
  if (priority == 0)
  { // ie. cache hit (mostly are misses so can't do anything?)
    // new line is least likely to be evicted; hence prioritized the most (its stack position is 0)
    if (_asic->_config->_banked == 1)
    {
      priority = _asic->_mem_ctrl->_banked_l2_cache[core_id]->GetLRUBits(line_addr, 0);
    }
    else
    {
      priority = _asic->_mem_ctrl->_l2_cache->GetLRUBits(line_addr, 0);
    }
  }
  else
  {
    auto it = _asic->_mem_ctrl->_outstanding_mem_req.find(line_addr);
    if (it != _asic->_mem_ctrl->_outstanding_mem_req.end())
    {
      priority /= 2;
      ++_asic->_stats->_stats_coalesces_in_cache_hit;
    }
    else
    {
      ++_asic->_stats->_stats_misses_in_cache_hit;
    }
  }

  if (_asic->_config->_algo != gcn)
  {
    // int full_batch_factor = (_asic->_mult_cores[core_id]->_batch_width-_asic->_mult_cores[core_id]->_num_batched_tasks_per_index[pointer_into_batch]);
    int full_batch_factor = batch_width - num_in_pointer;
    // change buckets of the lru range. for 1->4, reduce by 2
    if (full_batch_factor < 1)
      priority /= 2; // if full, prioritize these over hits
    // if hit, prioritize it. If miss, no point of prioritizing it
    // priority *= ((full_batch_factor+1)*0.5); // need an integer priority
  }
  ++_asic->_stats->_stats_accesses_in_cache_hit;
  return priority;
}

int task_controller::get_live_fine_tasks()
{
  int agg_oflow_entries = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    agg_oflow_entries += _asic->_asic_cores[i]->_fifo_task_queue.size();
    for (int tqid = 0; tqid < NUM_TQ_PER_CORE; ++tqid)
    {
      agg_oflow_entries += (TASK_QUEUE_SIZE - _remaining_task_entries[i][tqid]);
    }
  }

  if (_asic->_config->_algo == gcn)
  {
    for (int i = 0; i < core_cnt; ++i)
    {
      agg_oflow_entries += _asic->_asic_cores[i]->_prefetch_process[0].size();
    }
  }

  return agg_oflow_entries;
}

int task_controller::get_live_coarse_tasks()
{
  int mult_oflow_entries = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    mult_oflow_entries += _asic->_mult_cores[i]->_local_coarse_task_queue.size();
    mult_oflow_entries += (COARSE_TASK_QUEUE_SIZE - _remaining_local_coarse_task_entries[i]);
  }
  if (_asic->_config->_algo == gcn)
  {
    for (int i = 0; i < core_cnt; ++i)
    {
      for (int j = 0; j < 2; ++j)
      {
        mult_oflow_entries += _asic->_mult_cores[i]->_lsq_process[j].size();
      }
    }
  }

  return mult_oflow_entries;
}

void task_controller::central_task_schedule()
{
  // if(_cur_cycle%1000==0 && _cur_cycle!=0) {
  if (_asic->_cur_cycle % 500 == 0 && _asic->_cur_cycle != 0)
  {

    /*
   for(int i=0; i<core_cnt; ++i) {
     auto it = _asic_cores[i]->_task_queue[0].rbegin();
     if(it!=_asic_cores[i]->_task_queue[0].rend()) { // should have reasonable gap (may increase cycle range?)
       auto it2 = _asic_cores[i]->_task_queue[0].begin();
       cout << "i: " << i << " start: " << it->second.size() << " end: " << it2->second.size() << endl;
     }
   }*/

    int count = 0;
    _avg_timestamp = 0;
    // check the maximum timestamp of each task queues
    // for uniform graph

    // average of the maximums
    int min_timestamp = MAX_TIMESTAMP;
    int max_timestamp = 0; // MAX_TIMESTAMP;

    for (int i = 0; i < core_cnt; ++i)
    { // TODO: logically should add extra dimension of number of task queues
      for (int j = 0; j < NUM_TQ_PER_CORE; ++j)
      {
        if (_asic->_asic_cores[i]->_task_queue[j][0].size() < 10)
          continue;
        auto it = _asic->_asic_cores[i]->_task_queue[j][0].rbegin();
        if (it != _asic->_asic_cores[i]->_task_queue[j][0].rend())
        { // should have reasonable gap (may increase cycle range?)
          count++;
          // _avg_timestamp += it->first;
          if (it->first < min_timestamp)
            min_timestamp = it->first;
          // auto it2 = _asic_cores[i]->_task_queue[0].begin();
          // _avg_timestamp += (it->first+it2->first)/2;
          // if(it2->first>max_timestamp) max_timestamp=it->first;
          // cout << "timestamp at core i: " << i << " is: " << it2->first << " " << it->first << " and number: " << it2->second.size() << endl;
        }
      }
    }

    // if(min_timestamp>10 || _stats->_stat_tot_finished_edges>1.5*E)
    //if(max_timestamp>10)
    if (min_timestamp > 10)
      _avg_timestamp = 1.01 * min_timestamp;
    // _avg_timestamp = 1.02*max_timestamp;

    // if(count!=0) _avg_timestamp = _avg_timestamp*1.2/count;
    else
      _avg_timestamp = MAX_TIMESTAMP;

    // if(_cur_cycle==20000)
    // cout << "Timestamp this cycle: " << _avg_timestamp << endl;
    // IMPLE: for power law graphs
    /*
   for(int i=0; i<core_cnt; ++i) {
     auto it = _asic_cores[i]->_task_queue[0].begin();
     if(it!=_asic_cores[i]->_task_queue[0].end()) { // should have reasonable gap (may increase cycle range?)
       auto it2 = _asic_cores[i]->_task_queue[0].begin();
       cout << "timestamp at core i: " << i << " is: " << it2->first << " " << it->first << endl;

       count++;
       if(it->first>_avg_timestamp) _avg_timestamp = it->first;
     }
   }
   count=0;
   if(count==0) _avg_timestamp = MAX_TIMESTAMP;
   // else _avg_timestamp += 1;
   else _avg_timestamp *= 1.05;
   */
  }
}

void task_controller::insert_global_task(DTYPE timestamp, task_entry cur_task)
{
  ++_asic->_stats->_stat_tot_created_edge_tasks;

#if GRAPHPULSE == 1 && CENTRAL_FIFO == 0
  // priority=vid%512;
  // priority=cur_task.vid%64; // this did not prioritize small VID
  int vertices_in_a_row = V / 256;
  int rowid = cur_task.vid / vertices_in_a_row;
  int cycle_assum_one_row_per_cycle = (_asic->_cur_cycle / 256) * 256;
  timestamp = (cycle_assum_one_row_per_cycle + rowid);
#endif
  // if(cur_task.parent_id==168792) {
  // if(cur_task.tid==168792) {
  /*if(cur_task.tid==241377) {
    cout << "came to insert task corresponding to the neighbor and parent id: " << cur_task.parent_id << "\n";
    // exit(0);
  }*/

#if PR == 1 && ENTRY_ABORT == 1 && CF == 0
  if (cur_task.dist == 0)
    return;
#endif

  if (_asic->_config->_fifo)
  {
    timestamp = 0;
  }
  // cout << "Came to insert task in the global queue\n";
  // cur_task.dist=timestamp;

  /*if(cur_task.vid==493405) {
     cout << "distance in scratch2: " << _scratch[cur_task.vid] << " and the new distance: " << cur_task.dist << " and sent timestamp: " << timestamp << endl;
   }*/

#if SGU == 1 && ABORT == 1
  // delete any other task with same vid
  // want to scan whole whole task queue
  bool came_inside = false;
  for (auto it2 = _global_task_queue.begin(); it2 != _global_task_queue.end();)
  {
    came_inside = false;
    for (auto it3 = (*it2).second.begin(); it3 != (*it2).second.end();)
    {
      if ((*it3).vid == cur_task.vid)
      {

#if PR == 1
        // FIXME: this is possible now, we can do this...
        // cout << "old residual: " << (*it3).dist << " new residual: " << cur_task.dist << endl;
        // assert((*it3).dist < cur_task.dist && "Should have already detected higher residual");
#else
        assert((*it3).dist > cur_task.dist && "Should have already detected lower distance");
#endif
        _asic->_task_ctrl->_present_in_queue.reset(cur_task.vid);
        _asic->_stats->_stat_deleted_task_entry++;
        it3 = (*it2).second.erase(it3);
        if ((*it2).second.empty())
        { // if nothing left in list
          came_inside = true;
          it2 = _global_task_queue.erase(it2);
          break;
        }
      }
      else
      {
        ++it3;
      }
    }
    if (!came_inside)
    {
      ++it2;
    }
  }

#endif

#if ENTRY_ABORT == 1
  if (_present_in_queue.test(cur_task.vid) == 1)
    return;
  _present_in_queue.set(cur_task.vid); // assuming no one else will stop it now
#endif

  // cout << "Num mispec for the current task is: " << _num_spec[cur_task.vid] << endl;
  // int priority_order = timestamp*_num_spec[cur_task.vid];
  // int priority_order = timestamp+3*_num_spec[cur_task.vid];
  // int scalar = _num_spec[cur_task.vid]/50;
  // for new york road
  // int scalar = _num_spec[cur_task.vid]/5;
  // linear-mapping -- not good
  // scalar = _num_spec[cur_task.vid] < 50 ? 0 : 3; // bad
  // scalar = _num_spec[cur_task.vid]/300; // doesn't work, 300 is where it peaks
  // scalar = max(_num_spec[cur_task.vid],50);
  // int func = 1+(scalar*0.1);
  // func=1;
  // func = scalar*_num_spec[cur_task.vid];
  // func = 1+pow(scalar,2); // cube doesn't help
  // scalar = _num_spec[cur_task.vid]/10;
  // When I decrease this factor, it comes into play for many vertices which
  // -- timestamp is mostly good (Required only special cases)
  // we don't want
  // TODO: what hop count can I use, it has multiple edges?
  // Somehow the performance of scheduling policy is limited by the local
  // window?...

  /*
   int scalar = _num_spec[cur_task.vid]/50;
   scalar = _num_spec[cur_task.vid]/40; // 20;
   double func = 1+pow(scalar,2)/20;
   func = 1+pow(scalar,2);


   if(_num_spec[cur_task.vid]<40 && _num_spec[cur_task.vid]>20) {
     // range of multiple should be small
     // func = 1 + _num_spec[cur_task.vid]*0.05*0.05*0.25;
     func = 1.05;
     func = 1.03;
     // if greater than 1, give max*factor
   }
   int dist_factor = 0;
   if(core_cnt==16) dist_factor = pow(timestamp,1);
   else dist_factor = pow(timestamp,1.2);
   int priority_order = dist_factor*func; // *0.8;
   if(_num_spec[cur_task.vid]>40) { // we ignore own timestamp here?
     priority_order = get_middle_rank(_num_spec[cur_task.vid]/40);
   }

   // FOR FIFO
   if(!_global_task_queue.empty()) {
     auto it1 = _global_task_queue.rbegin();
     priority_order = 1+it1->first;
   } else {
     priority_order = timestamp;
   }
   priority_order=0; // FIFO
   */

  DTYPE priority_order = timestamp; // FIFO

  // priority_order = timestamp;
  // priority_order = pow(timestamp,2)/1000;
  // priority_order = timestamp + _num_spec[cur_task.vid]*8;

  // priority_order = (timestamp*_num_spec[cur_task.vid])/50;
  // for florida road -- direct multiple doesn't have an impact on priority
  // func = exp(_num_spec[cur_task.vid]);
  // priority_order = timestamp*func*0.2;

  /*
   // CHECK IF ALL IMCOMING EDGES ARE DONE
   bool all_done=true;
   for(int i=0; i<(int)_incoming_edgeid[cur_task.vid].size(); ++i) {
     int id = _incoming_edgeid[cur_task.vid][i];
     // if(!_neighbor[id].done) {
     // if(_stats->_stat_extra_finished_edges[id]<_mapping[cur_task.vid]) {
     // By this, I basically want to see the depth...
     // wouldn't this be good for bfs?
     // if(_stats->_stat_extra_finished_edges[id]<get_local_scratch_id(cur_task.vid)) {
     // if(_stats->_stat_extra_fnished_edges[id]<(0.6*_mapping_bfs[cur_task.vid]*core_cnt/_graph_vertices)) {
     if(_stats->_stat_extra_finished_edges[id]<(0.1*_mapping_bfs[cur_task.vid]*core_cnt/_graph_vertices)) {
     // if(_stats->_stat_extra_finished_edges[id]<4) {
     // if(!_neighbor[id].done) {
       all_done = false;
       break;
     }
   }
   int add_factor = 2*(_first_updated_dist[cur_task.vid]-cur_task.dist);
   if(all_done) {
     priority_order = dist_factor;
     // priority_order = dist_factor*1*func;
     // priority_order = dist_factor + add_factor;
   }
   else {
     // priority_order = dist_factor*1.5;
     // priority_order = dist_factor*func;
     priority_order = dist_factor*_num_spec[cur_task.vid]/5;
     // if(!_global_task_queue.empty()) {
     //   auto it = _global_task_queue.rbegin();
     //   int cur_last = it->first;
     //   priority_order = cur_last+1;
     // }
     _stats->_stat_first_time++;
  }
   */

  /*if(!_global_task_queue.empty()) {
        auto it1 = _global_task_queue.rbegin();
        auto it2 = _global_task_queue.begin();
        int cur_last = it1->first;
        int cur_start = it2->first;
        cout << "Current range of distances: " << cur_start << " " << cur_last << " ratio: " << cur_last/(double)cur_start << endl;
      }*/

  // this is unpredictable, sometimes less sometimes more (maybe just depends
  // on the weight of edges)
  // cout << "Current distance: " << dist_factor << " and the addition: " << add_factor << endl;
  // priority_order = dist_factor+(add_factor); // this doesn't work well!

  // DECIDING PRIORITY ORDER IF IT'S SOURCE VERTEX IS COMMITTED
#if ESPRESSO == 1
  // Oh not this, current lowest distance -- isn't that prioritized by default
  // in distance-order scheduling? they just meant no need to check for
  // conflicts?
  // if(_is_visited[cur_task.parent_vid]) {
  /*if(cur_task.dist == ) {
     cout << "Came inside this condition\n";
     priority_order=1;
   }*/
#endif

  auto it = _global_task_queue.find(priority_order);
  if (it == _global_task_queue.end())
  {
    // queue<task_entry> a; a.push(cur_task);
    list<task_entry> a;
    a.push_back(cur_task);
    _global_task_queue.insert(make_pair(priority_order, a));
  }
  else
  {
    it->second.push_back(cur_task);
    // it->second.push(cur_task);
  }
}

int task_controller::chose_nearby_core(int core_id)
{
  int min_task = MAX_TIMESTAMP;
  int min_core = -1;
  // for a core, these should be 3 other cores
  int subblock = (core_id / 8) * 2 + (core_id % 4) / 2;
  // Core 1: subblock, subblock
  // TODO: choose in the nearby circle (
  for (int r = 0; r < num_rows; ++r)
  {
    int c = _asic->_network->_core_net_map[subblock][r];
    int num_tasks = _asic->_asic_cores[c]->_task_queue[0][0].size();
#if NUM_TQ_PER_CORE > 1
    num_tasks += _asic->_asic_cores[c]->_task_queue[1][0].size();
#endif
    num_tasks += _asic->_asic_cores[c]->_fifo_task_queue.size();
    if (num_tasks < min_task)
    {
      min_task = num_tasks;
      min_core = c;
    }
  }
  return min_core;
}

int task_controller::chose_smallest_core()
{
  int min_task = MAX_TIMESTAMP;
  int min_core = -1;
  for (int c = 0; c < core_cnt; ++c)
  {
    int num_tasks = _asic->_asic_cores[c]->_task_queue[0][0].size();
#if NUM_TQ_PER_CORE > 1
    num_tasks += _asic->_asic_cores[c]->_task_queue[1][0].size();
#endif
    num_tasks += _asic->_asic_cores[c]->_fifo_task_queue.size();
    if (num_tasks < min_task)
    {
      min_task = num_tasks;
      min_core = c;
    }
  }
  return min_core;
}

bool task_controller::can_pull_from_overflow(int core_id)
{
#if DYN_LOAD_BAL == 0 // 1 // DYN_LOAD_BAL==0
  return _asic->_asic_cores[core_id]->_fifo_task_queue.size() > 0;
#else
  // TODO: Let's check all nearby cores
  bool flag = false;
  int base_core = core_id / 4;
  for (int c = base_core * 4; c < (base_core * 4 + 4); ++c)
  {
    flag |= (_asic->_asic_cores[c]->_fifo_task_queue.size() > 0);
  }
  return flag;
#endif
}

void task_controller::pull_tasks_from_fifo_to_tq(int core_id, int tqid)
{
  int source_overflow_buffer = core_id;
  // TODO: for dynamic load balance, it should check from one of these cores...
  // if one core's cb is full, it will not call for freeing from fifo task queue
#if DYN_LOAD_BAL == 1 // 0
  // Now chose one among the other three
  if (_asic->_asic_cores[core_id]->_fifo_task_queue.empty())
  {
    int base_core = (core_id / 4);
    for (int c = base_core * 4; c < (base_core * 4 + 4); ++c)
    {
      if (_asic->_asic_cores[c]->_fifo_task_queue.size() > 0)
      {
        source_overflow_buffer = c;
      }
    }
  }
  /*if(1) { // push to other core if that is empty
     // core_id = chose_smallest_core();
    // What is the task issue throughput required; do we require to fulfil everytime it asks?
    // The number of tasks to be pushed depends on the expected time for each tasks
    // If we need to send some tasks to each of the cores to get close to filled task queue;
    // can we prioritize for locality and priority ordering?
    // FIXME 1: this should be called periodically from 256-sized task queue
    // FIXME 2: or should we just push when asked but from a common buffer
     core_id = chose_nearby_core(core_id);
     _tasks_moved++;
   }*/
#endif
  while (!_asic->_asic_cores[source_overflow_buffer]->_fifo_task_queue.empty() && _remaining_task_entries[core_id][tqid] > HIGH_PRIO_RESERVE)
  {
    if (source_overflow_buffer != core_id)
    {
      _tasks_moved++;
    }
    _tasks_inserted++;
    task_entry pending_tuple = _asic->_asic_cores[source_overflow_buffer]->_fifo_task_queue.front();
    _asic->_asic_cores[source_overflow_buffer]->_fifo_task_queue.pop();
    _present_in_local_queue[source_overflow_buffer].reset(pending_tuple.vid);
    DTYPE priority = 0;
    if (!_asic->_config->is_vector())
    {
      priority = _asic->apply(_asic->_scratch[pending_tuple.vid], pending_tuple.vid);
      if (_asic->_config->_algo != pr)
      {
        pending_tuple.dist = _asic->_scratch[pending_tuple.vid]; // priority;
      }
    }
#if CACHE_HIT_AWARE_SCHED == 1
    // int paddr = _asic->_mem_ctrl->_vertex_offset + pending_tuple.vid * message_size;
    priority = calc_cache_hit_priority(pending_tuple.vid);
#endif
    insert_local_task(tqid, 0, core_id, priority, pending_tuple, true);
    if (pending_tuple.second_buffer)
    {
      --_asic->_stats->_stat_tot_created_edge_second_buffer_tasks;
    }
    else
    {
      --_asic->_stats->_stat_tot_created_edge_tasks;
    }
  }
}

DTYPE task_controller::calc_cache_hit_priority(int vid)
{
  DTYPE priority = 0;
  // FIXME: make sure that is the same address being accessed in consume_lsq??
  uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (vid * message_size * FEAT_LEN);
  assert(_asic->_config->_working_cache && "only applicable when cache is true");
  assert(_asic->_config->_fifo == 0 && "priority scheduling makes sense when no fifo");
  bool l2hit = _asic->_mem_ctrl->is_cache_hit(_asic->_scratch_ctrl->get_local_scratch_id(vid), paddr, ACCESS_LOAD, true, 0);
  // cout << "Cache-hit checking address: " << paddr << endl;
  if (!l2hit)
  { // FIXME: How is this only 16?
    priority = 10;
    ++_asic->_stats->_stat_delayed_cache_hit_tasks;
  }
  else
  {
    cout << "Looked up paddr: " << paddr << " is hit: " << l2hit << endl;
    ++_asic->_stats->_stat_allowed_cache_hit_tasks;
  }
  return priority;
}

// If pull mode, activate all the dependent vertices, otherwise only its
// current vertex
void task_controller::insert_new_task(int tqid, int lane_id, int core_id, DTYPE priority, task_entry cur_task)
{
  // if(_asic->_config->_prac==0 && _asic->_config->_algo==bfs) {
  //   int degree = _asic->_offset[cur_task.vid+1] - _asic->_offset[cur_task.vid];
  //   if(degree!=0) {
  //     priority = 1000/degree;
  //   }
  // }
  /*
 // TODO: define a function for moving tasks from fifo to the TQ and put this logic there
#if DYN_LOAD_BAL==1
  // Send the task to a new core (probably from the end) 
  // I have been deriving destination core from addr_id -- that's okay. Only this should not be involved in entry_abort.
  // Otherwise, does it assume anything while sending and receiving request?
  // its req_core_id should be same either way. (maybe CB entry is different at
  // different sides)
  // 67% of the tasks are being copied, how to manage this?? (should try to copy where tasks are less at the destination core)
  if(_asic->_asic_cores[core_id]->_fifo_task_queue.size()>500) {
    _tasks_moved++;
    // cout << "Came to allocate more memory\n";
    // core_id = chose_smallest_core(); // core_cnt-core_id-1; // will need to remove some asserts
    core_id = chose_nearby_core(core_id); // core_cnt-core_id-1; // will need to remove some asserts
  }
  _tasks_inserted++;
#endif
*/

  if (_asic->_config->_pull_mode)
  {
    // FIXME: If it is re-inserting a task for some reason, it should be local,
    // this logic is true when we activating a new vertex
    DTYPE priority = 0;
    if (!_asic->_config->is_vector() && !_asic->_config->_fifo)
    {
      priority = _asic->apply(_asic->_scratch[cur_task.vid], cur_task.vid);
    }
    if (_asic->_config->_pr == 0)
    {
      assert(_asic->_scratch[cur_task.vid] < MAX_TIMESTAMP && "a vertex should be activated only after its value is updated");
    }
    for (int e = _asic->_csr_offset[cur_task.vid]; e < _asic->_csr_offset[cur_task.vid + 1]; ++e)
    {
      int vid = _asic->_csr_neighbor[e].dst_id;
      task_entry new_task(vid, _asic->_offset[vid]);
      int allotted_core = _asic->_scratch_ctrl->get_local_scratch_id(vid);
      if (_present_in_local_queue[allotted_core].test(vid) == 0)
      { // barrier tasks are also considered online here
        _asic->_stats->_stat_online_tasks++;
        insert_local_task(tqid, 0, allotted_core, priority, new_task);
      }
    }
  }
  else
  {
    if (_asic->_config->_work_stealing)
    {
      // cout << "Came here to insert new task for vid: " << cur_task.vid << endl;
      insert_local_task(tqid, lane_id, core_id, priority, cur_task);
    }
    else
    {
      insert_global_task(priority, cur_task);
    }
  }
}

void asic::insert_global_bank(int bank_id, DTYPE priority, red_tuple cur_tuple)
{
#if PRIO_XBAR == 1
  auto it = _bank_queues[bank_id].find(priority);
  if (it == _bank_queues[bank_id].end())
  {
    list<red_tuple> a;
    a.push_back(cur_tuple); // front(cur_task);
    _bank_queues[bank_id].insert(make_pair(priority, a));
  }
  else
  {
    it->second.push_back(cur_tuple);
  }
#else
  _bank_queues[bank_id].push_back(cur_tuple);
#endif
}

void asic::generate_memory_accesses(default_random_engine generator, normal_distribution<double> distribution)
{

  // FIXME: if there are enough completion buffer entries?
  for (int c = 0; c < core_cnt; ++c)
  {
    uint64_t paddr = distribution(generator); // %1000; // want to skewed for natural data
    paddr *= line_size;
    cout << "generated address: " << paddr; // << endl;
    pref_tuple cur_tuple;
    bool l2hit = _mem_ctrl->is_cache_hit(c, paddr, ACCESS_LOAD, true, 0);
    if (l2hit)
    {
      // _asic_cores[c]->_priority_hit_gcn_updates.push_back(cur_tuple);
      cout << " hits at cycle: " << _cur_cycle << endl;
      _mem_ctrl->_l2hits++;
    }
    else
    {
      // _asic_cores[c]->_miss_gcn_updates.push_back(cur_tuple);
      cout << " sent a memory request at cycle: " << _cur_cycle; // << endl;
      bool sent_request = _mem_ctrl->send_mem_request(false, paddr, cur_tuple, VERTEX_SID);
      if (!sent_request)
      {
        cout << " coalesced\n";
        _mem_ctrl->_l2hits++;
        _mem_ctrl->_l2coalesces++;
      }
      else
      {
        cout << " miss\n";
      }
      _stats->_stat_pending_cache_misses++;
      _mem_ctrl->_num_pending_cache_reqs++;
    }
    _mem_ctrl->_l2accesses++;
  }
}

void task_controller::insert_local_task(int tqid, int lane_id, int core_id, DTYPE priority, task_entry cur_task, bool from_fifo)
{
  // reset should done only at first insertion, not from fifo
  if (!from_fifo && _asic->_config->_algo == pr)
  {
    // if(cur_task.dist==0) return; // dangling vertices
    /*if(_asic->_config->is_sync()) {
      assert(_asic->_vertex_data[cur_task.vid]==_asic->_scratch[cur_task.vid] && "at barriers, both copies should match");
    }*/
    // Let me just use updated scratch..
    cur_task.dist = _asic->_scratch[cur_task.vid]; // uses the distance at enqueue, hence same as sync...
    if (_asic->_config->_pull_mode == 0)
    {
      _asic->_scratch[cur_task.vid] = 0;
    }
  }
  if (_asic->_config->_algo == pr)
  {
    int degree = _asic->_offset[cur_task.vid + 1] - cur_task.start_offset;
    if (cur_task.dist == 0)
    {
      // _asic->_stats->_stat_tot_finished_edges+=degree;
      return;
    }
    if (degree == 0)
      return;
  }

  /*if(priority==-1 && cur_task.vid==81) {
    cout << "Break to see what happens for pending taski at cycle: " << _asic->_cur_cycle << " with start offset: " << cur_task.start_offset << "\n";
  }
  if(_asic->_cur_cycle==775 && priority==-1 && cur_task.vid==81) {
    cout << "BREAK TO DEBUG\n";
  }*/
#if FIFO == 2
  priority = cur_task.vid;
#endif
  if (_asic->_config->_domain == tree && _asic->_config->_fifo == 0)
  {
    priority = _asic->kdtree_depth + 1 - _asic->_scratch[cur_task.vid]; // should be PRAC=1??
    // TODO: distribute to a random core
    if (_asic->_config->_hats == 1)
    {
      core_id = rand() % core_cnt; // we want a lot of ready tasks
    }
  }

  if (cur_task.second_buffer)
  {
    ++_asic->_stats->_stat_tot_created_edge_second_buffer_tasks;
    // cout << "Created second buffer task at cycle: " << _asic->_cur_cycle << " for vid: " << cur_task.vid << endl;
  }
  else
  {
    ++_asic->_stats->_stat_tot_created_edge_tasks;
  }

  if (_asic->_config->_cache_hit_aware_sched)
  {
    priority = calc_cache_hit_priority(cur_task.vid);
  }

#if GRAPHPULSE == 1 && FIFO == 0
  // priority=vid%512;
  // priority=cur_task.vid%64; // this did not prioritize small VID
  int vertices_in_a_row = V / 256;
  int rowid = cur_task.vid / vertices_in_a_row;
  int cycle_assum_one_row_per_cycle = (_asic->_cur_cycle / 256) * 256;
  priority = cycle_assum_one_row_per_cycle + rowid;
#endif
  /*if(cur_task.vid==789187) {
    cout << "BREAK, it comes to insert this task, somehow lost it\n";
  }
  if(cur_task.vid==820735) { // this is a cycle: some problem with this?
    cout << "BREAK, its parent task is inserted\n";
  }*/
#if REUSE_STATS == 1 && SLICE_COUNT > 1
  for (int e = cur_task.start_offset; e < _asic->_offset[cur_task.vid + 1]; ++e)
  {
    int dst_slice = _asic->_scratch_ctrl->get_slice_num(_asic->_neighbor[e].dst_id);
    if (dst_slice != _asic->_current_slice)
    {
      // this is a boundary task
      if (!_asic->_stats->_is_source_done[dst_slice].test(cur_task.vid))
      {
        // cout << "updating for source id: " << cur_task.vid << " dst slice: " << dst_slice << endl;
        _asic->_stats->_stat_source_copy_vertex_count[dst_slice]++;
        _asic->_stats->_is_source_done[dst_slice].set(cur_task.vid);
      }
    }
  }
#endif

  // cout << "Inserting task for vertex: " << cur_task.vid << " with degree: " << (_offset[cur_task.vid+1]-_offset[cur_task.vid]) << " with first neighbor: " << _neighbor[_offset[cur_task.vid]].dst_id << endl;

#if SWARM == 1
  lane_id = rand() % LANE_WIDTH;
#endif

#if PR == 1 && ENTRY_ABORT == 1 && CF == 0
  if (cur_task.dist == 0)
    return;
#endif

  if (_asic->_config->_fifo)
  {
    if (!_asic->_switched_async)
    {
      if (priority != -1)
      {
        priority = 0;
      }
      /*if(priority!=0 && _asic->_config->_pull_mode) {
        priority=1; // Let's keep this as the constant priority
      } else {
        priority=0;
      }*/
    }
  }
  // #if SGU == 1 && PRAC==0 && FIFO==0 && ABORT==1// delete any other task with same vid
  bool abort = false;
#if SGU == 1 && FIFO == 0 && ABORT == 1 // delete any other task with same vid
  abort = true;
#endif
  if (_asic->_switched_async)
    abort = true;
  if (abort)
  {
    // want to scan whole whole task queue
    // Oh, cool it is correct only 1 entry per timestamp
    bool came_inside = false;
    for (int tq_id = 0; tq_id < NUM_TQ_PER_CORE; ++tq_id)
    {
      for (auto it2 = _asic->_asic_cores[core_id]->_task_queue[tq_id][lane_id].begin(); it2 != _asic->_asic_cores[core_id]->_task_queue[tq_id][lane_id].end();)
      {
        came_inside = false;
        for (auto it3 = (*it2).second.begin(); it3 != (*it2).second.end();)
        {
          if ((*it3).vid == cur_task.vid)
          {

#if PR == 1
            // residual should also decrease like distance
            // assert((*it3).dist < cur_task.dist && "Should have already detected higher residual");
#else
            // not true if things are coming from fifo
            if ((*it3).dist <= cur_task.dist)
            {
              // cout << "vid is: " << cur_task.vid << " old dist: " << (*it3).dist << " new dist: " << cur_task.dist << endl;
              return; // no need to do anything for the current task -- skip
            }
            assert((*it3).dist > cur_task.dist && "Should have already detected lower distance");
#endif
            _asic->_stats->_stat_deleted_task_entry++;
            _present_in_local_queue[core_id].reset(cur_task.vid);
            it3 = (*it2).second.erase(it3);
            _remaining_task_entries[core_id][tq_id]++;
            // if(it3==(*it2).second.end()) { // if nothing left in list
            if ((*it2).second.empty())
            { // if nothing left in list
              // cout << "Deleted whole list\n";
              came_inside = true;
              // if whole list empty, delete the timestamp entry
              it2 = _asic->_asic_cores[core_id]->_task_queue[tq_id][lane_id].erase(it2);
              break; // if list is empty, then break I guess -- there cannot be multiple instances of same vertex in task queue
            }
          }
          else
          {
            ++it3;
          }
        }
        if (!came_inside)
        {
          ++it2;
        }
      }
    }
  }

#if ENTRY_ABORT == 1
  // if abort=1, must be present in fifo (assume both are true together)
  // discard the current task, check in fifo queue if we want to pop that new
  // task
  if (priority != -1 && _present_in_local_queue[core_id].test(cur_task.vid) == 1)
  {
    // #if ABORT==1
    if (abort)
    {
      pull_tasks_from_fifo_to_tq(core_id, tqid);
    }
    _asic->_stats->_stat_deleted_task_entry++;
    return;
  }
  // now it will be pushed either in task queue or fifo
  if (priority != -1 || _asic->_config->is_sync())
  {
    _present_in_local_queue[core_id].set(cur_task.vid);
  }
#endif
  /*if(_remaining_task_entries[core_id]<TASK_QUEUE_SIZE) {
      if(_asic_cores[core_id]->_task_queue[0].empty()) {
        cout << "remaining entries: " << _remaining_task_entries[core_id] << endl;
      }
      assert(!_asic_cores[core_id]->_task_queue[0].empty());
    }*/
#if SWARM == 0
  // TODO: what if I move this to above setting that...or just remove
  // entry_abort?
  if (_remaining_task_entries[core_id][tqid] == 0)
  {
    // assert(cur_task.dist==priority); // not true in case of fifo
    assert(priority != -1 && "need to over prioritize, not push in FIFO");
    assert(cur_task.vid < _asic->_graph_vertices);
    _asic->_asic_cores[core_id]->_fifo_task_queue.push(cur_task);
    int fifo_size = _asic->_asic_cores[core_id]->_fifo_task_queue.size();
    if (fifo_size > _asic->_stats->_stat_max_fifo_storage)
    {
      _asic->_stats->_stat_max_fifo_storage = fifo_size;
    }
    _asic->_stats->_stat_overflow_tasks++;
    return;
  }

  // cout << " Output-inter remaining task entries: " << _remaining_task_entries[core_id] << " task queue empty: " << _asic_cores[core_id]->_task_queue[0].empty() << endl;

  // if only high priority entries are free
  if (_remaining_task_entries[core_id][tqid] < HIGH_PRIO_RESERVE)
  {
    auto itx = _asic->_asic_cores[core_id]->_task_queue[tqid][0].begin();
    // it should always reserve one entry for priority=-1
    if (itx->first <= priority && !(_remaining_task_entries[core_id][tqid] == 1 && priority != -1))
    {

      assert(priority != -1 && "need to over prioritize, not push in FIFO");
      assert(cur_task.vid < _asic->_graph_vertices);
      _asic->_asic_cores[core_id]->_fifo_task_queue.push(cur_task);
      int fifo_size = _asic->_asic_cores[core_id]->_fifo_task_queue.size();
      if (fifo_size > _asic->_stats->_stat_max_fifo_storage)
      {
        _asic->_stats->_stat_max_fifo_storage = fifo_size;
      }
      _asic->_stats->_stat_overflow_tasks++;
      return;
    }
    else
    {
      _asic->_stats->_stat_high_prio_task++;
    }
  }
#else
  assert(_remaining_task_entries[core_id][tqid] > 0 && "we assume infinite task queue size to model swarm");
#endif

  _remaining_task_entries[core_id][tqid]--;
  _asic->_stats->_stat_tot_vert_tasks++;

  auto it = _asic->_asic_cores[core_id]->_task_queue[tqid][lane_id].find(priority);
  if (it == _asic->_asic_cores[core_id]->_task_queue[tqid][lane_id].end())
  {
    list<task_entry> a;
    a.push_back(cur_task); // front(cur_task);
    _asic->_asic_cores[core_id]->_task_queue[tqid][lane_id].insert(make_pair(priority, a));
    /*if(priority==-1 && core_id==3 && cur_task.vid==81 && _asic->_cur_cycle==775) {
      auto it = _asic->_asic_cores[core_id]->_task_queue[tqid][lane_id].begin();
      cout << " priority: " << it->first << " vid: " << (it->second).front().vid << endl;
    }*/
  }
  else
  {
    assert(priority != -1 && "there should be only 1 pending -1 task at a time");
    it->second.push_back(cur_task);
  }
#if DIJKSTRA == 1 || SWARM == 1 || ESPRESSO == 1
  int left = _offset[cur_task.vid + 1] - cur_task.start_offset;
  meta_info x(left, left);
  // cout << "INSERTED INTO A META_INFO QUEUE for vid: " << cur_task.vid << " and tid: " << cur_task.tid << " at core: " << core_id << "\n";
  _asic_cores[core_id]->_meta_info_queue.insert(make_pair(cur_task.tid, x));
#endif
}

task_controller::task_controller(asic *host) : _asic(host)
{
  if (_asic->_config->_cache_hit_aware_sched == 1 && _asic->_config->_update_coalesce)
  {
    _present_in_miss_gcn_queue = (int *)malloc(V * sizeof(int));
    for (int i = 0; i < V; ++i)
    {
      _present_in_miss_gcn_queue[i] = 0;
    }
  }
  _present_in_queue.reset();
  for (int i = 0; i < SLICE_COUNT; ++i)
  {
    _check_worklist_duplicate[i].reset();
  }

  for (int c = 0; c < core_cnt; ++c)
  {
    _present_in_local_queue[c].reset();
  }
  _check_vertex_done_this_phase.reset();

  for (int i = 0; i < core_cnt; ++i)
  {
    for (int j = 0; j < NUM_TQ_PER_CORE; ++j)
    {
      _remaining_task_entries[i][j] = TASK_QUEUE_SIZE;
    }
    _remaining_local_coarse_task_entries[i] = COARSE_TASK_QUEUE_SIZE;
  }
  _remaining_central_coarse_task_entries = COARSE_TASK_QUEUE_SIZE;
}

int task_controller::find_min_coarse_task_core()
{
  int min_task_core = 0;
  int min_tasks_yet = 100000;
  int start_core = rand() % core_cnt;
  for (int c = start_core, i = 0; i < core_cnt; ++i, c = (c + 1) % core_cnt)
  {
    int tasks_in_core = _asic->_mult_cores[c]->_local_coarse_task_queue.size();
    tasks_in_core /= _asic->_mult_cores[c]->_data_parallel_throughput;
    tasks_in_core /= _asic->_coarse_alloc[c];
    if (tasks_in_core < min_tasks_yet && !is_local_coarse_grain_task_queue_full(c))
    {
      min_tasks_yet = tasks_in_core;
      min_task_core = c;
    }
  }
  return min_task_core;
}

int task_controller::find_max_coarse_task_core()
{
  int max_task_core = 0; // -1;
  int max_tasks_yet = 0;
  for (int c = 0; c < core_cnt; ++c)
  {
    int tasks_in_core = _asic->_mult_cores[c]->_local_coarse_task_queue.size();
    tasks_in_core /= _asic->_mult_cores[c]->_data_parallel_throughput;
    tasks_in_core /= _asic->_coarse_alloc[c];
    if (tasks_in_core > max_tasks_yet)
    {
      max_tasks_yet = tasks_in_core;
      max_task_core = c;
    }
  }
  return max_task_core;
}

void task_controller::insert_local_coarse_grained_task(int core, int row, int weight_rows_done, bool second_buffer)
{
  if (_asic->_config->_domain == graphs && _asic->_config->_algo != gcn)
  {
    assert(0 && "graph workloads other than gcn do not have coarse tasks");
  }
  if (_asic->_config->_algo == gcn && _asic->_config->_gcn_matrix == 0)
  {
    return; // we are not allowing these coarse-grained tasks to be created
  }
  // core from row: This should be inclined towards
  // matrix multiply should be evenly distributed -- blocked DFS should have been applied
  // TODO: can it check all cores to find the appropriate one...
  // Algo1: prioritize empty cores. find a random core with higher probability for higher throughput.
  // Algo2: find the core with minimum tasks; for 2-dfg-cores, consider half tasks.
  /*if(_asic->_config->_heter_cores==0) {
    core = find_min_coarse_task_core();
  }*/
  /*if(_asic->_config->_heter_cores==0) {
    int logical_dfg = row%_asic->_dfg_to_mult; // row/(V/_asic->_dfg_to_mult);
    int cur_dfg_id=0;
    for(int c=0; c<core_cnt; ++c) {
      cur_dfg_id += _asic->_mult_cores[c]->_data_parallel_throughput;
      if(cur_dfg_id>logical_dfg) {
        core = c;
        break;
      }
    }
    // srand(0); // this always push to the same core -- it is critical
    for(int c=rand()%core_cnt, i=0; i<core_cnt; ++i, c=(c+1)%core_cnt) {
      if(_asic->_mult_cores[c]->_local_coarse_task_queue.size()==0) {
        core = c;
        break;
      }
    }
  }*/
  // FIXME: checking: remove
  // core=15;
  if (_asic->_config->_algo == gcn)
  {
    core = find_min_coarse_task_core();
  }

  // push only to the start_cores
  if (_asic->_config->_batched_cores > 1)
  {
    core = (core / _asic->_config->_batched_cores) * _asic->_config->_batched_cores;
  }
  // cout << "Creating coarse task at core: " << core << " in cycle: " << _asic->_cur_cycle << endl;

  if (second_buffer)
  {
    ++_asic->_stats->_stat_tot_created_double_buffer_data_parallel_tasks;
  }
  else
  {
    ++_asic->_stats->_stat_tot_created_data_parallel_tasks;
  }
  ++_asic->_stats->_stat_tot_data_parallel_tasks;
  mult_task t;
  t.row = row;
  t.second_buffer = second_buffer;
  t.entry_cycle = _asic->_cur_cycle;
  t.weight_rows_done = weight_rows_done;
  _asic->_mult_cores[core]->_local_coarse_task_queue.push(t);
  _remaining_local_coarse_task_entries[core]--;
  assert(_remaining_local_coarse_task_entries[core] >= 0 && "cannot push in a full coarse-grained buffer");
}

bool task_controller::is_local_coarse_grain_task_queue_full(int core_id)
{
  bool is_full = (_remaining_local_coarse_task_entries[core_id] == 0);
  assert(!is_full && "currently we do not allow full task queue");
  return is_full;
}

bool task_controller::is_central_coarse_grain_task_queue_full()
{
  return (_remaining_central_coarse_task_entries == 0);
}

void task_controller::insert_coarse_grained_task(int row)
{
  ++_asic->_stats->_stat_tot_created_data_parallel_tasks;
  mult_task t;
  t.row = row;
  t.entry_cycle = _asic->_cur_cycle;
  _coarse_task_queue.push(t);
  _remaining_central_coarse_task_entries--;
}

// I actually need absolute size of map to do better load balancing
// TODO: initially it should be skewed to core 0 (or ensure this by
// filling the queue size)
// TODO: consider traffic of work stealing
void task_controller::task_queue_stealing()
{
#if CF == 0

  // TODO: select the most-loaded core
  int random_core = rand() % core_cnt;
  int load = TASK_QUEUE_SIZE;

  for (int i = 0; i < core_cnt; ++i)
  {
    if (_remaining_task_entries[i][0] < load)
    {
      load = _remaining_task_entries[i][0];
      random_core = i;
    }
  }
  /*if(_remaining_task_entries[random_core]>TASK_QUEUE_SIZE-HIGH_PRIO_RESERVE-5) {
    cout << "LOADED CORE IS ALSO EMPTY\n";
    }*/

  for (int i = 0; i < core_cnt; ++i)
  {
    if (_asic->_asic_cores[i]->_task_queue[0][0].empty())
    {
      // steal from randomly chosen core -- turn wasted if own core (let's hope
      // that there are less chances)

      random_core = rand() % core_cnt;
      // TODO: this is affinity-based task stealing
      // random_core = (rand()%4) - 2;
      // random_core = (i + random_core)%core_cnt;
      // random_core = random_core > 0 ? random_core : -random_core;
      // random_core = 0;
      // if(_asic_cores[random_core]->_task_queue[0].size()>1) {
      int elem_done = 0;
      auto it = (_asic->_asic_cores[random_core]->_task_queue[0][0]).begin();
      while (_asic->_asic_cores[random_core]->_task_queue[0][0].size() > 10 && elem_done < 5)
      {

        // don't want to pull useless tasks: but this case is not possible
        if (it->first > _avg_timestamp)
          break;

        // while(_remaining_task_entries[i]<TASK_QUEUE_SIZE-HIGH_PRIO_RESERVE-5 && elem_done<4) {

        // cout << "Came to steal at core: " << i << " from core: " << random_core << endl;
        // if(_asic_cores[random_core]->_task_queue[0].size()>1) {
        // cout << "Steal at core: " << i << " from core: " << random_core << endl;
        // auto it=(_asic_cores[random_core]->_task_queue[0]).rbegin();
        assert(!it->second.empty());
        // cout << "Insert core: " << i << " insert timestamp: " << it->first << " and vid: " << (it->second).front().vid << " from random core: " << random_core << endl;
        while (!(it->second).empty())
        { // Let's try this for better load balancing (extract all values from later timestamp)
          task_entry cur_task = (it->second).front();
          if (cur_task.start_offset != _asic->_offset[cur_task.vid])
          {
            // cout << "cannot pop half done\n";
            break;
          }
          _asic->_stats->_stat_task_stolen++;
          int dist = _asic->_scratch[cur_task.vid];
          insert_local_task(_asic->_asic_cores[i]->_agg_push_index, 0, i, dist, (it->second).front());
          _asic->_asic_cores[i]->_agg_push_index += 1;
          _asic->_asic_cores[i]->_agg_push_index %= NUM_TQ_PER_CORE;
          // insert_local_task(0, i, it->first, (it->second).front());
          /*
          if(!_asic->_config->_sgu) {
            // Update dependence information required for SGU only
            // 1) update parent task info
            int src_tile = cur_task.src_tile; // core where it's source id is
            // cout << "random core: " << random_core << " src_tile: " << src_tile << " current core: " << i << " vid: " << cur_task.vid << " parent_vid: " << cur_task.parent_vid << " searching for tid: " << cur_task.parent_id << endl;
            // assert(random_core==src_tile); // if work-steal first time
            int parent_id = cur_task.parent_id;
            bool found_tid=false;
            if(parent_id!=-1) { // no entry for src 0
              // update ptr instead of replacing
              auto ret = _asic->_asic_cores[src_tile]->_dependence_queue.equal_range(cur_task.parent_vid);
              for(auto itdn=ret.first; itdn!=ret.second; ++itdn) {
                if((itdn->second).first.second==cur_task.parent_id) {
                  for(int j=0; j<itdn->second.second.size(); ++j) {
                    if(itdn->second.second[j].task_id==cur_task.tid) {
                      itdn->second.second[j].core_id=i;
                      found_tid=true;
                      break;
                    }
                  }
                  break;
                }
              }

              if(!found_tid) {
                assert(0 && "Should be able to find tid in depedence queue in infinite length buffer\n");
              }
            }
          }
          */
          elem_done++;
          (it->second).pop_front();
          _remaining_task_entries[random_core][0]++;
        }
        if ((it->second).empty())
        {
          _asic->_asic_cores[random_core]->_task_queue[0][0].erase(it->first);
          it = _asic->_asic_cores[random_core]->_task_queue[0][0].begin();
        }
        else
        {
          ++it;
        }
      }
    }
  }
#endif
}

void task_controller::reset_task_queues()
{
  cout << "Resetting task queues with finished edges: " << _asic->_stats->_stat_tot_finished_edges << endl;
  _asic->_stats->_stat_global_finished_edges += _asic->_stats->_stat_tot_finished_edges;
  _asic->_stats->_stat_tot_finished_edges = 0;
  _asic->_edges_last_iteration = 0;
  // if task queue not empty, push these tasks into the worklist
  for (auto it = _global_task_queue.begin(); !_global_task_queue.empty();)
  {
    int vid = (it->second).front().vid;
    int slice_id = _asic->_scratch_ctrl->get_slice_num(vid);
#if SGU == 1
    assert(slice_id == _asic->_current_slice);
#endif
#if GRAPHMAT_SLICING == 1 || BLOCKED_ASYNC == 1
    _worklists[slice_id].push(make_pair(it->first, (it->second).front()));
#else
    if (_check_worklist_duplicate[slice_id].test(vid) != 1)
    {
      _worklists[slice_id].push(make_pair(it->first, (it->second).front()));
      _check_worklist_duplicate[slice_id].set(vid);
    }
#endif
#if ENTRY_ABORT == 1
    _present_in_queue.reset(vid);
#endif
    (it->second).pop_front();
    if ((it->second).empty())
    {
      _global_task_queue.erase(it->first);
    }
    it = _global_task_queue.begin();
  }

  // TODO: pop from the local task queue to push into the worklist
  int tasks_copied = 0;
  for (int a = 0; a < core_cnt; ++a)
  {
    for (int tqid = 0; tqid < NUM_TQ_PER_CORE; ++tqid)
    {
      for (auto it = _asic->_asic_cores[a]->_task_queue[tqid][0].begin(); it != _asic->_asic_cores[a]->_task_queue[tqid][0].end();)
      {
        bool empty = false;
        for (auto it3 = (*it).second.begin(); it3 != (*it).second.end();)
        {
          if ((*it3).start_offset != _asic->_offset[(*it3).vid])
          { // don't want to pop
#if GRAPHMAT_SLICING == 1 || BLOCKED_ASYNC == 1
            if (!_asic->_switched_async && !_asic->_config->_hybrid)
            {
              assert(0);
            }
#endif
            ++it3;
            continue;
          }
          // if this task has not yet started executing, it cannot be arbitrary
          // pushed?
          tasks_copied++;
          int slice_id = _asic->_scratch_ctrl->get_slice_num((*it3).vid);

          // if((*it3).vid==4976) cout << "Moved from local queue to worklist\n";
#if GRAPHMAT_SLICING == 1 || BLOCKED_ASYNC == 1
          _worklists[slice_id].push(make_pair(it->first, *it3));
#else
          if (_check_worklist_duplicate[slice_id].test((*it3).vid) != 1)
          {
            // (*it3).start_offset = _offset[(*it3).vid]; // OR THEY SHOULD START AGAIN
            _worklists[slice_id].push(make_pair(it->first, *it3));
            _check_worklist_duplicate[slice_id].set((*it3).vid);
          }
#endif

#if ENTRY_ABORT == 1
          _present_in_local_queue[a].reset((*it3).vid);
#endif

          _remaining_task_entries[a][tqid]++;
          it3 = (*it).second.erase(it3);
          if ((*it).second.empty())
          { // if nothing left in list
            empty = true;
            it = _asic->_asic_cores[a]->_task_queue[tqid][0].erase(it);
            break;
          }
          else
            ++it3;
        }
        // cout << "came out of list loop\n";

        // if the task queue is not empty then increment the iterator
        if (!empty && !(*it).second.empty())
        {
          // cout << "came out of list loop\n";
          ++it;
        }
      }
    }
    // cout << "came out of core loop\n";
  }

  cout << "Tasks copied after local task queue: " << tasks_copied << endl;

  // TODO: push tasks in fifo task queue also to the corresponding worklist
  for (int a = 0; a < core_cnt; ++a)
  {
    while (!_asic->_asic_cores[a]->_fifo_task_queue.empty())
    {
      task_entry temp = _asic->_asic_cores[a]->_fifo_task_queue.front();
      int slice_id = _asic->_scratch_ctrl->get_slice_num(temp.vid);
#if GRAPHMAT_SLICING == 1 || BLOCKED_ASYNC == 1
      _worklists[slice_id].push(make_pair(temp.dist, temp));
#else
      if (_check_worklist_duplicate[slice_id].test(temp.vid) != 1)
      {
        _worklists[slice_id].push(make_pair(temp.dist, temp));
        _check_worklist_duplicate[slice_id].set(temp.vid);
      }
#endif
#if ENTRY_ABORT == 1 // but why is it a problem if it is worklist...
      // Now, any new task which is produced after this was being pushed to
      // worklist...in finishing phase
      // for the next phase of the current slice, these vids would be
      // set...
      _present_in_local_queue[a].reset(temp.vid);
#endif
      _asic->_asic_cores[a]->_fifo_task_queue.pop();
    }
  }

  cout << "tasks copied: " << tasks_copied << endl;
#if GRAPHMAT == 1 || BLOCKED_ASYNC == 1
  if (!_asic->_switched_async && !_asic->_config->_hybrid)
  {
    assert(tasks_copied == 0 && "sync do not allow dynamic switch of slices");
  }
#endif
  _asic->_stats->_stat_transition_tasks += tasks_copied;
  /*for(int c=0; c<core_cnt; ++c) {
      bool x = _asic_cores[c]->pipeline_inactive(true);
    }*/
}

bool task_controller::can_push_coarse_grain_task()
{
  bool are_two_tasks_ready = _coarse_task_queue.size() >= BDCAST_WAIT;
  if (are_two_tasks_ready)
    return true;
  if (!are_two_tasks_ready && !_coarse_task_queue.empty())
  {
    if (_asic->_cur_cycle - _coarse_task_queue.front().entry_cycle > MAX_DELAY)
      return true;
  }
  return false;
}

// TODO: put a condition when we want to schedule it or not!
// for locality, we might still want to use local task queues?
mult_task task_controller::schedule_coarse_grain_task()
{
  assert(_coarse_task_queue.size() > 0 && "cannot schedule an empty task queue");
  // TODO: analyze all tasks and maybe reorder them? assign depending on the
  // src_id
  mult_task chosen_task = _coarse_task_queue.front();
  _coarse_task_queue.pop();
  return chosen_task;
}

// Assuming throughput (work distribution window) of core_cnt per cycle
void task_controller::distribute_lb()
{
  // cout << "Global task queue size: " << _global_task_queue.size() << endl;
  if (_global_task_queue.empty())
    return;
  int num_pops = 0;
  for (auto it = _global_task_queue.begin(); !_global_task_queue.empty() && num_pops < 256;)
  { // no limit on pop count to emulate decentralized system
    assert(!_global_task_queue.empty());
    ++num_pops;                                 // oh should it send a bunch of tasks?
    task_entry cur_task = (it->second).front(); // _global_task_queue[t].front();
    int assigned_core = -1;                     // the one that is empty
    assigned_core = _asic->calc_least_busy_core();

    DTYPE priority_order = it->first; // copied from global queue
#if CF == 0
    cur_task.dist = _asic->_scratch[cur_task.vid]; // to ensure priority in local task queue
#endif
#if PR == 1 && CF == 0
    if (cur_task.dist != 0) // this implies its updated value has been already seen earlier by older task (hence this is Case 3 abort)
      insert_local_task(_asic->_asic_cores[assigned_core]->_agg_push_index, 0, assigned_core, priority_order, cur_task);
#else
    insert_local_task(_asic->_asic_cores[assigned_core]->_agg_push_index, 0, assigned_core, priority_order, cur_task);
#endif
    _asic->_asic_cores[assigned_core]->_agg_push_index = (_asic->_asic_cores[assigned_core]->_agg_push_index + 1) % NUM_TQ_PER_CORE;

    // fall-back to balance load
    // cout << "Number of available tasks in the task queue: " << _asic_cores[assigned_core]->_task_queue[0].size() << endl;
    /*if(_cur_cycle<300000) {
    if(_asic_cores[assigned_core]->_task_queue[0].size()>5 && _asic_cores[assigned_core]->_task_queue[0].size()<10) {
      assigned_core++;
      assigned_core = (assigned_core < core_cnt) ? assigned_core : 0;
    }
    }*/
    if (_asic->_config->_entry_abort)
    {
      _present_in_queue.reset(cur_task.vid);
    }
    (it->second).pop_front();
    if ((it->second).empty())
    {
      _global_task_queue.erase(it->first);
      // ++it;
    }
    else
    { // ++it; // we need to work with the same it->second queue
    }
    it = _global_task_queue.begin();
    num_pops++;
  }
}

// Assuming throughput (work distribution window) of core_cnt per cycle
void task_controller::distribute_prio()
{
  // cout << "Global task queue size: " << _global_task_queue.size() << endl;
  if (_global_task_queue.empty())
    return;
  int num_pops = 0;
  // _stats->_stat_tot_vert_tasks+=_global_task_queue.size();
  // for(auto it=_global_task_queue.begin(); !_global_task_queue.empty() && it!=_global_task_queue.end() && num_pops<core_cnt;) {
  // for(auto it=_global_task_queue.begin(); !_global_task_queue.empty() && num_pops<core_cnt;) {
  for (auto it = _global_task_queue.begin(); !_global_task_queue.empty();)
  { // no limit on pop count to emulate decentralized system
    assert(!_global_task_queue.empty());
    // _stats->_stat_tot_tasks_dequed++;
    task_entry cur_task = (it->second).front(); // _global_task_queue[t].front();
    // FIXME: because we don't send everything to memory controller
    /*if((_offset[cur_task.vid+1]-_offset[cur_task.vid])>0) {
      _stats->_stat_tot_tasks_dequed += (_offset[cur_task.vid+1]-_offset[cur_task.vid]);
    }*/
    int assigned_core = -1;
    // cout << "in global task queue, current vertex: " << cur_task.vid << " dist: " << cur_task.dist << endl;

    // affinity-based task distribution (it source and dest are in same core
    // with higher probability
    assigned_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_task.vid);

    DTYPE priority_order = it->first; // copied from global queue
                                      // priority_order = (it->first)*(_num_spec[cur_task.vid]);

    // cout << "Pushed in assigned core: " << assigned_core << endl;
    // TODO: assign lane id as well...
    // Oh maybe I should assign at least throughput number of edges to each
    // core (let's consider that as 5)
    /*
    assigned_core=_which_core;
    _work_steal_depth++;
    if(_work_steal_depth%4==0) { // lazy task distribution
      _work_steal_depth=0;
      _which_core++;
      _which_core = (_which_core < core_cnt) ? _which_core : 0;
    }*/
    // cout << "pushing current task dist: " << cur_task.dist << endl;
    // if(cur_task.dist==0) cout << "vid: " << cur_task.vid << " dist is 0\n";
#if CF == 0
    cur_task.dist = _asic->_scratch[cur_task.vid]; // to ensure priority in local task queue
#endif
#if PR == 1 && CF == 0
    if (cur_task.dist != 0) // this implies its updated value has been already seen earlier by older task (hence this is Case 3 abort)
      insert_local_task(_asic->_asic_cores[assigned_core]->_agg_push_index, 0, assigned_core, priority_order, cur_task);
#else
    insert_local_task(_asic->_asic_cores[assigned_core]->_agg_push_index, 0, assigned_core, priority_order, cur_task);
#endif
    _asic->_asic_cores[assigned_core]->_agg_push_index = (_asic->_asic_cores[assigned_core]->_agg_push_index + 1) % NUM_TQ_PER_CORE;
    /*// this affinity also didn't help
    if(_is_high_degree[cur_task.vid]) {
      assigned_core = _mapped_core[cur_task.vid];
    } else {
      assigned_core = rand()%core_cnt;
      int per_core_vertex = _graph_vertices/core_cnt; 
      int scr_id = (cur_task.vid/per_core_vertex);
      // int scr_id = (_mapping[cur_task.vid]/per_core_vertex);
      scr_id = scr_id < core_cnt ? scr_id : core_cnt-1;
      assert(scr_id<core_cnt);
      assigned_core= scr_id;
    }*/

    // Random didn't help
    // assigned_core = rand()%core_cnt;
    // Trying linear work distribution
    /*
    int per_core_vertex = _graph_vertices/core_cnt; 
    int scr_id = (_mapping[cur_task.vid]/per_core_vertex);
    // int scr_id = (cur_task.vid/per_core_vertex);
    scr_id = scr_id < core_cnt ? scr_id : core_cnt-1;
    assert(scr_id<core_cnt);
    // cout << "scr_id: " << scr_id << endl;
    assigned_core= scr_id;
   */

    // fall-back to balance load
    // cout << "Number of available tasks in the task queue: " << _asic_cores[assigned_core]->_task_queue[0].size() << endl;
    /*if(_cur_cycle<300000) {
    if(_asic_cores[assigned_core]->_task_queue[0].size()>5 && _asic_cores[assigned_core]->_task_queue[0].size()<10) {
      assigned_core++;
      assigned_core = (assigned_core < core_cnt) ? assigned_core : 0;
    }
    }*/

    /*
    insert_local_task(0, _which_core, it->first, cur_task);
    assigned_core=_which_core;
    _work_steal_depth++;
    if(_work_steal_depth%8==0) { // lazy task distribution
      _work_steal_depth=0;
      _which_core++;
      _which_core = (_which_core < core_cnt) ? _which_core : 0;
    }
    */
    // #endif
    // for espresso, push in the meta_info queue as well

#if 0 // SGU == 1 || SWARM==0
   // Update dependence information required for SGU only
   // 1) update parent task info
   int src_tile = cur_task.src_tile;
   int parent_id = cur_task.parent_id;
   if(parent_id!=-1) { // no entry for src 0
     // TODO: drop if full...lets delete at start
     /*if(_asic_cores[src_tile]->_dependence_queue.size()>MAX_DEP_QUEUE_SIZE) {
       auto itd = _asic_cores[src_tile]->_dependence_queue.begin();
       itd = _asic_cores[src_tile]->_dependence_queue.erase(itd);
     }*/
     forward_info f(assigned_core,cur_task.vid,it->first,cur_task.tid);
     auto ret = _asic_cores[src_tile]->_dependence_queue.equal_range(cur_task.parent_vid);
     for(auto itdn=ret.first; itdn!=ret.second; ++itdn) {
       if((itdn->second).first.second==cur_task.parent_id) {
         (itdn->second).second.push_back(f);
       }
     }
     // _asic_cores[src_tile]->_dependence_queue[cur_task.parent_vid].second.push_back(f);
   }
#endif
    if (_asic->_config->_entry_abort)
    {
      _present_in_queue.reset(cur_task.vid);
    }
    (it->second).pop_front();
    if ((it->second).empty())
    {
      _global_task_queue.erase(it->first);
      // ++it;
    }
    else
    { // ++it; // we need to work with the same it->second queue
    }
    it = _global_task_queue.begin();
    num_pops++;
  }
}

int asic::calc_least_busy_core()
{
  int min_edge_tasks = MAX_TIMESTAMP, least_busy_core_id = -1;
  for (int c = 0; c < core_cnt; ++c)
  {
    int num_tasks = 0;
    for (int t = 0; t < MAX_TIMESTAMP; ++t)
    {
      num_tasks += _asic_cores[c]->_task_queue[0][t].size();
      /*list<task_entry> x =_asic_cores[c]->_task_queue[t];
      for(auto it=x.begin(); it!=x.end(); ++it) {
        int vid = (*it).vid;
        num_tasks += (_offset[vid+1] - _offset[vid]);
      }*/
    }
    if (num_tasks < min_edge_tasks)
    {
      min_edge_tasks = num_tasks;
      least_busy_core_id = c;
    }
  }
  return least_busy_core_id;
}

// Assuming throughput (work distribution window) of core_cnt per cycle
// FIXME: Distribute sorted chunks or really in sorted order?
void task_controller::distribute_one_task_per_core()
{
  if (_global_task_queue.empty())
    return;
  // cout << "Global task queue size: " << _global_task_queue.size() << endl;
  int num_pops = 0;
  for (auto it = _global_task_queue.begin(); !_global_task_queue.empty() && num_pops < core_cnt;)
  { // no limit on pop count to emulate decentralized system
    assert(!_global_task_queue.empty());
    task_entry cur_task = (it->second).front(); // _global_task_queue[t].front();

    int assigned_core = num_pops;

    // affinity-based task distribution (it source and dest are in same core
    // with higher probability)
    assigned_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_task.vid);

    DTYPE priority_order = it->first; // copied from global queue
    insert_local_task(_asic->_asic_cores[assigned_core]->_agg_push_index, 0, assigned_core, priority_order, cur_task);
    _asic->_asic_cores[assigned_core]->_agg_push_index += 1;
    _asic->_asic_cores[assigned_core]->_agg_push_index %= NUM_TQ_PER_CORE;
    // #if SGU == 1 || SWARM==0
    /*if(_asic->_config->_exec_model==async) {
     // Update dependence information required for SGU only
     // 1) update parent task info
     int src_tile = cur_task.src_tile;
     int parent_id = cur_task.parent_id;
     if(parent_id!=-1) { // no entry for src 0
       forward_info f(assigned_core,cur_task.vid,it->first,cur_task.tid);
       auto ret = _asic->_asic_cores[src_tile]->_dependence_queue.equal_range(cur_task.parent_vid);
       for(auto itdn=ret.first; itdn!=ret.second; ++itdn) {
         if((itdn->second).first.second==cur_task.parent_id) {
           (itdn->second).second.push_back(f);
         }
       }
     }
   }*/
    // #endif
    (it->second).pop_front();
    if ((it->second).empty())
    {
      _global_task_queue.erase(it->first);
      // ++it;
    }
    else
    { // ++it; // we need to work with the same it->second queue
    }
    it = _global_task_queue.begin();
    num_pops++;
  }
}

// get the priority relative to the priority queue
int task_controller::get_middle_rank(int factor)
{
  int cur_last, cur_start;
  if (!_global_task_queue.empty())
  {
    auto it1 = _global_task_queue.rbegin();
    auto it2 = _global_task_queue.begin();
    cur_last = it1->first;
    cur_start = it2->first;
    // cout << "Current range of distances: " << cur_start << " " << cur_last << " ratio: " << cur_last/(double)cur_start << endl;
  }
  if (cur_start != 0)
  {
    // double rank = (cur_last*factor/(double)cur_start);
    double rank = (cur_last * factor / cur_start);
    return rank * cur_start;
  }
  else
  {
    return cur_start;
  }
}

void task_controller::push_task_into_worklist(int wklist, DTYPE priority, task_entry cur_task)
{
  if (_asic->_config->_pull_mode)
  {
    DTYPE priority = 0;
    if (!_asic->_config->is_vector())
    {
      priority = _asic->apply(_asic->_vertex_data[cur_task.vid], cur_task.vid);
    }
    for (int e = _asic->_csr_offset[cur_task.vid]; e < _asic->_csr_offset[cur_task.vid + 1]; ++e)
    {
      int vid = _asic->_csr_neighbor[e].dst_id;
      int wklist = _asic->_scratch_ctrl->get_slice_num(vid);
      task_entry new_task(vid, _asic->_offset[vid]);
      _worklists[wklist].push(make_pair(priority, new_task));
    }
  }
  else
  {
    _worklists[wklist].push(make_pair(priority, cur_task));
  }
}

bool task_controller::worklists_empty()
{
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    // if(!_worklists[i].empty()) return false;
#if PR == 1
    // if(_worklists[i].size()>2500) return false;
    if (_worklists[i].size() > 0)
      return false;
#else
    if (!_worklists[i].empty())
      return false;
#endif
  }
  return true;
}

int task_controller::move_tasks_from_wklist_to_tq(int slice_id)
{
  int tot_tasks_copied = 0;
  while (!_worklists[slice_id].empty())
  {
    task_entry new_task = _worklists[slice_id].front().second;
    DTYPE priority_stored = _worklists[slice_id].front().first;
    _worklists[slice_id].pop();
    _check_worklist_duplicate[slice_id].reset(new_task.vid);
    // TODO: vidushi: ensure that dist is passed during creation
    // if(_asic->_config->_algo==pr && new_task.dist==0) continue;
    if (!_asic->_config->is_vector())
    {
      new_task.dist = _asic->_scratch[new_task.vid];
    }
    // #if PR==0 && ABCD==0 && FIFO==0 && PULL==0 && GRAPHPULSE==0 // TODO: check this!!!
    //     if(_asic->_scratch[new_task.vid] > priority_stored) {
    //       cout << "vid: " << new_task.vid << " scratch: " << _asic->_scratch[new_task.vid] << " worklist: " << priority_stored << endl;
    //     }
    //     assert(_asic->_scratch[new_task.vid] <= priority_stored && "updated distance should be better/same than the one stored in worklist");
    // #endif
    int target_core = _asic->_scratch_ctrl->get_local_scratch_id(new_task.vid);
#if WORK_STEALING == 0
    insert_global_task(_asic->apply(_asic->_scratch[new_task.vid], new_task.vid), new_task);
#else
    DTYPE priority = 0;
#if CF == 0
    priority = _asic->apply(_asic->_scratch[new_task.vid], new_task.vid);
#endif
    insert_local_task(_asic->_asic_cores[target_core]->_agg_push_index, 0, _asic->_scratch_ctrl->get_local_scratch_id(new_task.vid), priority, new_task);
#endif
    // number of task queues should be 1 for sync...
    _asic->_asic_cores[target_core]->_agg_push_index = (_asic->_asic_cores[target_core]->_agg_push_index + 1) % NUM_TQ_PER_CORE;
    tot_tasks_copied++;
  }
  return tot_tasks_copied;
}

// centralized dispatch
void task_controller::distribute2()
{
  if (_global_task_queue.empty())
    return;
  int num_edges_processed = 0;
  pref_tuple next_tuple;
  int i = 0;
  int src_id;
  // int cur_core=0,cur_edges=0;
  int max_edges_per_core = process_thr;
  int assigned_core = 0;

  // Actually, I can do scheduling of the current edges among the cores equally
  for (auto it = _global_task_queue.begin(); !_global_task_queue.empty() && it != _global_task_queue.end() && num_edges_processed < process_thr;)
  { //_max_elem_mem_load;) {
    assert(!_global_task_queue.empty());
    assert(!(it->second).empty());
    task_entry cur_task = (it->second).front();
#if SWARM == 1 || ESPRESSO == 1
    next_tuple.tid = cur_task.tid;
#endif
    src_id = cur_task.vid;
    next_tuple.src_dist = cur_task.dist;
    next_tuple.src_id = src_id;
    i = cur_task.start_offset;
    for (i = cur_task.start_offset; num_edges_processed < process_thr && i < _asic->_offset[src_id + 1]; ++i)
    {
      // cout << cur_task.vid << " " << _neighbor[i].dst_id << endl;
      if (_num_edges == max_edges_per_core)
      {
        _num_edges = 0;
        _which_core++;
        _which_core = _which_core < core_cnt ? _which_core : 0;
      }
      _num_edges++;
      assigned_core = _which_core;
      // Causes a bit of load imbalance,
      // Oh, so extract last 16-bits, use first 8-bits for core_id and last
      // 8-bits for bank_id
      // Oh, what was happening earlier is it was mapping everything to the
      // same cluster we are working on right now (so basically we are working
      // on a single cluster at a time, and want to distribute work among 16
      // cores -- so what if I do _mapping[cur_task.vid]%
      // assigned_core = (_mapping[cur_task.vid]%256)/16;
      // if(assigned_core>=core_cnt) assigned_core=0; // just for core=1 case
      // assigned_core = get_local_scratch_id(cur_task.vid);
      assigned_core = _asic->_scratch_ctrl->get_local_scratch_id(_asic->_neighbor[i].dst_id);

      // next_tuple.edge_id = i;
      next_tuple.edge = _asic->_neighbor[i];
      _asic->_asic_cores[assigned_core]->insert_prefetch_process(0, 0, next_tuple);
      num_edges_processed++;
      // _stats->_stat_dist_tasks_dequed[assigned_core]++;
// this is causing 5x drop in performance even with ideal net?
#if NETWORK == 0
      // vid and dst id are in different cores half of the times... (10x
      // degradation in performance?)
      // this should be incoming traffic, I am making it outgoing, do something
      // We should just keep it at the same core if it is static scheduling
      for (int i = 0; i < 2; ++i)
      { // 4*4 byes (edge weight, dist)
        assigned_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_task.vid);
        int dest_core = _asic->_scratch_ctrl->get_mc_core_id(cur_task.vid);
        // this would be 0 if it was local
        // dest_core = assigned_core;
        dest_core = _asic->_scratch_ctrl->get_local_scratch_id(_asic->_neighbor[i].dst_id);
        // dest_core = get_mc_core_id(_neighbor[i].dst_id);
        // push_dummy_packet(assigned_core, dest_core, false);
        // push_dummy_packet(dest_core, assigned_core, false);
      }
#endif
    }

    if (i == _asic->_offset[src_id + 1])
    {
      (it->second).pop_front();
      if ((it->second).empty())
      {
        _global_task_queue.erase(it->first);
        it = _global_task_queue.begin();
      }
    }
    else
    {
      (it->second).front().start_offset = i;
    }
  }
}

int task_controller::tot_pending_tasks()
{
  int start_active_vertices = 0;
  for (int slice_id = 0; slice_id < _asic->_slice_count; ++slice_id)
  {
    start_active_vertices += _worklists[slice_id].size();
  }
  // we should calculate active tasks
  for (int c = 0; c < core_cnt; ++c)
  {
    start_active_vertices += _asic->_asic_cores[c]->_fifo_task_queue.size();
    start_active_vertices += (COARSE_TASK_QUEUE_SIZE - _remaining_local_coarse_task_entries[c]);
    for (int t = 0; t < NUM_TQ_PER_CORE; ++t)
    {
      start_active_vertices += (TASK_QUEUE_SIZE - _remaining_task_entries[c][t]);
    }
  }
  cout << "Total pending tasks: " << start_active_vertices << endl;
  return start_active_vertices;
}

int task_controller::chose_new_slice(int old_slice)
{
#if ABCD == 0
  return (old_slice + 1) % SLICE_COUNT;
#else
  // TODO: choose based on priority
  // priority based on difference? (only old_slice was changed)
  /*if(old_slice!=-1) {
        _grad[old_slice]=0;
      for(int i=old_slice*_slice_size; i<(old_slice+1)*_slice_size && i<_graph_vertices; ++i) {
        _grad[old_slice] += abs(_vertex_data[i]-_scratch[i]);
      }
    }*/

  // Chose the slice with max gradient
  DTYPE max_grad = 0;
  int next_slice = -1;
  for (int i = 0; i < SLICE_COUNT; ++i)
  {
    if (_asic->_grad[i] > max_grad)
    {
      next_slice = i;
      max_grad = _asic->_grad[i];
    }
  }
  // is this the case mostly?
  if (next_slice == -1)
  {
    // it should come here when there are no active vertices
    next_slice = rand() % SLICE_COUNT;
  }
  cout << "Next slice is: " << next_slice << endl;
  return next_slice;
#endif
}

void memory_controller::initialize_cache()
{
  if (_asic->_config->_domain == graphs)
  {
    return;
  }
  int max_cache_data = L2SIZE * 1024 / message_size;
  int put = min(max_cache_data, _asic->_graph_vertices * message_size);
  if (_asic->_config->is_vector())
  {
    put = min(max_cache_data, _asic->_graph_vertices * FEAT_LEN * message_size);
  }
  if (_asic->_config->_domain == tree)
  {

    // not completely solvable as only a few leaves fit
    int max_leaves = (L2SIZE * 1024) / (NUM_DATA_PER_LEAF * message_size);
    int num_cache_lines_per_leaf = (NUM_DATA_PER_LEAF * message_size) / line_size;
    for (int i = 0; i < max_leaves; ++i)
    {
      int leaf_id = i * 4;
      for (int n = 0; n < num_cache_lines_per_leaf; ++n)
      {
        uint64_t paddr = _weight_offset + (leaf_id * num_cache_lines_per_leaf + n) * line_size;
        if (!_banked)
        {
          bool hit = _l2_cache->LookupAndFillCache(0, 0, paddr, ACCESS_LOAD, true, 0);
        }
        else
        {
          int cache_bank = leaf_id % core_cnt;
          assert(cache_bank < core_cnt);
          bool hit = _banked_l2_cache[cache_bank]->LookupAndFillCache(0, 0, paddr, ACCESS_LOAD, true, 0);
        }
      }
    }
  }
  else
  {
    for (int i = 0; i < max_cache_data; ++i)
    {
      uint64_t paddr = i * message_size; // byte-address
      if (!_banked)
      {
        bool hit = _l2_cache->LookupAndFillCache(0, 0, paddr, ACCESS_LOAD, true, 0); // check cache statistics
      }
      else
      {
        int cache_bank = _asic->_scratch_ctrl->get_local_scratch_id(i);
        // paddr = paddr/core_cnt;
        assert(cache_bank < core_cnt);
        bool hit = _banked_l2_cache[cache_bank]->LookupAndFillCache(0, 0, paddr, ACCESS_LOAD, true, 0); // check cache statistics
      }
    }
  }
}

void memory_controller::reset_compulsory_misses()
{
  uint64_t num_cache_lines = E / (line_size / message_size);
  for (uint64_t i = 0; i < num_cache_lines; ++i)
  {
    _is_edge_hot_miss[i] = true;
  }
}

/* callback functors */
void memory_controller::read_complete(unsigned data, uint64_t address, uint64_t clock_cycle)
{

  // cout << "Receiving memory request at cycle: " << _asic->_cur_cycle << " at core: " << 0 << endl; // req_core_id << endl;
  // cout << "Completed read for address: " << address << " current status of my cb entry: " << _asic->_compl_buf[0]->_reorder_buf[17].waiting_addr << endl;

  _asic->_stats->_stat_dram_cache_accesses++;
  // cout << "Memory response: " << _asic->_stats->_stat_dram_cache_accesses << " at cycle: " << _asic->_cur_cycle << endl;
  if (_asic->_finishing_phase)
    _asic->_stats->_stat_dram_add_on_access++;

  // or if it is coarseMem for kNN
  if (_asic->_config->_hash_join == 1)
  {
    bool l2hit = _l2_cache->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
  }

  auto it = _outstanding_mem_req.find(address);
  // cout << "Came to return address: " << address << endl;
  assert(it != _outstanding_mem_req.end() && "this address is pulled from outstanding memory");
  int req_core_id = -1;
  int tot = 0;
  pref_tuple return_to_lsq;
  // cout << "Returned address: " << address << " at cycle: " << _asic->_cur_cycle << endl;
  for (auto req = _outstanding_mem_req[address].begin(); req != _outstanding_mem_req[address].end();)
  {
    tot++;
    // cout << "Size of list: " << _outstanding_mem_req[address].size() << " ";

    return_to_lsq = *req;
    req = _outstanding_mem_req[address].erase(req);
    if (_asic->_config->_hash_join)
    {
      // cout << "Received a memory request at cycle: " << _asic->_cur_cycle << endl;
      --_asic->_stats->_stat_pending_cache_misses;
      --_asic->_mem_ctrl->_num_pending_cache_reqs;
      continue;
    }

    // FIXME: value not correct for some reason
    // return_to_lsq.edge.wgt = data;
    req_core_id = return_to_lsq.req_core;
    // cout << "Start offset: " << return_to_lsq.arob_entry << endl;

    // mark the entry valid in cb buffer
    int cb_buf_entry = return_to_lsq.cb_entry;
    _stats->_stat_pending_colaesces--;

    // TODO: if cb_entry is equal to -1, then this is reordering message,
    // hence can be directly pushed to the bank queues
    // TODO: this should also go here if graphmat_slicing==1 and resizing=1
    bool go_in = _asic->_config->_working_cache == 1 || _asic->_config->_edge_cache == 1;
    if ((_asic->_config->_graphmat_slicing) && (_asic->_finishing_phase || _asic->_switched_cache))
      go_in = true;
    if (go_in)
    {
      int assoc_core = req_core_id;
      if (cb_buf_entry == -1)
      {
        _num_pending_cache_reqs--;
        if (return_to_lsq.update_width > 0)
        { // wide gcn packet
          //  cout << "memory response for dst_id: " << return_to_lsq.edge.dst_id << " at cycle: " << _asic->_cur_cycle << endl;
          int dst_core = _asic->_scratch_ctrl->get_local_scratch_id(return_to_lsq.edge.dst_id);
          if (_asic->_config->_algo == gcn && _asic->_config->_cache_hit_aware_sched)
          {
            return_to_lsq.src_dist = _asic->_cur_cycle; // enqueue cycle...
            ++_asic->_stats->_stat_tot_miss_updates_served[dst_core];
            ++_asic->_asic_cores[dst_core]->_mem_responses_this_slice;
            if (_asic->_config->_update_coalesce)
            {
              assert(_asic->_task_ctrl->_present_in_miss_gcn_queue[return_to_lsq.edge.dst_id] > 0 && "at least 1 request should be pending for a memory response");
              while (_asic->_task_ctrl->_present_in_miss_gcn_queue[return_to_lsq.edge.dst_id] > 0)
              {
                _asic->_asic_cores[dst_core]->_priority_hit_gcn_updates.push_back(return_to_lsq);
                --_asic->_task_ctrl->_present_in_miss_gcn_queue[return_to_lsq.edge.dst_id];
              }
              assert(_asic->_task_ctrl->_present_in_miss_gcn_queue[return_to_lsq.edge.dst_id] == 0 && "all pending requests should have been served");
              // for update coalesce, there should be no outstanding memory requests for the same address!!
              assert(req == _outstanding_mem_req[address].end() && "only 1 request should be active at a time");
            }
            else
            {
              _asic->_asic_cores[dst_core]->_priority_hit_gcn_updates.push_back(return_to_lsq);
            }

            // TODO: this is very inefficient, better to coalesce with pending vid's as well. Let me have a bitvector for pending memory requests
            // If there is such a value, just merge at insertion to miss queue (push a gcn update tuple to cgra_event_queue)
            /*if(_asic->_config->_update_coalesce) {
              for (auto it = _asic->_asic_cores[dst_core]->_miss_gcn_updates.begin();
                   it != _asic->_asic_cores[dst_core]->_miss_gcn_updates.end(); ++it) {
                // requests that are still in miss queue ie. not coalesced already
                if (it->edge.dst_id == return_to_lsq.edge.dst_id) {
                  // remove an element from deque or mark as invalid -- this will cause atomic conflicts
                  // cout << "Marked dst_id invalid: " << return_to_lsq.edge.dst_id << " at cycle: " << _asic->_cur_cycle << endl;
                  if (!it->invalid) {
                    it->invalid = true;
                    // ++_asic->_stats->_stat_tot_miss_updates_served;
                    // ++_asic->_stats->_stat_tot_priority_hit_updates_served;
                    _asic->_asic_cores[dst_core]->_priority_hit_gcn_updates.push_back(return_to_lsq);
                  }
                }
              }
              for (auto it = _asic->_asic_cores[dst_core]->_hit_gcn_updates.begin();
                   it != _asic->_asic_cores[dst_core]->_hit_gcn_updates.end(); ++it) {
                if (it->edge.dst_id == return_to_lsq.edge.dst_id) {
                  if (!it->invalid) {
                    it->invalid = true;
                    _asic->_asic_cores[dst_core]->_priority_hit_gcn_updates.push_back(return_to_lsq);
                  }
                }
              }
            }*/
          }
          else
          {
            // assert(_asic->_config->_algo==gcn && "update width should be -ve for others");
            assert(_asic->_config->is_vector() && "update width should be -ve for others");
            if (_asic->_config->_update_coalesce)
            {
              _asic->_asic_cores[dst_core]->_priority_hit_gcn_updates.push_back(return_to_lsq);
            }
            else
            {
              _asic->_asic_cores[dst_core]->insert_prefetch_process(0, 0, return_to_lsq);
            }
            ++_asic->_stats->_stat_tot_miss_updates_served[dst_core];
          }
          assoc_core = dst_core;

          // TODO: find a victim and replace in the cache...
          // bool l2hit = _l2_cache->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0); // check cache statistics
          // l2hit = _l2_cache->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0); // check cache statistics
          // assert(l2hit==true);
        }
        else
        {
          int dst_id = return_to_lsq.src_id;
          int dest_core = _asic->_scratch_ctrl->get_local_scratch_id(dst_id);
          int bank_id2 = _asic->_scratch_ctrl->get_local_bank_id(dst_id);
          if (_asic->_config->_perfect_lb == 1 && _asic->_config->_net == mesh && _asic->_config->_prac == 0)
          {
            _asic->_last_bank_per_core[dest_core] = (_asic->_last_bank_per_core[dest_core] + 1) % num_banks;
            bank_id2 = _asic->_last_bank_per_core[dest_core];
            _asic->_last_core = (_asic->_last_core + 1) % core_cnt;
            dest_core = _asic->_last_core;
            // cout << "Core: " << dest_core << " chosen bank: " << bank_id_global << endl;
          }
          red_tuple cur_tuple;
          cur_tuple.dst_id = dst_id;
          cur_tuple.new_dist = return_to_lsq.src_dist;
          assert(cur_tuple.new_dist >= 0 && "new dist should be greater than 0");
          assoc_core = dest_core;
          if (_asic->_config->_network)
          {
            // cout << "DOES IT COME HERE\n";
            _asic->_asic_cores[dest_core]->_local_bank_queues[bank_id2].push(cur_tuple);
          }
          else
          {
            _asic->insert_global_bank(bank_id2, cur_tuple.new_dist, cur_tuple);
            // _asic->_bank_queues[bank_id2].push_back(cur_tuple);
          }
        }
        // filling the cache back
        if (_asic->_config->_phi == 0)
        {
          // address *= line_size;
          if (!_banked)
          {
            bool l2hit = _l2_cache->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
          }
          else
          {
            bool l2hit = _banked_l2_cache[assoc_core]->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
          }
          // bool new_hit = is_cache_hit(0, address, ACCESS_LOAD, true, 0);
          // bool basic_new_hit = _l2_cache->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
          // cout << "Filling cache with address: " << address << " at cycle: " << _asic->_cur_cycle << endl;
        }
        _stats->_stat_pending_cache_misses--;
        continue;
      }
    }
    auto compl_buf = _asic->_compl_buf[req_core_id];
    if (return_to_lsq.core_type == coarseMem)
    {
      compl_buf = _asic->_coarse_compl_buf[req_core_id][return_to_lsq.dfg_id];
      // cout << "Serving a coarse-grained memory request\n";
    }
    else
    {
      // cout << "Serving a fine-grained memory request\n";
    }

    assert(!compl_buf->_reorder_buf[cb_buf_entry].valid && "cb overflow, no element should be available here beforehand");
    assert(compl_buf->_reorder_buf[cb_buf_entry].waiting_addr > 0 && "it should be waiting on certain number of cache lines");
    compl_buf->_reorder_buf[cb_buf_entry].waiting_addr--;
    if (_asic->_config->_algo == pr)
    {
      assert(return_to_lsq.src_dist >= 0);
    }
    // dummy packet; not for fixing value (just for waiting on CGRA)
    // if(return_to_lsq.core_type!=2) {
    if (return_to_lsq.core_type != dummyCB)
    {
      compl_buf->_reorder_buf[cb_buf_entry].cur_tuple = return_to_lsq;
    }

    // cout << "Received output from memory cb_entry: " << cb_buf_entry << " at cycle: " << _asic->_cur_cycle
    // << " waiting_addr: " << compl_buf->_reorder_buf[cb_buf_entry].waiting_addr << endl;

    if (compl_buf->_reorder_buf[cb_buf_entry].waiting_addr == 0)
    {
      compl_buf->_reorder_buf[cb_buf_entry].valid = true;
      if (_asic->_config->_domain == tree)
      {
        if (_banked)
        {
          _banked_l2_cache[req_core_id]->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
        }
        else
        {
          _l2_cache->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
        }
      }

      // compl_buf->_reorder_buf[cb_buf_entry].cur_tuple=return_to_lsq;

      if (_asic->_config->_edge_cache && _asic->_config->_phi == 0)
      {
        if (!_banked)
        {
          bool l2hit = _l2_cache->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
        }
        else
        {
          bool l2hit = _banked_l2_cache[req_core_id]->LookupAndFillCache(0, 0, address, ACCESS_LOAD, true, 0);
        }
        _asic->_space_allotted++;
      }
    }
    else
    {
      // cout << "Core type: " << return_to_lsq.core_type << " req core id: " << return_to_lsq.req_core << " Waiting address left is: " << compl_buf->_reorder_buf[cb_buf_entry].waiting_addr << " cb buf entry: " << cb_buf_entry << endl;
    }
    // cout << "Read request being returned to req core: " << req_core_id << endl;
    // _asic_cores[req_core_id]->_pref_lsq[0].push(return_to_lsq);
  }
  _stats->_stat_cache_line_util += tot * message_size / (float)line_size;
  _outstanding_mem_req.erase(address);
  // cout << "Erased outstanding memory request from address: " << address << endl;

  if (_asic->_config->_lru)
  {
    update_cache_on_new_access(req_core_id, address, _asic->_cur_cycle);
  }
  // receive_cache_miss(address, req_core_id);
}

void memory_controller::write_complete(unsigned id, uint64_t address, uint64_t clock_cycle)
{
  // printf("[Callback] write complete: %d 0x%lx cycle=%lu\n", id, address, clock_cycle);
  _cur_writes++;
}

// push remote scratch to network
void scratch_controller::send_scratch_request(int req_core_id, uint64_t scratch_addr, pref_tuple cur_tuple)
{
  _asic->_scratch_ctrl->_scratch_reqs_this_cycle++;
  net_packet remote_scr_req = _asic->_network->create_scalar_request_packet(cur_tuple.src_id, cur_tuple.edge.dst_id, req_core_id, cur_tuple.cb_entry, cur_tuple.second_buffer);
  remote_scr_req.last = cur_tuple.last; //not required (stored in CB)
  remote_scr_req.dst_id = scratch_addr;
  remote_scr_req.dfg_id = cur_tuple.dfg_id;
  if (cur_tuple.core_type == fineScratch)
  {
    remote_scr_req.new_dist = -1;
  }
  else
  {
    remote_scr_req.new_dist = 0;
    if (_asic->_config->_hats != 1)
    {
      assert(!_asic->_coarse_reorder_buf[req_core_id][cur_tuple.dfg_id]->_reorder_buf[cur_tuple.cb_entry].valid && "coarse CB entry should have been allocated earlier");
      assert(_asic->_coarse_reorder_buf[req_core_id][cur_tuple.dfg_id]->_reorder_buf[cur_tuple.cb_entry].waiting_addr > 0 && "coarse CB entry should have been allocated earlier");
    }
  }
  _net_data.push(remote_scr_req);
  // if(_asic->_config->_perfect_net==0) {
  //   assert(remote_scr_req.cb_entry>=0 && "should not pass a -ve cb entry");
  //   _asic->_network->push_net_packet(req_core_id, remote_scr_req);
  // } else {
  //   _net_data.push(remote_scr_req);
  // }
}

// TODO: FIXME: Use the receive_scratch_request to access the local coalescer and put in the
// local completion buffer...
// TODO: push it into the completion buffer or whatever is waiting for reads --
// currently it should push in a coarse-grained reorder buffer?
void scratch_controller::receive_scratch_request()
{ // TODO: should call this first??
  // pull data from net_input_buffer and copy to reorer buffer...
  pref_tuple return_to_lsq;
  while (!_net_data.empty())
  {
    net_packet remote_read_data = _net_data.front();
    _net_data.pop();

#if LOCAL_HATS == 1
    _asic->_local_coalescer[remote_read_data.req_core_id]->push_local_read_responses_to_cb(remote_read_data.cb_entry); // index_into_local
    continue;
#endif

    return_to_lsq.dfg_id = remote_read_data.dfg_id;

    // return_to_lsq.entry_cycle = remote_read_data.entry_cycle;
    int cb_buf_entry = remote_read_data.cb_entry;
    auto reorder_buf = _asic->_coarse_reorder_buf[remote_read_data.req_core_id][return_to_lsq.dfg_id];
    // FIXME: how to separate multiply scratch response and the fine-grained one?
    // if(remote_read_data.core_type==fineScratch) { // convey that info
    if (remote_read_data.packet_type == scalarResponse && remote_read_data.new_dist == -1)
    { // NOT AN IDEAL WAY
      reorder_buf = _asic->_fine_reorder_buf[remote_read_data.req_core_id];
    }
    else
    {
      // cout << "Received scr addr for vid: " << return_to_lsq.src_id << " cb entry: " << cb_buf_entry << " request core id: " << remote_read_data.req_core_id << " and waiting addr: " << reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr << " at cycle: " << _asic->_cur_cycle << endl;
    }

    /*if(remote_read_data.req_core_id==12 && cb_buf_entry==0) {
      cout << "cb buf entry: " << cb_buf_entry << " req_core: " << remote_read_data.req_core_id << " waiting_addr: " << reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr << endl;
    }*/

    // return_to_lsq.second_buffer = remote_read_data.second_buffer;
    // return_to_lsq.src_id = remote_read_data.src_id;
    // Set inside the hardware -- so nobody should overwrite it!!
    // cout << "Remote read data src_id: " << remote_read_data.src_id << " from cb entry: " <<  reorder_buf->_reorder_buf[cb_buf_entry].cur_tuple.src_id << endl;
    if (_asic->_config->_algo == gcn)
    {
      return_to_lsq.second_buffer = reorder_buf->_reorder_buf[cb_buf_entry].cur_tuple.second_buffer;
      return_to_lsq.src_id = reorder_buf->_reorder_buf[cb_buf_entry].cur_tuple.src_id;
    }
    else
    {
      return_to_lsq.src_id = remote_read_data.src_id;
    }

    // cout << "Receiving req core id: " << remote_read_data.req_core_id << " and src: " << return_to_lsq.src_id << " cb buf entry: " << cb_buf_entry << " dst_id: " << remote_read_data.dst_id << endl;
    return_to_lsq.last = remote_read_data.last;
    // cout << "Last of Src_id: " << return_to_lsq.src_id << " is: " << return_to_lsq.last << endl;
    if (cb_buf_entry != -1)
    {
      assert(!reorder_buf->_reorder_buf[cb_buf_entry].valid && "cb overflow, no element should be available here beforehand");
      assert(reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr > 0 && "it should be waiting on certain number of cache lines");
      reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr--;

      // We also need to see how it will push to lsq...
      if (reorder_buf->_reorder_buf[cb_buf_entry].waiting_addr == 0)
      {
        reorder_buf->_reorder_buf[cb_buf_entry].valid = true;
        reorder_buf->_reorder_buf[cb_buf_entry].cur_tuple = return_to_lsq;
      }
    }
    else
    { // no reordering required
      _asic->_asic_cores[remote_read_data.req_core_id]->push_scratch_data(return_to_lsq);
    }
  }
}

bool memory_controller::send_mem_request(bool isWrite, uint64_t line_addr, pref_tuple cur_tuple, int streamid)
{
  _mem_reqs_this_cycle++;
  // cout << "Sending real memory request for src_id: " << cur_tuple.src_id << " with degree: " << _asic->get_degree(cur_tuple.src_id) << endl;
  // assert(line_addr < MEMSIZE && "cannot access memory greater than mem size");

  //  if(_asic->_config->_algo!=pr) {
  //    assert(cur_tuple.src_dist>=0);
  //  }

  line_addr /= line_size;
  line_addr *= line_size;

  // cout << "Sent request for memory address: " << line_addr << endl;

  if (isWrite)
  {
    // cout << "Write request send for: " << line_addr << endl;
    _mem->addTransaction(isWrite, line_addr);
    return true;
  }

  if (!_asic->_config->_edge_cache && !(_asic->_config->_domain == tree && (cur_tuple.core_type == coarseMem || cur_tuple.core_type == fineMem)))
  {
    _stats->_stat_dram_bytes_requested += message_size;
    bool sent_request = send_mem_request_actual(isWrite, line_addr, cur_tuple, streamid);
    return sent_request;
  }

  int src_core = cur_tuple.req_core;
  auto compl_buf = _asic->_compl_buf[src_core];
  if (cur_tuple.core_type == coarseMem)
  {
    compl_buf = _asic->_coarse_compl_buf[src_core][cur_tuple.dfg_id];
  }

  // TODO: check whether it is a hit in the private cache
  bool hit = false;
  if (_asic->_config->_domain == tree && cur_tuple.core_type == fineMem)
  {
    hit = true; // ho need to load, just for simulation purposes
  }
  else
  {
    // if read request, check src_core -- otherwise copy dst_core here
    hit = is_cache_hit(src_core, line_addr, ACCESS_LOAD, true, 0);
  }
  if (!(_asic->_config->_domain == tree && cur_tuple.core_type == fineMem))
  {
    _l2accesses++;
    ++_l2accesses_per_core[cur_tuple.req_core];
  }
  /*if(_asic->_config->_cache_hit_aware_sched) {
    hit=false;
  }*/
  if (hit)
  {
    if (!(_asic->_config->_domain == tree && cur_tuple.core_type == fineMem))
    {
      _l2hits++;
      ++_l2hits_per_core[cur_tuple.req_core];
    }
    compl_buf->_reorder_buf[cur_tuple.cb_entry].waiting_addr--;
    if (compl_buf->_reorder_buf[cur_tuple.cb_entry].waiting_addr == 0 && cur_tuple.core_type != dummyCB)
    {
      compl_buf->_reorder_buf[cur_tuple.cb_entry].valid = true;
      compl_buf->_reorder_buf[cur_tuple.cb_entry].cur_tuple = cur_tuple;
    }
    // cout << "Hits for tuple: " << cur_tuple.src_id << endl;
    return false;
  }
  else
  {
    _stats->_stat_dram_bytes_requested += message_size;
    bool sent_request = send_mem_request_actual(isWrite, line_addr, cur_tuple, streamid);
    if (!sent_request)
    {
      // cout << "Coalesced for tuple: " << cur_tuple.src_id << endl;
      if (!(_asic->_config->_domain == tree && cur_tuple.core_type == fineMem))
      {
        _l2hits++;
        ++_l2hits_per_core[cur_tuple.req_core];
        _l2coalesces++;
      }
    }
    else
    {
      // cout << "Miss for tuple: " << cur_tuple.src_id << endl;
    }
    return sent_request;
  }

  // if(cur_tuple.req_core==0 && cur_tuple.lane_id==0)
  // cout << "Setting pending to be true at: " << _cur_cycle << endl;

  // FIXME: not sure what this is?
  /*
#if GCN_SWARM==1 || SWARM==1
  // push in the l2 cache delay buffer
  pending_mem_req a(isWrite, line_addr, cur_tuple);
  int delay = 18;
  int mc_ctrl = use_knh_hash(line_addr)/2;
  int src_core = cur_tuple.req_core;
  delay += _network->calc_hops(mc_ctrl, src_core); 
  assert(delay<CACHE_LATENCY);
  // add the hop latency between location of line_addr and src_core
  // int entry = (_cur_cache_delay_ptr + CACHE_LATENCY) % (CACHE_LATENCY+1);
  int entry = (_cur_cache_delay_ptr + delay) % (CACHE_LATENCY+1);
  _l2cache_delay_buffer[entry].push(a);
#endif
  */
}

void memory_controller::empty_delay_buffer()
{
  while (!_l2cache_delay_buffer[_cur_cache_delay_ptr].empty())
  {
    pending_mem_req a = _l2cache_delay_buffer[_cur_cache_delay_ptr].front();
    send_mem_request_actual(a.isWrite, a.line_addr, a.cur_tuple, VERTEX_SID);
    _l2cache_delay_buffer[_cur_cache_delay_ptr].pop();
  }
  _cur_cache_delay_ptr = (_cur_cache_delay_ptr + 1) % (CACHE_LATENCY + 1);
}

// TODO: send_mem_req should push in the pending queue instead of adding transaction -- prioritize the sid that has least amount of unused data... (pending requests for an sid should be equal to its approx. prefetch distance)
// TODO: shouldn't we have allocated completion buffer here?? otherwise those
// entries will be allocated unnecessarily. Why can't we do that here -- or for
// waiting_addr count associated with cb entries...? Only put a condition: do
// not allocate cb entry if the size of pending memory addr > X.
void memory_controller::schedule_pending_memory_addresses()
{
  for (int sid = 0; sid < MAX_STREAMS; ++sid)
  {
    while (!_pending_mem_addr[sid].empty())
    {
      uint64_t line_addr = _pending_mem_addr[sid].front().first;
      pref_tuple cur_tuple = _pending_mem_addr[sid].front().second;

      _mem->addTransaction(0, line_addr);
      // TODO: should allocate entry in the pending memory request here!!
      // assert than entry is not available

      list<pref_tuple> x;
      x.push_back(cur_tuple);
      _outstanding_mem_req.insert(make_pair(line_addr, x));
      auto it2 = _outstanding_mem_req.find(line_addr);
      assert(it2 != _outstanding_mem_req.end());
      _pending_mem_addr[sid].pop();
    }
  }
}

// this does not see an entry available here, but at the output, it consumes the current entry as well -- not possible
// entries are allocated, and then served it is popped
bool memory_controller::send_mem_request_actual(bool isWrite, uint64_t line_addr, pref_tuple cur_tuple, int streamid)
{
  // cout << "Sending memory request at cycle: " << _asic->_cur_cycle << endl;
  _stats->_stat_received_mem++;
  _stats->_stat_pending_colaesces++;
  // push into the delay buffer based on a flag

  auto it = _outstanding_mem_req.find(line_addr);
  if (it == _outstanding_mem_req.end())
  {
    /*if(line_addr>=_weight_offset) {
      cout << "Send a read request for addr: " << line_addr << " and requesting core: " << cur_tuple.req_core << endl;
    }*/
    int new_line_addr = line_addr; // | (_prev_chan << 5);
    // _prev_chan = (_prev_chan+1)%16;

    // TODO: here are the new cache lines, let's capture how many are issued each cycle
    // _transactions_this_cycle++;

    _mem->addTransaction(isWrite, new_line_addr);
    // cout << "Sending a real memory request at cycle: " << _asic->_cur_cycle << " line addr: " << new_line_addr << endl;
    // _mem_ctrl->_pending_mem_addr[streamid].push(make_pair(line_addr, cur_tuple));

    list<pref_tuple> x;
    x.push_back(cur_tuple);
    _outstanding_mem_req.insert(make_pair(line_addr, x));
    auto it2 = _outstanding_mem_req.find(line_addr);
    assert(it2 != _outstanding_mem_req.end());
    return true;
  }
  else
  {
    /*if(line_addr>=_weight_offset) {
      cout << "Found coalescer for a read request for addr: " << line_addr << " and requesting core: " << cur_tuple.req_core << endl;
    }*/
    _outstanding_mem_req[line_addr].push_back(cur_tuple);
    _stats->_stat_coalesced_mem++;
  }
  return false;
}

// Okay, messing up with the memory request ordering -- too specific to graph workloads
void memory_controller::receive_cache_miss(int line_addr, int req_core_id)
{
  // insert in cache
  int cur_prio_info = 0;
  if (_asic->_config->_special_cache == 1)
  {
    cur_prio_info = 200 - cur_prio_info;
  }
  if (_asic->_config->_lru)
  {
    cur_prio_info = _asic->_cur_cycle;
  }
  // there could be multiple requesting cores -- ideally should push in all
  update_cache_on_new_access(req_core_id, line_addr, cur_prio_info);
  // }
  // collect compulsory miss stats
  /* stats later
    int cache_line_id = line_addr;
    if(!_is_edge_hot_miss[cache_line_id]) { // if compulsory miss
      _asic->_stats->_stat_num_compulsory_misses++;
      _is_edge_hot_miss[cache_line_id]=true;
    } else { // hit in the cache: no need to go over the network
      // cout << "Hit pushed in the banks\n";
    }
    */
}

// TODO: maintain cur_prio_info based on some metric...
// associativity is not increasing cache size as well as cache hits (nothing changed) -- need it to insert somewhere else
void memory_controller::update_cache_on_new_access(int core_id, int edge_id, int cur_prio_info)
{
  bool cacheable = true; // conventional cache

  int associate_base_addr = edge_id; // -- this is based addr from now..// /16; // 64/4 = 16 entries
  int set_id = associate_base_addr % num_sets;
  int tag = (associate_base_addr >> (int)log(num_sets));

  // find eviction candidate from assoc entries
  int min_prio = INF;
  int entry_id = 0;
  for (int a = 0; a < assoc; ++a)
  {
    int cur_prio = _local_cache[core_id][set_id][a].meta_info.priority_info;
    if (cur_prio <= min_prio)
    { // for same prio, always replace 0
      min_prio = cur_prio;
      entry_id = a;
    }
  }
  struct cache_meta meta = _local_cache[core_id][set_id][entry_id].meta_info;
  int priority_info = meta.priority_info;
  // this information is present in the cur_tuple, so send this in func

  // cache if distance or 1/residual is smaller
  // don't cache if the current distance is larger
  if (_asic->_config->_special_cache)
  {
    if (priority_info != 0)
    {
      // need diff with respect to current low
      int diff = cur_prio_info - priority_info; // 88% cache hit rate (25% line uncached)
      int threshold = _asic->_cur_cycle * 1 / (float)epsilon;
      threshold = 0;
      // cout << "diff: " << cur_prio_info << " threshold: " << threshold << endl;
      if (diff > threshold)
        cacheable = false;
      // this is for degree
      // if(diff<threshold) cacheable=false;
    }
    // if(cur_prio_info<2*epsilon) cacheable=false;
  }

  // FIXME: what to do with free entries? (priority should be 0 then)
  if (cacheable)
  {

    // check if polluted entry
    if (_local_cache[core_id][set_id][entry_id].polluted_entry == true)
    {
      _stats->_stat_tot_unused_line++;
    }

    _stats->_stat_entry_cacheable++;

    vector<int> cache_line;
    vector<bool> first_access;
    int elem_ind = edge_id % 16;
    // insert data in the cache line (or could directly access from memory)
    for (int i = associate_base_addr; i < associate_base_addr + 16; ++i)
    {
      cache_line.push_back(i);
      if (i == associate_base_addr + elem_ind)
      {
        // false for the given index and true for else
        first_access.push_back(false); // not first access to scalar element
      }
      else
      {
        first_access.push_back(true); // not first access to scalar element
      }
    }
    cache_meta meta_info(tag, cur_prio_info);
    struct cache_set set;
    set.meta_info = meta_info;
    set.cache_line = cache_line;
    set.first_access = first_access;
    _first_access_cycle[edge_id] = _asic->_cur_cycle;
    set.polluted_entry = true;
    _local_cache[core_id][set_id][entry_id] = set;
  }
  else
  {
    _stats->_stat_entry_uncacheable++;
  }
}

bool memory_controller::is_cache_hit(int core, Addr_t paddr, UINT32 accessType, bool updateReplacement, UINT32 privateBankID)
{
  // return true;
  // TODO: what about other accesses to memory? -- need to adapt it for that...
  if (_asic->_config->_domain == graphs && (_asic->_config->_edge_cache == 0 && _asic->_config->_working_cache == 0))
  {
    return false;
  }
  bool hit = false;
  switch (_asic->_config->_cache_repl)
  {
  case phi:
    return _l2_cache->LookupAndFillCache(paddr, ACCESS_LOAD, true, 0);
    break;
  case allhits:
    return true;
    break;
  case allmiss:
    return false;
    break;
  default:
    break;
  }
  // TODO: it should still check for compulsory misses
  /*
  if(_asic->_is_edge_hot_miss[line_addr/line_size]) {
    hit=false;
    _asic->_is_edge_hot_miss[line_addr/line_size]=false;
  } else hit=true;
  */

  if (_banked)
  {
    hit = _banked_l2_cache[core]->LookupCache(paddr, accessType, updateReplacement, privateBankID); // check cache statistics
  }
  else
  {
    hit = _l2_cache->LookupCache(paddr, accessType, updateReplacement, privateBankID); // check cache statistics
  }
  return hit;

  // if hit, we serve the request right away
}

void memory_controller::load_graph_in_memory()
{
  int edges_per_cache_line = line_size / 8; // assuming 4 bytes line
  int total_lines = _asic->_graph_edges / edges_per_cache_line;
  for (int i = 0; i < total_lines; ++i)
  {
    int line_addr = i * line_size;
    pref_tuple x;
    send_mem_request(true, line_addr, x, EDGE_SID);
  }
  // memory barrier
  while (_cur_writes < total_lines)
  {
    _mem->update();
    // _cur_cycle++;
  }
  _cur_writes = 0;
}

// It should be ordered in vertex id.
uint64_t memory_controller::get_edge_addr(int edge_id, int vid)
{
  int correct_vid = _asic->_scratch_ctrl->_mapping[vid];
  edge_id = _asic->_mod_offset[correct_vid];
  uint64_t line_addr = (_edge_offset + edge_id / (line_size / message_size));
  return line_addr * line_size;
}

uint64_t memory_controller::get_vertex_addr(int vertex_id)
{
  int width = message_size;
  if (_asic->_config->is_vector())
  {
    width *= FEAT_LEN;
  }
  uint64_t line_addr = (_vertex_offset + vertex_id / (line_size / width));
  return line_addr * line_size;
}

void memory_controller::print_mem_ctrl_stats()
{
  // cout << "L2 hits: " << _l2hits << " hit rate: " << _l2hits/(double)_l2accesses << endl;
  assert(_l2hits >= 0 && "hits cannot be negative");
  cout << "L2 hit rate: " << _l2hits / (double)_l2accesses << endl;
  cout << "L2 accesses/edges: " << _l2accesses << " and size: " << L2SIZE << " kB" << endl;
}

memory_controller::memory_controller(asic *host, stats *stat) : _asic(host)
{
  for (int c = 0; c < core_cnt; ++c)
  {
    _l2hits_per_core[c] = 0;
    _l2accesses_per_core[c] = 0;
  }
  _l2hits = 0;
  _stats = stat;
  TransactionCompleteCB *read_cb = new Callback<memory_controller, void, unsigned, uint64_t, uint64_t>(this, &memory_controller::read_complete);
  TransactionCompleteCB *write_cb = new Callback<memory_controller, void, unsigned, uint64_t, uint64_t>(this, &memory_controller::write_complete);

  _mem = getMemorySystemInstance("ini/sgu_dram.ini", "sgu_system.ini.example", "DRAMSim2", "graphsim", MEMSIZE);
  _mem->RegisterCallbacks(read_cb, write_cb, memory_controller::power_callback);

  // FIXME: @vidushi: not sure if same association is possible for different
  // instances or not!!
  /*if(_asic->_config->_numa_mem) {
    for(int c=0; c<core_cnt; ++c) {
      TransactionCompleteCB *numa_read_cb = new Callback<memory_controller, void, unsigned, uint64_t, uint64_t>(this, &memory_controller::read_complete);
      TransactionCompleteCB *numa_write_cb = new Callback<memory_controller, void, unsigned, uint64_t, uint64_t>(this, &memory_controller::write_complete);

      _numa_mem = getMemorySystemInstance("ini/sgu_dram.ini", "sgu_system.ini.example", "DRAMSim2", "graphsim", NUMA_MEMSIZE);
      _numa_mem->RegisterCallbacks(numa_read_cb, numa_write_cb, memory_controller::power_callback);
    }
  }*/

  _edge_offset = 0;
  _vertex_offset = E * message_size;
  _vertex_offset = (_vertex_offset / line_size) * line_size;
  _weight_offset = _vertex_offset + V * message_size * FEAT_LEN;
  _weight_offset = (_weight_offset / line_size) * line_size;

#if BANKED == 1
  _banked = 1;
#endif
  if (!_banked)
  {
    _l2_cache = new SIMPLE_CACHE(L2SIZE * 1024, 8, 1, 64, 0, 1);
  }
  else
  {
    for (int c = 0; c < core_cnt; ++c)
    {
      _banked_l2_cache[c] = new SIMPLE_CACHE(L2SIZE * 1024 / core_cnt, 8, 1, 64, 0, 1);
    }
  }
}

// we assumed perfect locality
void scratch_controller::perform_linear_load_balancing(int slice_id)
{
  int degree_per_core[core_cnt];

  for (int i = 0; i < core_cnt; ++i)
    degree_per_core[i] = 0;

  int start_vert = _slice_size * slice_id;
  int end_vert = start_vert + _slice_size;

  int tot_vert_per_core = _slice_size / core_cnt;
  for (unsigned i = start_vert; i < end_vert; ++i)
  {
    int core = i / _slice_size;
    _mapping[i] = i - start_vert; // core*tot_vert_per_core + i%_graph_vertices;
    degree_per_core[core] += (_asic->_offset[i + 1] - _asic->_offset[i]);
  }

  float avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "Linear LB out factor: " << max / (float)(avg / (float)core_cnt) << endl;

  for (unsigned i = start_vert; i < end_vert; ++i)
  {
    int core = i / _slice_size;
    // _mapping[i] = core*tot_vert_per_core + i%_graph_vertices;
    degree_per_core[core] += _in_degree[i];
  }

  avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "Linear LB in factor: " << max / (float)(avg / (float)core_cnt) << endl;
  locality_test(slice_id);
}

// TODO: can we similarly try BFS or BDFS?
void scratch_controller::perform_bdfs_load_balancing(int slice_id)
{
  // Let's see how to perform this mapping...
  // would like to fill the mapping array...
  // run dfs only on the current slice
  int start_vert = _slice_size * slice_id;
  int end_vert = start_vert + _slice_size;

  // TODO: if bfs version, I can just malloc bfs_visited and mapping_bfs here..
  if (_asic->_config->_spatial_part == bbfs)
  {
    _mapping_bfs = (int *)malloc(_asic->_graph_vertices * sizeof(int));
    _bfs_visited = (bool *)malloc(_asic->_graph_vertices * sizeof(bool));
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      _bfs_visited[i] = false;
      // cout << "Set i: " << i << " to: " << _bfs_visited[i] << endl;
    }

    // DFS(start_vert, slice_id); // this is spatial, local to a slice...
    // cout << "vertices visited after slice: " << slice_id << " is: " << _vertices_visited << endl;
    _vertices_visited = 0;
    /*while(_vertices_visited<_slice_size && start_vert<end_vert) {
      // DFS(start_vert, slice_id);
      // cout << "Start vert: " << start_vert << " and slice: " << slice_id << " is it visited: " << _bfs_visited[start_vert] << endl;
      BFS(start_vert, slice_id);
      for(int i=start_vert; i<end_vert; ++i) {
        // if(!_visited[i]) {
        if(!_bfs_visited[i]) {
          start_vert=i;
          break; // continue;
        }
      }
    }*/
    for (int i = start_vert; i < end_vert; ++i)
    {
      if (!_bfs_visited[i])
      {
        _mapping_bfs[i] = ++_global_ind;
        _bfs_visited[i] = true;
        _mapping_new_to_old[_global_ind] = i;
      }
      _mapping[i] = _mapping_bfs[i];
      // cout << "mapping of vertex i: " << i << " is: " << _mapping[i] << endl;
    }
  }
  else
  {
    // #else // use the depth-first search
    // spatial partitioning for each slice...
    _vertices_visited = 0;
    DFS(start_vert, slice_id);
    /*while(_vertices_visited<_slice_size && start_vert<end_vert) {
      cout << "Entered with start_vert: " << start_vert << " end vert: " << end_vert << " vertices visited: " << _vertices_visited << endl;
      DFS(start_vert, slice_id);
      // if we are not calling dfs again (EITHER THIS OR THE NEXT LOOP)
      for(int i=start_vert; i<end_vert; ++i) {
        if(!_visited[i]) {
          start_vert=i;
          break; 
        }
      }
    }*/
    for (int i = start_vert; i < end_vert; ++i)
    {
      if (!_visited[i])
      {
        _mapping[i] = ++_global_ind;
        _visited[i] = true;
        _mapping_new_to_old[_global_ind] = i;
      }
    }
  }
  cout << "vertices visited after slice: " << slice_id << " and start vert: " << start_vert << " is: " << _vertices_visited << endl;
}

void scratch_controller::optimize_load()
{
  if (_asic->_config->_noload)
    return;

  // renaming required only when new mapping array is going to be created

  // remapping required only if no metis file..
  if (metis_file != "")
  {
    // if(1) {
    int cur_e = -1;
    int cur_v = -1;
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      int old_vid = _mapping_new_to_old[i];

      if (old_vid == _asic->_src_vid)
      {
        _asic->_src_vid = i;
        cout << "new src_vid is: " << i << endl;
      }

      for (int j = _asic->_offset[old_vid]; j < _asic->_offset[old_vid + 1]; ++j)
      {
        _stats->_stat_extra_finished_edges[++cur_e] = _mapping[_asic->_neighbor[j].dst_id];
        _temp_edge_weights[cur_e] = _asic->_neighbor[j].wgt;
      }
      _num_spec[++cur_v] = cur_e + 1;
    }

    while (cur_v < _asic->_graph_vertices)
      _num_spec[++cur_v] = cur_e + 1;

    cout << "Done storing temporary graph in the order of locality-aware partitioning\n";

    for (int i = 1; i <= _asic->_graph_vertices; ++i)
    {
      _asic->_offset[i] = _num_spec[i - 1];
      if (_asic->_config->_algo != cf)
      {
        _asic->_vertex_data[i - 1] = _asic->_scratch[i - 1];
      }
      assert(_asic->_offset[i] >= _asic->_offset[i - 1] && "do not follow the basic condition for offset");
      ;
    }

    cur_e = -1;
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      for (int j = _asic->_offset[i]; j < _asic->_offset[i + 1]; ++j)
      {
        _asic->_neighbor[++cur_e].src_id = i;
        _asic->_neighbor[cur_e].dst_id = _stats->_stat_extra_finished_edges[cur_e];
        _asic->_neighbor[cur_e].wgt = _temp_edge_weights[cur_e];
        // _asic->_neighbor[cur_e].done = false;
        assert(j == cur_e && "traversing edges in sequence");
        // cout << "cur e: " << cur_e << " edge traversed: " << _stats->_stat_extra_finished_edges[cur_e] << "dst at i: " << _neighbor[cur_e].dst_id << endl;
        // _stats->_stat_extra_finished_edges[cur_e] = 0;
      }
    }

    cout << "Done modifying the original graph data-structure\n";
    _asic->init_vertex_data();
    // cout << "New src vid: " << _src_vid << " vertex data: " << _vertex_data[_src_vid] << " scratch: " << _scratch[_src_vid] << endl;

    free(_temp_edge_weights);
    free(_stats->_stat_extra_finished_edges);
    free(_mapping_new_to_old);
    _asic->fill_correct_vertex_data();
  }
  for (int i = 0; i < _asic->_graph_vertices; ++i)
    _mapping[i] = i;

  // Step2: then call perform load balancing
  // this is spatial mapping..graph is not renamed so no problem
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    perform_load_balancing2(i);
  }
}

// we assumed no locality
void scratch_controller::perform_modulo_load_balancing(int slice_id)
{
  int degree_per_core[core_cnt];

  for (int i = 0; i < core_cnt; ++i)
    degree_per_core[i] = 0;

  int start_vert = _slice_size * slice_id;
  int end_vert = start_vert + _slice_size;

  int tot_vert_per_core = _slice_size / core_cnt;
  for (unsigned i = start_vert; i < end_vert; ++i)
  {
    int corr_v = i;          // i%core_cnt;
    int core = i % core_cnt; // i/_graph_vertices;
    // _mapping[i] = core*tot_vert_per_core + i%_graph_vertices;
    degree_per_core[core] += (_asic->_offset[corr_v + 1] - _asic->_offset[corr_v]);
  }

  float avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "Modulo LB out factor: " << max / (float)(avg / (float)core_cnt) << endl;

  avg = 0, max = 0;
  for (unsigned i = start_vert; i < end_vert; ++i)
  {
    int corr_v = i;          // i%core_cnt;
    int core = i % core_cnt; // i/_graph_vertices;
    _mapping[i] = core * tot_vert_per_core + (i - start_vert) / core_cnt;
    degree_per_core[core] += _in_degree[corr_v];
  }
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "Modulo LB in factor: " << max / (float)(avg / (float)core_cnt) << endl;

  locality_test(slice_id);

  // TODO: calculate in-degree load balance factor..
}

// use mapping_dfg to fill in mapping...
// dynamic is better (task issue near data calls for static
// partitioning_hence we kind-of-metis)

/*Does prioritizing in-degree helped it? doesn't seem so
 * What is the impact of low degree threshold? higher number creates variance
 * in in-degree and out-degree?
 *Looks like we obtained a midpoint between locality and load imbalance
 *Does this holds true for metis partitions? it helps more (a more serious issue in this case)
 *Do we need dfs or linear is good enough? I think it is okay..
 */

// we assumed decent locality

// do not rename, only change mapping (local_scratch_id considers that)
// FIXME: some issue here (creating problem for indoChina graph)
void scratch_controller::perform_load_balancing2(int slice_id)
{
  int low_degree_threshold = 10;
  int medium_degree_threshold = 50;

  int count_low_degree = 0;
  int count_high_out_degree = 0;
  int count_med_out_degree = 0; // prefer this...

  int start_vert = _slice_size * slice_id;
  int end_vert = start_vert + _slice_size;

  int degree_per_core[core_cnt];

  for (int i = 0; i < core_cnt; ++i)
    degree_per_core[i] = 0;

  float out_degree_lb = 0;
  float in_degree_lb = 0;

  for (int i = start_vert; i < end_vert; ++i)
  {
    int out_degree = _asic->_offset[i + 1] - _asic->_offset[i];
    if (out_degree < low_degree_threshold)
    {
      low_degree.push_back(i);
      count_low_degree++;
    }
    else
    {
      if (out_degree < medium_degree_threshold)
      {
        high_inc_degree.push_back(i);
        count_med_out_degree++;
      }
      else
      {
        high_out_degree.push_back(i);
        count_high_out_degree++;
      }
    }
  }

  cout << "Categorization done, low degree nodes: " << count_low_degree << " medium out degree: " << count_med_out_degree << " high out degree: " << count_high_out_degree << endl;

  int tot_vert_per_core = _slice_size / core_cnt;

  // Now start allocation
  // Step1: evenly distribute low degree vertices
  int low_size = ceil(count_low_degree / (float)core_cnt);
  for (unsigned i = 0; i < low_degree.size(); ++i)
  {
    int core = i / low_size;
    _mapping[low_degree[i]] = core * tot_vert_per_core + i % low_size;
    degree_per_core[core] += (_asic->_offset[low_degree[i] + 1] - _asic->_offset[low_degree[i]]);
  }

  float avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }
  cout << "Low degree LB factor: " << max / (float)(avg / (float)core_cnt) << endl;

  // Step3: calculate the lb
  // Step2: distribute high out degree uniformly
  int high_inc_size = ceil(count_med_out_degree / (float)core_cnt);
  for (unsigned i = 0; i < high_inc_degree.size(); ++i)
  {
    int core = i / high_inc_size;
    _mapping[high_inc_degree[i]] = core * tot_vert_per_core + i % high_inc_size + low_size; // + high_out_size;
    degree_per_core[core] += _in_degree[high_inc_degree[i]];
  }

  // get avg and max
  avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "High degree In LB factor: " << max / (float)(avg / (float)core_cnt) << endl;

  // Step2: find partitions where lb for out-degree is less than 2
  // Step2: distribute high out degree uniformly
  int high_out_size = ceil(count_high_out_degree / (float)core_cnt);
  for (unsigned i = 0; i < high_out_degree.size(); ++i)
  {
    int core = i / high_out_size;
    _mapping[high_out_degree[i]] = core * tot_vert_per_core + i % high_out_size + low_size + high_inc_size;
    degree_per_core[core] += (_asic->_offset[high_out_degree[i] + 1] - _asic->_offset[high_out_degree[i]]);
  }

  // get avg and max
  avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "High degree Out LB factor: " << max / (float)(avg / (float)core_cnt) << endl;

  locality_test(slice_id);
  low_degree.clear();
  high_inc_degree.clear();
  high_out_degree.clear();
}

void scratch_controller::perform_load_balancing(int slice_id)
{

  int low_degree_threshold = 50; // 15;

  int count_low_degree = 0;
  int count_high_out_degree = 0;
  int count_high_inc_degree = 0; // prefer this...

  int start_vert = _slice_size * slice_id;
  int end_vert = start_vert + _slice_size;

  vector<int> low_degree;
  vector<int> high_inc_degree;
  vector<int> high_out_degree;
  int degree_per_core[core_cnt];

  for (int i = 0; i < core_cnt; ++i)
    degree_per_core[i] = 0;

  float out_degree_lb = 0;
  float in_degree_lb = 0;

  // this is to be done inside a metis cluster (would a nested level metis not
  // work -- may be NO because their goal is just locality)
  // put constraint on one and solve other -- multi-objective function
  // eg. out-degree lb < 2 and try to minimize in-degree-lb
  /*for(int i=start_vert; i<end_vert; ++i) {
    int out_degree = _offset[i+1]-_offset[i];
    int in_degree = _in_degree[i];
    if(in_degree>low_degree_threshold) {
      high_inc_degree.push_back(i);
      count_high_inc_degree++;
    }
    else {
      if(out_degree<low_degree_threshold) {
        low_degree.push_back(i);
        count_low_degree++;
      }
      else {
        high_out_degree.push_back(i);
        count_high_out_degree++;
      }
    }
  }*/

  for (int i = start_vert; i < end_vert; ++i)
  {
    int out_degree = _asic->_offset[i + 1] - _asic->_offset[i];
    int in_degree = _in_degree[i];
    if (in_degree > low_degree_threshold)
    {
      high_inc_degree.push_back(i);
      count_high_inc_degree++;
    }
    else
    {
      if (out_degree < low_degree_threshold)
      {
        low_degree.push_back(i);
        count_low_degree++;
      }
      else
      {
        high_out_degree.push_back(i);
        count_high_out_degree++;
      }
    }
  }

  cout << "Categorization done, low degree nodes: " << count_low_degree << " high in degree: " << count_high_inc_degree << " high out degree: " << count_high_out_degree << endl;

  int tot_vert_per_core = _slice_size / core_cnt;

  // Now start allocation
  // Step1: evenly distribute low degree vertices
  int low_size = count_low_degree / core_cnt;
  for (unsigned i = 0; i < low_degree.size(); ++i)
  {
    int core = i / low_size;
    _mapping[low_degree[i]] = core * tot_vert_per_core + i % low_size;
    degree_per_core[core] += (_asic->_offset[low_degree[i] + 1] - _asic->_offset[low_degree[i]]);
  }

  float avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }
  cout << "Low degree LB factor: " << max / (float)(avg / (float)core_cnt) << endl;

  // Step3: calculate the lb
  // Step2: distribute high out degree uniformly
  int high_inc_size = ceil(count_high_inc_degree / (float)core_cnt);
  for (unsigned i = 0; i < high_inc_degree.size(); ++i)
  {
    int core = i / high_inc_size;
    _mapping[high_inc_degree[i]] = core * tot_vert_per_core + i % high_inc_size + low_size; // + high_out_size;
    degree_per_core[core] += _in_degree[high_inc_degree[i]];
  }

  // get avg and max
  avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "High degree In LB factor: " << max / (float)(avg / (float)core_cnt) << endl;

  // Step2: find partitions where lb for out-degree is less than 2
  // Step2: distribute high out degree uniformly
  int high_out_size = count_high_out_degree / core_cnt;
  for (unsigned i = 0; i < high_out_degree.size(); ++i)
  {
    int core = i / high_out_size;
    _mapping[high_out_degree[i]] = core * tot_vert_per_core + i % high_out_size + low_size + high_inc_size;
    degree_per_core[core] += (_asic->_offset[high_out_degree[i] + 1] - _asic->_offset[high_out_degree[i]]);
  }

  // get avg and max
  avg = 0, max = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    avg += degree_per_core[i];
    if (degree_per_core[i] > max)
      max = degree_per_core[i];
    degree_per_core[i] = 0;
  }

  cout << "High degree Out LB factor: " << max / (float)(avg / (float)core_cnt) << endl;

  locality_test(slice_id);
  low_degree.clear();
  high_inc_degree.clear();
  high_out_degree.clear();
}

void scratch_controller::print_mapping()
{
#if SGU_SLICING == 0
  return;
#endif
  for (int slice_id = 0; slice_id < _asic->_slice_count; ++slice_id)
  {
    int start_vert = _slice_size * slice_id;
    int end_vert = start_vert + _slice_size;
    for (int j = start_vert; j < end_vert; ++j)
    {
      cout << "vertex: " << j << " mapping: " << _mapping[j] << " mapped core: " << get_local_scratch_id(j) << endl;
    }
  }
  exit(0);
}

void scratch_controller::load_balance_test()
{
  cout << "Came in to calculate the amount of load balance in this partition\n";
  int degree_per_core[core_cnt];
  for (int i = 0; i < core_cnt; ++i)
    degree_per_core[i] = 0;
  int count = _asic->_slice_count;
#if SGU_SLICING == 0 && GRAPHMAT_SLICING == 0 && BLOCKED_ASYNC == 0
  count = 1;
#endif
  for (int slice_num = 0; slice_num < count; ++slice_num)
  {
    for (int s = 0; s < _slice_size; ++s)
    {
      for (int d = _asic->_offset[s]; d < _asic->_offset[s + 1]; ++d)
      {
        int dst_slice = get_slice_num(_asic->_neighbor[d].dst_id);
        if (dst_slice != slice_num)
          continue;
        int core = get_local_scratch_id(_asic->_neighbor[d].dst_id);
        degree_per_core[core]++; // = _in_degree[s]; // for edges in this core
      }
    }
    float avg = 0, max = 0;
    for (int i = 0; i < core_cnt; ++i)
    {
      avg += degree_per_core[i];
      if (degree_per_core[i] > max)
        max = degree_per_core[i];
      degree_per_core[i] = 0;
    }

    cout << "In-degree slice: " << _slice_num << " LB factor: " << max / (float)(avg / (float)core_cnt) << endl;
  }

  for (int slice_num = 0; slice_num < count; ++slice_num)
  {
    for (int s = 0; s < _slice_size; ++s)
    {
      for (int d = _asic->_offset[s]; d < _asic->_offset[s + 1]; ++d)
      {
        int dst_slice = get_slice_num(_asic->_neighbor[d].dst_id);
        if (dst_slice != slice_num)
          continue;
        int core = get_local_scratch_id(s);
        degree_per_core[core]++; // = _in_degree[s]; // for edges in this core
      }
    }
    float avg = 0, max = 0;
    for (int i = 0; i < core_cnt; ++i)
    {
      avg += degree_per_core[i];
      if (degree_per_core[i] > max)
        max = degree_per_core[i];
      degree_per_core[i] = 0;
    }

    cout << "Out-degree slice: " << slice_num << " LB factor: " << max / (float)(avg / (float)core_cnt) << endl;
  }

  // exit(0);
}

// test for how many source and destinations are in the same core
// go through all the the edges and note their core differences
void scratch_controller::locality_test(int slice_id)
{
#if SGU_SLICING == 0 && GRAPHMAT_SLICING == 0 && BLOCKED_ASYNC == 0
  return;
#endif
  int src_core = 0, dest_core = 0;
  int local_count = 0, remote_count = 0;

  int start_vert = _slice_size * slice_id;
  int end_vert = start_vert + _slice_size;

#if GCN == 0 || LADIES_GCN == 1
  for (int i = start_vert; i < end_vert; ++i)
  {
#else
  for (int i = 0; i < MINIBATCH_SIZE; ++i)
  {
#endif
    src_core = get_local_scratch_id(i);
    for (int j = _asic->_offset[i]; j < _asic->_offset[i + 1]; ++j)
    {
      dest_core = get_local_scratch_id(_asic->_neighbor[j].dst_id);
      // cout << "src_core: " << src_core << " dest_core: " << dest_core << endl;
      if (src_core == dest_core)
      {
        local_count++;
        // _asic->_neighbor[j].hops = 0;
      }
      else
      {
        // TODO: use if specific policy is active
        // _asic->_neighbor[j].hops = _asic->_network->calc_hops(src_core, dest_core);
        remote_count++;
      }
    }
  }
  if (remote_count == 0 && core_cnt != 1)
    assert(0 && "mapping is incorrect");
  cout << "Ratio of remote to local (lower is better): " << remote_count / (double)local_count << endl;
}

void scratch_controller::linear_mapping()
{
  cout << "Linear mapping is switched on\n";
#if GCN == 1 && LADIES_GCN == 0
  for (int i = 0; i < MINIBATCH_SIZE; ++i)
  {
    _asic->_gcn_minibatch[i] = i;
#elif LADIES_GCN == 1
  for (int i = 0; i < LADIES_SAMPLE; ++i)
  {
#else
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
#endif
    _mapping[i] = i;
    // _mapping[i]=rand()%_graph_vertices;
  }
  _vertices_visited = _asic->_graph_vertices;
  _edges_visited = E;
#if GCN == 1
  _vertices_visited = MINIBATCH_SIZE;
#endif
}

scratch_controller::scratch_controller(asic *host, stats *stat) : _asic(host)
{
  _stats = stat;
  _map_grp_to_core = (int *)malloc(core_cnt * sizeof(int));
  _num_boundary_nodes_per_slice = (int *)malloc(SLICE_COUNT * sizeof(int));

#if SGU_SLICING == 1 || SGU == 1 || GRAPHMAT_SLICING == 1 || GRAPHMAT == 1 || BLOCKED_ASYNC == 1
  _slice_size = ceil(_asic->_graph_vertices / (double)_asic->_slice_count);
  if (metis_file != "")
  {
    _asic->_metis_slice_count = METIS_SLICE_COUNT;
    _metis_slice_size = ceil(_asic->_graph_vertices / (double)_asic->_metis_slice_count);

    _temp_edge_weights = (int *)malloc(E * sizeof(int));
    _mapping_old_to_new = (int *)malloc(_asic->_graph_vertices * sizeof(int));
    _mapping_new_to_old = (int *)malloc(_asic->_graph_vertices * sizeof(int));
    _slice_num = (uint16_t *)malloc(_asic->_graph_vertices * sizeof(uint16_t));
    int per_core = 1.1 * _asic->_graph_vertices / _asic->_slice_count;
    for (int i = 0; i < _asic->_slice_count; ++i)
    {
      _slice_vids[i] = (int *)malloc(per_core * sizeof(int));
    }
  }
  else
  {
#if BDFS == 1
    _mapping_new_to_old = (int *)malloc(_asic->_graph_vertices * sizeof(int));
    _temp_edge_weights = (int *)malloc(E * sizeof(int));
#endif
  }
  _in_degree = (int *)malloc(_asic->_graph_vertices * sizeof(int));
  for (int j = 0; j < _asic->_graph_vertices; ++j)
    _in_degree[j] = 0;
#endif

#if SGU_SLICING == 0 && GRAPHMAT_SLICING == 0 && BLOCKED_ASYNC == 0
  _slice_size = _asic->_graph_vertices;
#endif
  for (int b = 0; b < num_banks; ++b)
  {
    _present_in_bank_queue[b].reset();
  }

  _mapping = (int *)malloc(_asic->_graph_vertices * sizeof(int));
#if RANDOM_SPATIAL == 1
  _mapped_core = (int *)malloc(_asic->_graph_vertices * sizeof(int));
#endif

#if BFS_MAP == 1
  _mapping_bfs = (int *)malloc(_asic->_graph_vertices * sizeof(int));
#endif

#if DFSMAP == 1
  _mapping_dfs = (int *)malloc(_asic->_graph_vertices * sizeof(int));
#endif
  _is_visited.reset();
  _is_visited.set(_asic->_src_vid); // set source to visited by default

#if BFS_MAP == 1
  _bfs_visited = (bool *)malloc(_asic->_graph_vertices * sizeof(bool));
#endif
  _visited = (bool *)malloc(_asic->_graph_vertices * sizeof(bool));
  _num_spec = (int *)malloc((_asic->_graph_vertices + 1) * sizeof(int));

  // Mark all the vertices as not visited -- for dfs
  for (int i = 0; i < _asic->_graph_vertices; i++)
    _visited[i] = false;
#if GRAPHMAT == 1 || BLOCKED_ASYNC == 1
  _num_slices_it_is_copy_vertex = (int *)malloc(_asic->_graph_vertices * sizeof(int));
#endif
}

// idea is to reduce the number of hop counts between remote accesses
// TODO: Let me try different spatial locations of the partition
// I want to reduce hops for the groups that have higher connectivity among them
// Also would like to pipeline stuff? (same as above, connection on the closeby link)
void scratch_controller::map_grp_to_core()
{
  // Initial mapping is linear, after this we need to check..
  for (int i = 0; i < core_cnt; ++i)
  {
    _map_grp_to_core[i] = i;
  }
  return;
  // edge cuts between groups
  int edge_cuts[core_cnt][core_cnt];
  for (int i = 0; i < core_cnt; ++i)
  {
    for (int j = 0; j < core_cnt; ++j)
    {
      edge_cuts[i][j] = 0;
    }
  }
  int g1, g2;

  for (int i = 0; i < _asic->_graph_edges; ++i)
  {
    int src_id = _asic->_neighbor[i].src_id;
    int dst_id = _asic->_neighbor[i].dst_id;
#if PROFILING == 1
    g1 = _map_vertex_to_core[src_id];
    g2 = _map_vertex_to_core[dst_id];
    if (g1 == -1 || g2 == -1)
      continue;
#else
    g1 = get_grp_id(src_id);
    g2 = get_grp_id(dst_id);
#endif
    edge_cuts[g1][g2]++;
  }

  // now need to fill _map_grp_to_core;
  // First check the variance
  for (int i = 0; i < core_cnt; ++i)
  {
    for (int j = 0; j < core_cnt; ++j)
    {
      if (i != j)
        cout << i << " " << j << ": " << edge_cuts[i][j] << endl;
    }
  }
  // exit(0);
  // for flickr
  _map_grp_to_core[0] = 0;
  _map_grp_to_core[1] = 2;
  _map_grp_to_core[2] = 4;
  _map_grp_to_core[3] = 8;
  _map_grp_to_core[4] = 7;
  _map_grp_to_core[5] = 15;
  _map_grp_to_core[6] = 6;
  _map_grp_to_core[7] = 11;
  _map_grp_to_core[8] = 3;
  _map_grp_to_core[9] = 14;
  _map_grp_to_core[10] = 1;
  _map_grp_to_core[11] = 5;
  _map_grp_to_core[12] = 9;
  _map_grp_to_core[13] = 10;
  _map_grp_to_core[14] = 12;
  _map_grp_to_core[15] = 13;

  // Let me try to map my way
  vector<int> affinity_node;
  int old_order[core_cnt];
  for (int i = 0; i < core_cnt; ++i)
  {
    _map_grp_to_core[i] = -1;
    int sum = 0;
    for (int j = 0; j < core_cnt; ++j)
    {
      sum += (edge_cuts[i][j] + edge_cuts[j][i]);
    }
    affinity_node.push_back(sum);
    old_order[i] = sum;
  }

  sort(affinity_node.begin(), affinity_node.end());
  int first_maps[4];
  // assuming mesh for now, inner circle is 4 cores.
  for (int i = 0; i < 4; ++i)
  {
    for (int j = 0; j < core_cnt; ++j)
    {
      if (old_order[j] == affinity_node[i])
      {
        _map_grp_to_core[old_order[j]] = (i / 2) * num_rows + 5 + i % 2; // 5,6,9,10
        first_maps[i] = j;
      }
    }
  }

  // now mapping to the outer circle (currently only 2 circles)
  // Start with 5 (find all its most affinity nodes that are not mapped already)
  int cur_max_cut = 0, cur_allotted_core = -1;
  for (int central = 0; central < 4; ++central)
  {
    for (int round = 0; round < 3; ++round)
    {
      for (int i = 0; i < core_cnt; ++i)
      {
        if (_map_grp_to_core[i] != -1)
          continue;
        if (edge_cuts[first_maps[central]][i] > cur_max_cut)
        {
          cur_max_cut = edge_cuts[first_maps[central]][i];
          cur_allotted_core = i;
        }
      }
      // derive RHS from central and round
      if (central == 0)
      {
        _map_grp_to_core[cur_allotted_core] = round + (round / 2) * 2; // 0, 1, 4
      }
      else if (central == 1)
      {
        _map_grp_to_core[cur_allotted_core] = central * 2 + round + (round / 2) * (central + 2); // 2, 3, 7
      }
      else if (central == 2)
      {
        _map_grp_to_core[cur_allotted_core] = 8 + (central % 2) * 3 + (round == 0 ? 0 : 1) * (num_rows) + round / 2; // 8, 12, 13
      }
      else if (central == 3)
      {
        _map_grp_to_core[cur_allotted_core] = 8 + (central % 2) * 3 + (round == 0 ? 0 : 1) * (num_rows) + round / 2; // 11, 14, 15
      }
    }
  }

  return;

  // Step 1: sort (and place them at 0 and in neighbors: (i+1), i+num_rows, (i-1), i-num_rows)
  // for each node, consider only which one among them suits best
  // like greedy algorithm? (I am pretty sure some algorithm like this exists in literature)
  // different combinations would be too many...
  // also, here we consider all edges to be the same weight, if we consider this case while eviction
  // profiling could learn this as well...

  // FIXME: this should not affect remote/local ratio at all (just reduced
  // number of hop count -- which seems to help from other experiment) although
  int weighted_sum = 0;
  for (int i = 0; i < core_cnt; ++i)
  {
    for (int j = 0; j < core_cnt; ++j)
    {
      int hop_count = _asic->_network->calc_hops(_map_grp_to_core[i], _map_grp_to_core[j]);
      int cuts = edge_cuts[_map_grp_to_core[i]][_map_grp_to_core[j]];
      if (hop_count == 0)
        continue;
      // cout << hop_count << ": " << cuts << endl;
      weighted_sum += hop_count * cuts;
    }
  }
  // this is like a loss function to test different mappings, for all those,
  // hop counts would change, we would like maximum hop counts for minimum cuts to minimize this
  // cuts are fixed.
  cout << "Weighted sum: " << weighted_sum / 64 << endl;

  /*for(int i=12; i<16; ++i) {
    // _map_grp_to_core[i]=i-8;
    _map_grp_to_core[i]=19-i;
    _map_grp_to_core[i-8]=19-i;
    // _map_grp_to_core[i-8]=i;
  }*/

  /*
  weighted_sum=0; 
  for(int i=0; i<core_cnt; ++i) {
    for(int j=0; j<core_cnt; ++j) {
      int hop_count = _network->calc_hops(_map_grp_to_core[i],_map_grp_to_core[j]);
      int cuts = edge_cuts[_map_grp_to_core[i]][_map_grp_to_core[j]];
      if(hop_count==0) continue;
      // cout << hop_count << ": " << cuts << endl;
      weighted_sum += hop_count*cuts;
    }
  }

  cout << "Weighted sum: " << weighted_sum/64 << endl;
  */
  // exit(0);
}

// prints all not yet visited vertices reachable from s
void scratch_controller::bdfs_algo(int s)
{
  // Initially mark all verices as not visited
  // vector<bool> visited(_graph_vertices, false);

  // Create a stack for DFS
  deque<int> stack;
  int cur_depth = 0;

  // Push the current source node.
  stack.push_front(s);

  while (!stack.empty())
  {
    // Pop a vertex from stack and print it
    // s = stack.top();
    s = stack.front();
    // stack.pop();
    stack.pop_front();

    // Stack may contain same vertex twice. So
    // we need to print the popped item only
    // if it is not visited.
    if (!_visited[s])
    {
      // cout << s << " ";
      _mapping[s] = ++_global_ind;
      _mapping_dfs[s] = _mapping[s];
      _visited[s] = true;
      cur_depth++;
    }

    // Get all adjacent vertices of the popped vertex s
    // If a adjacent has not been visited, then puah it
    // to the stack.

    for (int i = _asic->_offset[s]; i < _asic->_offset[s + 1]; ++i)
    {
      if (!_visited[_asic->_neighbor[i].dst_id])
      {
        // cout << _neighbor[i].dst_id << " ";
        // if(cur_depth<10) {
        /*if(cur_depth>2) {
              stack.push_front(_neighbor[i].dst_id); 
            } else {
              cur_depth=0;
              stack.push_back(_neighbor[i].dst_id); 
            }*/

        if (cur_depth <= BD)
        {
          stack.push_front(_asic->_neighbor[i].dst_id);
        }
        else
        {
          // cur_depth=0;
          stack.push_back(_asic->_neighbor[i].dst_id);
        }
      }
    }
    if (cur_depth > BD)
      cur_depth = 0;
    // cout << "\n";
  }
}

void scratch_controller::BDFSUtil(int v)
{
  // Mark the current node as visited and
  _visited[v] = true;
  // _mapping[++_global_ind] = v;
  _mapping[v] = ++_global_ind;

  // Recur for all the vertices adjacent to this vertex
  for (int i = _asic->_offset[v]; i < _asic->_offset[v + 1]; ++i)
  {
    if (!_visited[_asic->_neighbor[i].dst_id])
    {
      BDFSUtil(_asic->_neighbor[i].dst_id);
    }
  }
}

// prints all not yet visited vertices reachable from s
void scratch_controller::DFS(int s, int slice_id)
{
  // Initially mark all verices as not visited
  // vector<bool> visited(_graph_vertices, false);

  // cout << "Starting dfs with start vertex s: " << s << " and slice id: " << slice_id << " and size: " << _slice_size << " and start vertices visited: " << _vertices_visited << endl;
  // Create a stack for DFS
  stack<int> stack;

  // Push the current source node.
  stack.push(s);

  while (!stack.empty())
  {
    // Pop a vertex from stack and print it
    s = stack.top();
    stack.pop();

    // Stack may contain same vertex twice. So
    // we need to print the popped item only
    // if it is not visited.
    if (!_visited[s])
    {
      assert(slice_id == get_slice_num(s));
      _vertices_visited++;
      _edges_visited += (_asic->_offset[s + 1] - _asic->_offset[s]);
#if GCN == 1
      _asic->_gcn_minibatch[_vertices_visited - 1] = s;
      // cout << "vertices visitied: " << _vertices_visited << " and vertex here: " << s << endl;
      if (_vertices_visited > MINIBATCH_SIZE)
        return;
#endif
      _mapping[s] = ++_global_ind;
      _mapping_new_to_old[_global_ind] = s;
      // cout << "slice id: " << slice_id << " vertex: " << s << " " << " and mapping: " << _mapping[s] << endl;
#if DFSMAP == 1
      _mapping_dfs[s] = _mapping[s];
#endif
      _visited[s] = true;
    }

    // Get all adjacent vertices of the popped vertex s
    // If a adjacent has not been visited, then push it
    // to the stack.
    for (int i = _asic->_offset[s]; i < _asic->_offset[s + 1]; ++i)
    {
      int dest_slice = get_slice_num(_asic->_neighbor[i].dst_id);
      if (dest_slice != slice_id)
        continue;
      if (!_visited[_asic->_neighbor[i].dst_id])
      {
        // cout << _neighbor[i].dst_id << " ";
        stack.push(_asic->_neighbor[i].dst_id);
      }
    }
    // cout << "\n";
  }
  // cout << vertices visited during dfs are: " << _vertices_visited << " and edges: " << _edges_visited << endl;
}

void scratch_controller::DFSUtil(int v)
{
  // Mark the current node as visited and
  _visited[v] = true;
  // _mapping[++_global_ind] = v;
  _mapping[v] = ++_global_ind;

  // Recur for all the vertices adjacent to this vertex
  for (int i = _asic->_offset[v]; i < _asic->_offset[v + 1]; ++i)
  {
    if (!_visited[_asic->_neighbor[i].dst_id])
    {
      DFSUtil(_asic->_neighbor[i].dst_id);
    }
  }
}

// DFS traversal of the vertices reachable from v.
// It uses recursive DFSUtil()
// FIXME: problem in renaming...
/*void asic::DFS(int v)
{

    // Call the recursive helper function
    // to print DFS traversal
    DFSUtil(v);
}
*/

void scratch_controller::BFS(int s, int slice_id)
{

  // cout << "Came to start bfs with vertex: " << s << " and cluster num: " << _stats->_stat_num_clusters << endl;
  _stats->_stat_num_clusters++;
  assert(!_bfs_visited[s] && "start vertex cannot be visited already");
  // Create a queue for BFS
  list<int> queue;
  int depth = 0;
  int elem_done = 0;

  // Mark the current node as visited and enqueue it
  _bfs_visited[s] = true;
  queue.push_back(s);
  queue.push_back(-1); // marks the end of a level

  // 'i' will be used to get all adjacent
  // vertices of a vertex
  // list<int>::iterator i;
  int i;

  while (!queue.empty())
  {
    // Dequeue a vertex from queue and print it
    s = queue.front();
    if (s == -1)
    {
      queue.pop_front();
      if (queue.size() > 0)
      {
        queue.push_back(-1);
        depth++;
        // cout << "Current depth is: " << depth << " with queue size: " << queue.size() << endl;
        // if(depth>10) exit(0);
      }
      continue;
    }
    assert(slice_id == get_slice_num(s));
    elem_done++;
    // cout << " vert: " << s << " ";
    _vertices_visited++;
    _mapping_bfs[s] = ++_global_ind;
    _mapping_new_to_old[_global_ind] = s;
    queue.pop_front();
    // assert(_stats->_stat_num_clusters<_graph_vertices && "number of clusters should always be less than total vertices");
    // _cluster_elem_done[_stats->_stat_num_clusters]++;

    // Get all adjacent vertices of the dequeued
    // vertex s. If a adjacent has not been visited,
    // then mark it visited and enqueue it
    for (i = _asic->_offset[s]; i < _asic->_offset[s + 1]; ++i)
    {
      if (slice_id != get_slice_num(_asic->_neighbor[i].dst_id))
        continue;
      if (!_bfs_visited[_asic->_neighbor[i].dst_id])
      {
        if (depth < BD)
        {
          // if(elem_done<32) {
          _bfs_visited[_asic->_neighbor[i].dst_id] = true;
          queue.push_back(_asic->_neighbor[i].dst_id);
        }
        else
        {
          BFS(_asic->_neighbor[i].dst_id, slice_id);
        }
      }
    }
  }
  // cout << "Depth of this graph is: " << depth << " with elem done: " << elem_done << endl;
}

// FIXME: check this!
int scratch_controller::get_mem_ctrl_id(int dst_id)
{

  return dst_id % NUM_MC;
  // return dst_id/LOCAL_SCRATCH_SIZE;
  // return (dst_id/num_banks)%(core_cnt);
}

int scratch_controller::get_mc_core_id(int dst_id)
{

  // return _map_grp_to_core[get_grp_id(dst_id)];
  // int rank = _mapping_dfs[dst_id];
  // int per_core_vertex = _graph_vertices/core_cnt;
  // int scr_id = (rank/per_core_vertex);
  // scr_id = scr_id < core_cnt ? scr_id : core_cnt-1;
  // assert(scr_id<core_cnt);
  // return scr_id;

  int mc_id = get_mem_ctrl_id(dst_id);
  int core_id;
  int rand_num = rand() % num_rows;
  switch (mc_id)
  {
  case 0:
    core_id = rand_num;
    break;
  case 1:
    core_id = rand_num * num_rows + num_rows - 1;
    break;
  case 2:
    core_id = rand_num + num_rows * (num_rows - 1);
    break;
  case 3:
    core_id = rand_num * num_rows;
    break;
  default:
    cout << "Wrong mc id";
    exit(0);
    break;
  }
  assert(core_id < core_cnt);
  return core_id;
  // return dst_id/LOCAL_SCRATCH_SIZE;
  // return (dst_id/num_banks)%(core_cnt);
}

// So this splits linear mapping to groups
int scratch_controller::get_grp_id(int dst_id)
{

  int rank = _mapping[dst_id];
#if BFS_MAP == 1
  rank = _mapping_bfs[dst_id];
#endif
  int per_core_vertex = _vertices_visited / core_cnt;
  // int per_core_vertex = _graph_vertices/core_cnt;
#if DFSMAP == 0 && BFS_MAP == 0
  per_core_vertex = _slice_size / core_cnt;
#endif
  int scr_id = (rank / per_core_vertex);
  // scr_id = scr_id < core_cnt ? scr_id : core_cnt-1;
  // TODO: I am not convinced but it solves the problem? because of it, the neighbors were mapped to
  // core 15, then when core 15 went to exeucute that task, it could not be
  // completed somehow?
  scr_id = scr_id < core_cnt ? scr_id : 0;
  assert(scr_id < core_cnt);
  void print_mapping();
  /*if(dst_id>0.75*_graph_vertices) {
    cout << "dst_id: " << dst_id << " and scr_id: " << scr_id << " per core vertex: " << per_core_vertex << " rank: " << rank << " vertices visited: " << _vertices_visited << endl;
    exit(0);
  }*/
  return scr_id;
}

//rotate/flip a quadrant appropriately
void scratch_controller::rot(int n, int *x, int *y, int rx, int ry)
{
  if (ry == 0)
  {
    if (rx == 1)
    {
      *x = n - 1 - *x;
      *y = n - 1 - *y;
    }

    //Swap x and y
    int t = *x;
    *x = *y;
    *y = t;
  }
}

// this works when most connections are between core 1 and core 2
// should i try some greedy algorithm???
int scratch_controller::get_hilbert_core(int cluster_id)
{
  // return cluster_id%core_cnt;
  int n = num_rows;
  int d = cluster_id % core_cnt;
  int x = 0, y = 0;
  // these cluster_id should be in the range 0, slice_size/cluster_size
  int rx, ry, s, t = d;
  for (s = 1; s < n; s *= 2)
  {
    rx = 1 & (t / 2);
    ry = 1 & (t ^ rx);
    rot(s, &x, &y, rx, ry);
    x += s * rx;
    y += s * ry;
    t /= 4;
  }
  // convert x,y to core_id
  int return_core = x * num_rows + y;
  return return_core;
}

int scratch_controller::avg_remote_latency()
{
  int num_hops = 0;
  int num_rem = 0;
  for (int i = 0; i < V; ++i)
  {
    for (int e = _asic->_offset[i]; e < _asic->_offset[i + 1]; ++e)
    {
      int src_core = get_local_scratch_id(_asic->_neighbor[i].src_id);
      int dst_core = get_local_scratch_id(_asic->_neighbor[i].dst_id);
      num_hops += _asic->_network->calc_hops(src_core, dst_core);
      if (src_core != dst_core)
        ++num_rem;
    }
  }
  cout << "Average hops per edge: " << num_hops / (float)E << endl;
  cout << "%age edges that are remote: " << num_rem / (float)E << endl;

  /*int count[core_cnt];
  for(int c=0; c<core_cnt; ++c) count[c]=0;
  for(int i=0; i<V; ++i) {
      count[get_local_scratch_id(i)]++;
  }
  for(int c=0; c<core_cnt; ++c) {
      cout << "At core: " << c << " count: " << count[c] << endl;
  }*/

  return num_hops;
}

// ok, last log(num_banks) bits for finding the local bank_id
int scratch_controller::get_local_scratch_id(int dst_id, bool compute_placement)
{

#if PROFILING == 1
  int orig_core = _map_vertex_to_core[dst_id];
  // return _map_grp_to_core[orig_core];
  return orig_core;
#endif
  // return dst_id/LOCAL_SCRATCH_SIZE;
  // scratch size has to be small
  // int factor=8;
  // return (_mapping_bfs[dst_id]%(core_cnt*process_thr*factor))/(factor*process_thr);
  int mod_core;
#if BLOCKED_DFS == 1
  // 2.48 vs 2.57 with the hilbert curve
  mod_core = (_mapping[dst_id] % (core_cnt * process_thr * SPATIAL_STRIDE_FACTOR)) / (SPATIAL_STRIDE_FACTOR * process_thr);
#if HILBERT_MAP == 0
  return mod_core;
#else
  // if odd set, we should send (core_cnt-mod_core)
  int set_core = (_mapping[dst_id] % (2 * core_cnt * process_thr * SPATIAL_STRIDE_FACTOR)) / (SPATIAL_STRIDE_FACTOR * process_thr);
  if (set_core < core_cnt)
  {
    return get_hilbert_core(mod_core);
  }
  else
  {
    return get_hilbert_core(core_cnt - mod_core);
  }
#endif
  // int cluster_id = (_mapping[dst_id])/(SPATIAL_STRIDE_FACTOR*process_thr);
  // return get_hilbert_core(cluster_id);
#endif
#if RANDOM_SPATIAL == 1
  return _mapped_core[dst_id];
#endif
  mod_core = get_grp_id(dst_id); // (mapping/per_core_vertex -- slice_size/core_cnt)
#if HILBERT_MAP == 0
  if (_asic->_config->_algo == gcn && compute_placement)
  {
    if (_asic->_asic_cores[mod_core]->_fine_grain_throughput == 0 || _asic->_fine_alloc[mod_core] == 0)
    {
      // it should allocate random core among active cores
      // int start_core = dst_id; // rand()%core_cnt;
      for (int x = (mod_core + 1) % core_cnt, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
      {
        bool cond = false;
        if (_asic->_config->_prefer_tq_latency)
        {
          cond = _asic->_fine_alloc[x] == 1;
        }
        else
        {
          cond = _asic->_asic_cores[x]->_fine_grain_throughput > 0;
        }
        if (cond)
        {
          mod_core = x;
          break;
        }
      }
      // return rand()%4;
    }
  }
  return mod_core; // oh this is being selected...
#else
  return get_hilbert_core(mod_core);
#endif
  // #if SGU_SLICING==1
  // #endif
  //   return dst_id%core_cnt; // good for load balance with this new scheme
  // #if PR==1 && WORKING_CACHE==1
  //   return dst_id%core_cnt;
  // #endif

#if GCN == 1
  int per_core = LADIES_SAMPLE / core_cnt;
  mod_core = dst_id / per_core;
  // new tasks should be pushed only to active cores
  if (_asic->_asic_cores[mod_core]->_fine_grain_throughput == 0)
  {
    // it should allocate random core among active cores
    // int start_core = dst_id; // rand()%core_cnt;
    for (int x = mod_core + 1, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
    {
      if (_asic->_asic_cores[x]->_fine_grain_throughput > 0)
      {
        mod_core = x;
        break;
      }
    }
    // return rand()%4;
  }
  return mod_core;

  // return dst_id%core_cnt;
#endif

  // Oh this should be blocked by the current tile
  // int slice_size = _graph_vertices/_slice_count;
  // int tile_num = dst_id/slice_size;
  // return (tile_num+(dst_id%_slice_count));
  int tile_ind = dst_id % _slice_size; // 0 to _slice_size
  int x = core_cnt * tile_ind / _slice_size;
  return x;
#if LADIES_GCN == 1
  int num_vert_per_core = LADIES_SAMPLE / core_cnt;
  int scr_size = num_vert_per_core * FEAT_LEN * 4;
  x = (dst_id * FEAT_LEN * 4 / scr_size); ///core_cnt;
  return x % core_cnt;
#endif
  return get_grp_id(dst_id);
  return _mapping[dst_id] % core_cnt;
#if MAPPING_COL == 1
  return (dst_id) % (core_cnt);
#else
  return get_grp_id(dst_id);
  // return rand()%core_cnt;
  // return _mapped_core[dst_id];
  // factor=32;
  // return _map_grp_to_core[get_grp_id(dst_id)];
  // return _mapping[dst_id]%core_cnt;
#if BFS_MAP == 1
  return (_mapping_bfs[dst_id] % (core_cnt * process_thr * factor)) / (factor * process_thr);
#else
  return (_mapping[dst_id] % (core_cnt * process_thr * 8)) / (8 * process_thr);
#endif
  return (_mapping[dst_id] % (core_cnt * process_thr * 2)) / (num_banks);
  return (_mapping[dst_id] % (core_cnt * num_banks)) / num_banks;
  // return (_mapping[dst_id]%(core_cnt*process_thr))/process_thr;
  // return (_mapping[dst_id]%(core_cnt*process_thr*4))/(4*process_thr);
  // return rand()%core_cnt;

  // return _mapped_core[dst_id];
  int rank = _mapping[dst_id];
  // return ((dst_id*64)/LOCAL_SCRATCH_SIZE)%core_cnt;
  // return ((dst_id)/(core_cnt*num_banks))%core_cnt;
  // I want to extract starting 6 bits
  // return ((dst_id)/(core_cnt*num_banks))%core_cnt;
  int per_core_vertex = _asic->_graph_vertices / core_cnt;
  int scr_id = (rank / per_core_vertex);
  scr_id = scr_id < core_cnt ? scr_id : core_cnt - 1;
  assert(scr_id < core_cnt);
  // cout << "scr_id: " << scr_id << endl;
  return scr_id;
  // return (dst_id/64)%core_cnt;  -- not sure why this more load imbalance..
#endif
  // return (dst_id/num_banks)%(core_cnt);
}

int scratch_controller::get_global_bank_id(int dst_id)
{
  int banks_per_core = num_banks / core_cnt; // 256/1 = 256
  int bank_ind = get_local_bank_id(dst_id);  // use_knh_hash(dst_id); // this is out of 16
  int bank_id_global = get_local_scratch_id(dst_id) * banks_per_core + bank_ind;
  if (_asic->_config->_prac == 0)
  { // Why do?
    // int vert_per_core = V/16;
    bank_id_global = dst_id % num_banks; // (dst_id/(V/16))*(num_banks/16) + (bank_ind); // it is concentrated on a single core? need bdfs?
    if (dst_id < 256)
    { // should be different for each source access
      bank_id_global = (bank_id_global + (rand() % core_cnt) * 16) % num_banks;
    }
    // bank_id_global = dst_id%num_banks;
  }
  return bank_id_global;
}

int scratch_controller::get_local_bank_id(int dst_id)
{
  // int local_scratch_addr = dst_id%LOCAL_SCRATCH_SIZE;
  // int bank_id = local_scratch_addr/num_banks;
  // return bank_id;
  // This is linear mapping (we could also extract the start bits here)
  // int d= _mapping[dst_id]/num_banks;
  // return d%num_banks;
  // return rand()%num_banks;
  // int x = _mapping[dst_id]/8;
  // return x%num_banks;
  if (_asic->_config->_working_cache)
  {
    // same bank for dst_id to dst_id + line_size/message_size
    int x = dst_id >> 4;    // assuming message size of 4 bytes
    return use_knh_hash(x); // dst_id%num_banks;
  }
#if GRAPHPULSE == 1
  return dst_id % num_banks;
#else
  return use_knh_hash(dst_id); // rand()%num_banks;
#endif
  // return dst_id%num_banks;
  return _mapping[dst_id] % num_banks;
  // return dst_id%num_banks;
}

void scratch_controller::perform_spatial_part()
{

  // spatial partitioning defined only for non-sliced execution
  if (_asic->_config->_exec_model == async_slicing || _asic->_config->_exec_model == sync_slicing || _asic->_config->_exec_model == blocked_async)
  {
    for (int i = 0; i < _asic->_slice_count; ++i)
    {
      switch (_asic->_config->_spatial_part)
      {
      case bdfs:
        perform_bdfs_load_balancing(i);
        break;
      case linear:
        perform_linear_load_balancing(i);
        break;
      case modulo:
        perform_modulo_load_balancing(i);
        break;
      case random_spatial:
        for (int v = i * _slice_size; v < (i + 1) * _slice_size; ++v)
        {
          _asic->_mapped_core[v] = rand() % core_cnt;
        }
        break;
      default:
        perform_load_balancing2(i);
        break;
      }
    }
  }

  if (_asic->_config->_spatial_part == bdfs)
  {
    optimize_load();
  }

  // model->power_law_part();
  // no need with sens_aware_part
  map_grp_to_core();
  if (_asic->_config->_exec_model != async)
  {
    locality_test(0);
  }
}

void scratch_controller::read_mapping()
{
  // read mapping file
  // string str(csr_file);
  FILE *mapping_file = fopen("road_mapping.txt", "r");
  char linetoread[5000];
  int ind = -1;
  while (fgets(linetoread, 5000, mapping_file) != NULL)
  {
    std::string raw(linetoread);
    std::istringstream iss(raw.c_str());
    iss >> _mapping[++ind];
  }
  fclose(mapping_file);
}

void scratch_controller::generate_metis_format()
{
  // char s[] = csr_file "_in";
  // ofstream metis_input(s);
  // ofstream metis_input("usa_road");
  // ofstream metis_input("twitter");
  // ofstream metis_input("indochina");
  // ofstream metis_input("wiki");
  // ofstream metis_input("west_road");
  ofstream metis_input("full_usa_road");
  int e = E; // all are directed, it is an odd number
  // half edges should be mentioned and double should be listed...or they are
  // listed in that way? (odd number of edges is not allowed) -- for a double
  // listed graph -- not sure what was the deal with livejounral..
  if (e % 2 == 0)
    e = E / 2; //very aggressively assuming undirected graph

  // cout << "Started writing metis format file: " << s << endl;
  if (metis_input.is_open())
  {
    metis_input << _asic->_graph_vertices << " " << e << endl; // undirected?
    std::string s = "";
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      for (int j = _asic->_offset[i]; j < _asic->_offset[i + 1]; ++j)
      {
        s += " " + std::to_string(_asic->_neighbor[j].dst_id + 1);
        // metis_input << " " << (_neighbor[j].dst_id+1);
        --e;
      }
      s += "\n";
      // metis_input << s;
      if (i % 10000 == 0)
      {
        metis_input << s;
        s = "";
        cout << "10 thousand vertices done with edges " << e << "\n";
      }
      // metis_input << endl;
    }
    metis_input << s;
  }
  metis_input.close();
  if (e != 0)
    cout << "Extra edges than marked: " << e << endl;
  exit(0);
  // call metis function
  // system("./metis-5.1.0/build/Linux-x86_64/programs/gpmetis s" a);
}

// move a vertex b/w partitions to balance out this or 10% space reserved? (or we could reserve it from the double buffer of other slices)
void scratch_controller::read_mongoose_slicing()
{
  FILE *graph_file = fopen("output", "r");

  cout << "Started reading mongoose file\n";
  char linetoread[5000];
  for (int i = 0; i < _asic->_slice_count; ++i)
    _asic->_slice_size_exact[i] = -1;
  while (fgets(linetoread, 5000, graph_file) != NULL)
  {
    std::string raw(linetoread);
    std::istringstream iss(raw.c_str());
    int a, b;
    iss >> a >> b;
    // _stats->_slice_vids[b].push_back(a);
    _slice_vids[b][++_asic->_slice_size_exact[b]] = a; // .push_back(a);
    _slice_num[a] = b;
  }
  fclose(graph_file);
  cout << "Done reading mongoose file\n";
}

void scratch_controller::read_metis_slicing()
{
  // FILE* graph_file = fopen("datasets/metis_inputs/flickr_in.part.4", "r");
  // FILE* graph_file = fopen("datasets/metis_inputs/lj_csr_in.part.4", "r");
  // FILE* graph_file = fopen("datasets/metis_inputs/orkut.part.4", "r");
  // FILE* graph_file = fopen("datasets/metis_inputs/usa_road.part.4", "r");

  if (metis_file == "")
  {
    cout << "NO METIS PARTITIONING DONE YET!\n";
    return;
  }

  for (int i = 0; i < _asic->_metis_slice_count; ++i)
    _asic->_slice_size_exact[i] = -1;
  int a = -1;
  int b;
#if RANDOM == 1
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    ++a;
    b = rand() % SLICE_COUNT;
    _slice_vids[b][++_slice_size_exact[b]] = a; // .push_back(a);
    _stats->_slice_num[a] = b;
  }
  return;
#endif

  string str(metis_file);
  FILE *graph_file = fopen(str.c_str(), "r");

  cout << "Started reading metis file\n";
  char linetoread[5000];
  while (fgets(linetoread, 5000, graph_file) != NULL)
  {
    std::string raw(linetoread);
    std::istringstream iss(raw.c_str());
    ++a;
    iss >> b;
    // _stats->_slice_vids[b].push_back(a);
    _slice_vids[b][++_asic->_slice_size_exact[b]] = a; // .push_back(a);
    _slice_num[a] = b;
    // cout << "vertex " << a << " slice count: " << b << endl;
  }
  fclose(graph_file);
  cout << "Done reading metis file\n";
}

void scratch_controller::get_sync_boundary_nodes()
{

  for (int i = 0; i < _asic->_graph_vertices; ++i)
    _num_slices_it_is_copy_vertex[i] = 0;

  cout << "Came in to get slice boundary nodes\n";
  // read from mongoose to get slice number..
  int boundary_nodes[_asic->_slice_count];
  for (int i = 0; i < core_cnt; ++i)
    _asic->_task_ctrl->_present_in_local_queue[i].reset();
  assert(_asic->_slice_count <= core_cnt);

  for (int j = 0; j < _asic->_slice_count; ++j)
  {
    boundary_nodes[j] = 0;
  }
  cout << "Allocation done\n";

  for (int v = 0; v < _asic->_graph_vertices; ++v)
  {
    int src_slice = get_slice_num(v);
    for (int e = _asic->_offset[v]; e < _asic->_offset[v + 1]; ++e)
    {
      int dst_slice = get_slice_num(_asic->_neighbor[e].dst_id);
      // FIXME: why is slice number same?
      /*if(src_slice!=dst_slice) {
        cout << "src slice: " << src_slice << " dst slice: " << dst_slice << " src vid: " << v << " dst: " << _asic->_neighbor[e].dst_id << endl;
      }*/
      if (dst_slice != src_slice && _asic->_task_ctrl->_present_in_local_queue[dst_slice].test(v) == 0)
      {
        boundary_nodes[dst_slice]++; // for a slice, remote source node should be covered only once
        _num_slices_it_is_copy_vertex[v]++;
        _asic->_task_ctrl->_present_in_local_queue[dst_slice].set(v);
      }
    }
  }

  cout << "Calculated boundary nodes\n";

  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    cout << "Percentage boundary nodes for slice i: " << i << " is: " << boundary_nodes[i] / (float)_slice_size << endl;
  }
  // exit(0);

  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    _asic->_task_ctrl->_present_in_local_queue[i].reset();
  }

  // exit(0);
}

void scratch_controller::get_linear_sgu_boundary_vertices()
{
#if SGU_SLICING == 0 && HYBRID == 0
  return;
#endif
  cout << "Came in to get slice boundary nodes\n";
  // read from mongoose to get slice number..
  int *boundary_nodes[_asic->_slice_count];

  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    boundary_nodes[i] = (int *)malloc(_asic->_slice_count * sizeof(int));
    for (int j = 0; j < _asic->_slice_count; ++j)
    {
      boundary_nodes[i][j] = 0;
    }
  }
  cout << "Allocation done\n";

  // get_slice_num => old vid -> slice
  // slice_ids => old vid -> new one
  for (int i = 0; i < _asic->_slice_count; ++i)
  { // this should be in order of dfs
    _node_done.reset();

    int v = i * _slice_size;
    for (int a = 0; a < _slice_size; ++a)
    { // for all vertices in this slice
      v += 1;
      for (int e = _asic->_offset[v]; e < _asic->_offset[v + 1]; ++e)
      { // for all edges in this slice
        int dst = _asic->_neighbor[e].dst_id;
        // cout << "new system, v: " << v << " dst: " << dst << endl;
        int dst_slice = get_slice_num(dst);
        if (!_node_done.test(dst) && i != dst_slice)
        {
          boundary_nodes[i][dst_slice]++;
          _node_done.set(dst);
        }
      }
    }
  }

  cout << "Calculated boundary nodes\n";

  int sum = 0;
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    sum = 0;
    for (int j = 0; j < _asic->_slice_count; ++j)
    {
      sum += boundary_nodes[i][j];
    }
    _num_boundary_nodes_per_slice[i] = sum;
    cout << "Percentage boundary nodes for slice i: " << i << " is: " << sum / (float)_slice_size << endl;
  }

  cout << "Calculated boundary nodes\n";
}

// Let's just do Mongoose (only thing is how to make multiple partitions)
void scratch_controller::get_slice_boundary_nodes()
{

  if (metis_file == "")
  {
    cout << "NO METIS PARTITIONING DONE YET!\n";
    // TODO: still calculate the number of copy vertices
    get_linear_sgu_boundary_vertices();
    return;
  }

  cout << "Came in to get slice boundary nodes\n";
  // read from mongoose to get slice number..
  int *boundary_nodes[_asic->_slice_count];

  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    boundary_nodes[i] = (int *)malloc(_asic->_slice_count * sizeof(int));
    for (int j = 0; j < _asic->_slice_count; ++j)
    {
      boundary_nodes[i][j] = 0;
    }
  }

  int in_degree_category[4];
  // int total_out_degree[_slice_count];
  // total_out_degree[j]=0;
  for (int i = 0; i < 4; ++i)
  {
    in_degree_category[i] = 0;
  }

  cout << "Allocation done\n";

  // in-degree of copy nodes from this slice only...

  // get_slice_num => old vid -> slice
  // slice_ids => old vid -> new one

  // this thing consuming too much time
  for (int i = 0; i < _asic->_slice_count; ++i)
  { // this should be in order of dfs
    cout << "Starting with slice: " << i << " and size: " << _asic->_slice_size_exact[i] << endl;
    _node_done.reset();

    for (int a = 0; a < _asic->_slice_size_exact[i]; ++a)
    {
      int v = _slice_vids[i][a];
      for (int e = _asic->_offset[v]; e < _asic->_offset[v + 1]; ++e)
      {
        int dst = _asic->_neighbor[e].dst_id;
        // cout << "new system, v: " << v << " dst: " << dst << endl;
        int dst_slice = _slice_num[dst]; // get_slice_num(dst);
        // TODO: if this node is not done (that rate will also be interesting)
        if (i != dst_slice && !_node_done.test(dst))
        {
          // copy vertex = v (start and end offset should be different)
          boundary_nodes[i][dst_slice]++;
          _node_done.set(dst);
        }
      }
      // if(a%10000==0)
      // cout << " done for local slice id: " << a << " size: " << _slice_size[i] << endl;
    }
  }

  cout << "Calculated boundary nodes\n";

  /*for(int j=0; j<_slice_count; ++j) { 
    cout << "AVERAGE OUT DEGREE OF SLICE: " << j << " is: " << total_out_degree[j] << endl;
    for(int i=0; i<4; ++i) { 
      cout << "slice: " << j << " category: " << i << " number of nodes: " << in_degree_category[j][i] << endl; 
    }
  }*/

  int sum = 0;
  cout << "Printing boundary nodes";
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    sum = 0;
    for (int j = 0; j < _asic->_slice_count; ++j)
    {
      cout << "i: " << i << " j: " << j << " boundary nodes: " << boundary_nodes[i][j] << endl;
      sum += boundary_nodes[i][j];
    }
    _num_boundary_nodes_per_slice[i] = sum;
    // cout << "Total boundary nodes for i: " << i << " is: " << sum << " with slice size: " << slice_size << endl;
    cout << "Percentage boundary nodes for slice i: " << i << " is: " << sum / (float)_slice_size << endl;
    // cout << "Total boundary nodes for i: " << i << " is: " << sum/(float)_graph_vertices << endl;
  }
  cout << "Slicing overhead in GCN is: " << (GCN_LAYERS * FEAT_LEN * message_size * sum) << endl;

  /*
 for(int k=0; k<_slice_count; ++k) { 
      cout << "AVERAGE OUT DEGREE OF SLICE: " << k << " is: " << total_out_degree[k]/(float)_num_boundary_nodes_per_slice[k] << endl;
      cout << "non dangling: " << k << " is: " << count_non_dangling[k]/(float)_num_boundary_nodes_per_slice[k] << endl;
    }*/
  // exit(0);
  // TODO: rename here... (sorting all small edge list to produce CSR format: sorting ensures potential coalesces in accessing destination vertex but that does not help me much because I still send scalar accesses using decomposable network)
  // int mapping_old_to_new[_graph_vertices];
  int c = -1;
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    // for(int a=0; a<_stats->_slice_vids[i].size(); ++a) {
    for (int a = 0; a <= _asic->_slice_size_exact[i]; ++a)
    {
      _mapping_old_to_new[_slice_vids[i][a]] = ++c;
      // cout << "old: " << _stats->_slice_vids[i][a] << " new: " << c << endl;
    }
  }

  // boundary nodes should store the absolute position
  sum = 0;
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    for (int d = 0; d < _asic->_slice_count; ++d)
    {
      if (i == d)
        continue;
      boundary_nodes[i][d] = sum;
      sum += boundary_nodes[i][d];
    }
  }

  /*int counter[_slice_count][_slice_count];
  for(int i=0; i<_slice_count; ++i) {
    for(int j=0; j<_slice_count; ++j) {
      counter[i][j]=boundary_nodes[i][d];
    }
  }*/

  // To achieve higher spatial locality
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    _node_done.reset();
    for (int a = 0; a < _asic->_slice_size_exact[i]; ++a)
    {
      int v = _slice_vids[i][a];
      for (int e = _asic->_offset[v]; e < _asic->_offset[v + 1]; ++e)
      {
        int dst = _asic->_neighbor[e].dst_id;
        int dst_slice = _slice_num[dst]; // get_slice_num(dst);
        if (!_node_done.test(dst) && i != dst_slice)
        {
          // _old_vertex_to_copy_location[i].emplace(_stats->_mapping_old_to_new[dst], boundary_nodes[i][dst_slice]);
          _old_vertex_to_copy_location[i].emplace(_mapping_old_to_new[dst], ++boundary_nodes[i][dst_slice]);
          _node_done.set(dst);
        }
      }
    }
  }

  // FIXME: Oh this doesn't match because source is different now...(need to do
  // something)
#if GCN == 0 && CF == 0
  for (int i = 0; i < _asic->_graph_vertices; ++i)
    _asic->_vertex_data[i] = _asic->_correct_vertex_data[i];
#endif
  int cur_e = -1;
  int cur_v = 0;
  for (int i = 0; i < _asic->_slice_count; ++i)
  {
    for (int a = 0; a <= _asic->_slice_size_exact[i]; ++a)
    {
      // for(int a=0; a<_stats->_slice_vids[i].size(); ++a) {
      int old_v = _slice_vids[i][a];
#if GCN == 0 && CF == 0
      _asic->_correct_vertex_data[cur_v] = _asic->_vertex_data[old_v];
#endif
      // cout << "old+v: " << old_v << " start: " << _offset[old_v] << " end: " << _offset[old_v+1] << endl;
      for (int e = _asic->_offset[old_v]; e < _asic->_offset[old_v + 1]; ++e)
      {
        // Now fill neighbor and vertex arrays according to its propery
        // cout << "old: " << _neighbor[e].dst_id << " new: " << _stats->_mapping_old_to_new[_neighbor[e].dst_id] << endl;
        _stats->_stat_extra_finished_edges[++cur_e] = _mapping_old_to_new[_asic->_neighbor[e].dst_id];
        _temp_edge_weights[cur_e] = _asic->_neighbor[e].wgt;
      }
      _num_spec[++cur_v] = cur_e + 1;
    }
  }
  while (cur_v < _asic->_graph_vertices)
    _num_spec[++cur_v] = cur_e + 1;

  cout << "Done storing temporary graph\n";

  for (int i = 1; i <= _asic->_graph_vertices; ++i)
  {
    _asic->_offset[i] = _num_spec[i];
#if GCN == 0 && CF == 0
    _asic->_vertex_data[i - 1] = _asic->_scratch[i - 1];
#endif
    assert(_asic->_offset[i] >= _asic->_offset[i - 1] && "do not follow the basic condition for offset");
    ;
    // cout << "offset at i: " << _offset[i] << endl;
  }

  cur_e = -1;
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    for (int j = _asic->_offset[i]; j < _asic->_offset[i + 1]; ++j)
    {
      _asic->_neighbor[++cur_e].src_id = i;
      _asic->_neighbor[cur_e].dst_id = _stats->_stat_extra_finished_edges[cur_e];
      _asic->_neighbor[cur_e].wgt = _temp_edge_weights[cur_e];
      // _asic->_neighbor[cur_e].done = false;
      assert(j == cur_e && "traversing edges in sequence");
      // cout << "cur e: " << cur_e << " edge traversed: " << _stats->_stat_extra_finished_edges[cur_e] << "dst at i: " << _neighbor[cur_e].dst_id << endl;
      // _stats->_stat_extra_finished_edges[cur_e] = 0;
    }
  }

  cout << "Done modifying the original graph data-structure\n";

#if BDFS == 0
  free(_temp_edge_weights);
  free(_stats->_stat_extra_finished_edges);
#endif

  // check the boundary vertices here...
  int count = 0;
  for (int e = 0; e < _asic->_graph_edges; ++e)
  {
    int src_slice = get_slice_num(_asic->_neighbor[e].src_id);
    int dst_slice = get_slice_num(_asic->_neighbor[e].dst_id);
    if (src_slice != dst_slice)
    {
      ++count;
    }
  }
  cout << "Resulting boundary edges: " << count / (float)E << endl;

  for (int i = 0; i < _asic->_slice_count; ++i)
    free(_slice_vids[i]);
  _asic->_src_vid = _mapping_old_to_new[_asic->_src_vid];
  _asic->init_vertex_data();
  _is_visited.reset();
  _is_visited.set(_asic->_src_vid);
  // DEBUG: see edge weights for src_vid
  /*for(int i=_offset[_src_vid]; i<_offset[_src_vid+1]; ++i) {
    cout << "wgt at i: " << i << " is: " << _neighbor[i].wgt << endl;
  }*/
  /*for(int old=0; old<_graph_vertices; ++old) {
    int v = _stats->_mapping_old_to_new[old]; //ensure offset is correct
    for(int i=_offset[v]; i<_offset[v+1]; ++i) {
      // FIXME: there are some weird weights at the end for 0,1,2...probably
      // for halos which are causing the error
      // cout << "src: " << old << " wgt at i: " << i << " is: " << _neighbor[i].wgt << endl;
      cout << old << " " << _neighbor[i].dst_id << " " << _neighbor[i].wgt << endl;
    }
  }*/
  free(_mapping_old_to_new);
#if BDFS == 0
  free(_mapping_new_to_old);
#endif
  // exit(0);

  /*for(int i=0; i<E; ++i) {
    _neighbor[i].dst_id = _edge_traversed[i];
    cout << "dst at i: " << _neighbor[i].dst_id << endl;
  }*/
  // exit(0);
  // uncomment when want to debug for correctness using comparison to the
  // output of perform_disjkstra();
#if PR == 0 && ABFS == 1
  int edges_traversed = 0;

  // timestamp, vertex
  priority_queue<iPair, vector<iPair>, greater<iPair>> pq;
  pq.push(make_pair(0, _asic->_src_vid));

  while (!pq.empty())
  {
    int src_id = pq.top().second;
    pq.pop();
    _is_visited.set(src_id);
    // cout << "COMMIT NEW VERTEX: " << src_id << endl;
    // because no is_visited condition
    for (int i = _asic->_offset[src_id]; i < _asic->_offset[src_id + 1]; ++i)
    {
      int dst_id = _asic->_neighbor[i].dst_id;
      if (_is_visited[dst_id])
        continue;
      int weight = _asic->_neighbor[i].wgt;
      edges_traversed++;

      // cout << "dst_id: " << dst_id << " weight: " << weight << endl;

      // process+relax step (algo specific)
      int temp_dist = _asic->_scratch[src_id] + weight;
      if (_asic->_scratch[dst_id] > temp_dist)
      {
        _asic->_scratch[dst_id] = temp_dist;
        pq.push(make_pair(_asic->_scratch[dst_id], dst_id));
      }
    }
    // if(_is_visited[src_id]) continue;
  }
  cout << "Edges traversed during dijkstra: " << edges_traversed << endl;
  cout << "Percentage edges traversed during dijkstra: " << edges_traversed / (double)E << endl;
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    _asic->_correct_vertex_data[i] = _asic->_scratch[i];
  }

  // exit(0);
  // TODO: prepare for simulation
  for (int i = 0; i < _asic->_graph_vertices; ++i)
    _asic->_scratch[i] = MAX_TIMESTAMP - 1;
  _asic->_scratch[_asic->_src_vid] = 0;
#endif
}

int scratch_controller::use_knh_hash(int i)
{
  int bank = 0;
  int bank_i = 0;
  int CACHE_BANKS = 16;
  // int CACHE_BANKS = 64;
  // KNH hash table covers 2, 4, 8, 16, 32, 64 banks
  assert((CACHE_BANKS >= 2) && (CACHE_BANKS <= 64) && ((CACHE_BANKS & (CACHE_BANKS - 1)) == 0));
  int H[6][22] = {{6, 10, 12, 14, 16, 17, 18, 20, 22, 24, 25, 26, 27, 28, 30, 32, 33, 35, 36, 38, 39, 40},
                  {7, 11, 13, 15, 17, 19, 20, 21, 22, 23, 24, 26, 28, 29, 31, 33, 34, 35, 37, 38, 41, -1},
                  {8, 12, 13, 16, 19, 22, 23, 26, 27, 30, 31, 34, 35, 36, 37, 38, 39, 42, 44, 45, -1, -1},
                  {9, 12, 16, 17, 19, 21, 22, 23, 25, 26, 27, 29, 31, 32, 36, 37, 38, 43, 44, -1, -1, -1},
                  {10, 11, 13, 16, 17, 18, 19, 20, 21, 22, 27, 28, 30, 31, 33, 34, 39, 40, 42, -1, -1, -1},
                  {11, 17, 19, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 39, 40, 41, 42, 43, -1, -1, -1, -1}};
  int cnt[6] = {22, 21, 20, 19, 19, 18};

  for (int ii = 0; ii < log2(CACHE_BANKS); ii++)
  {
    for (int jj = 0; jj < cnt[ii]; jj++)
    {
      bank_i ^= ((i) >> H[ii][jj]) & 1;
    }
    bank += (UINT64)(bank_i * pow(2, ii));
    bank_i = 0;
  }

  return bank;
}

bool asic::is_leaf_node(int vid)
{
  return (_scratch[vid] == (kdtree_depth));
  int degree = _offset[vid + 1] - _offset[vid];
  return degree == 0; // for tree...
  // cout << "Depth of the current node: " << _scratch[vid] << endl;
}

void scratch_controller::fix_in_degree()
{
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    _in_degree[i] = 0;
  }
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    for (int j = _asic->_offset[i]; j < _asic->_offset[i + 1]; ++j)
    {
      _in_degree[_asic->_neighbor[j].dst_id]++;
    }
  }
  /*
  int max_in_degree=0;
  for(int i=0; i<_asic->_graph_vertices; ++i) {
    if(_in_degree[i] > max_in_degree) {
      max_in_degree = _in_degree[i];
    }
  }
  cout << "Max in degree: " << max_in_degree << endl;
  */

  int in_hist[5];
  int out_hist[5];
  for (int i = 0; i < 5; ++i)
  {
    in_hist[i] = 0;
    out_hist[i] = 0;
  }
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    if (_in_degree[i] != 0)
    {
      ++_asic->_non_dangling_graph_vertices;
    }
    else
    {
      _asic->_dangling_queue.push_back(i);
    }
    // cout << "vid: " << i << " in-degree: " << _in_degree[i] << endl;
    if (_in_degree[i] < 5)
    {
      ++in_hist[0];
    }
    else if (_in_degree[i] < 10)
    {
      ++in_hist[1];
    }
    else if (_in_degree[i] < 50)
    {
      ++in_hist[2];
    }
    else if (_in_degree[i] < 100)
    {
      ++in_hist[3];
    }
    else if (_in_degree[i] > 100)
    {
      ++in_hist[4];
    }
    int out_degree = _asic->_offset[i + 1] - _asic->_offset[i];
    if (out_degree < 5)
    {
      ++out_hist[0];
    }
    else if (out_degree < 10)
    {
      ++out_hist[1];
    }
    else if (out_degree < 50)
    {
      ++out_hist[2];
    }
    else if (out_degree < 100)
    {
      ++out_hist[3];
    }
    else if (out_degree > 100)
    {
      ++out_hist[4];
    }
    /*if(_in_degree[i]>50) {
      cout << "vid: " << i << " in-degree: " << _in_degree[i] << endl;
    }*/
    // cout << "vid: " << i << " out-degree: " << out_degree << endl;
    /*if(out_degree>100) {
      cout << "vid: " << i << " out-degree: " << out_degree << endl;
    }*/
  }
  for (int i = 0; i < 5; ++i)
  {
    cout << "in-degree hist at i: " << i << " is: " << in_hist[i] << endl;
    cout << "out-degree hist at i: " << i << " is: " << out_hist[i] << endl;
  }
  // exit(0);
}

void scratch_controller::push_dangling_vertices(bool second_buffer)
{
  for (int i = 0; i < _asic->_dangling_queue.size(); ++i)
  {
    int vid = _asic->_dangling_queue[i];
    task_entry cur_task(vid, _asic->_offset[vid]);
    cur_task.second_buffer = second_buffer;
    _asic->_task_ctrl->insert_local_task(0, 0, get_local_scratch_id(vid), 0, cur_task);
  }
}

// In async, actually it is similar as one of them is off-chip always
// It might be useful to consider both ways and compare them?
void scratch_controller::graphicionado_slice_fill()
{
  bitset<SLICE_COUNT> done;
  int tot_extra_traffic = 0;
  int worst_case_extra_traffic = 0;
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    int src_slice = get_slice_num(i);
    vector<int> bslice;
    done.reset();
    for (int e = _asic->_offset[i]; e < _asic->_offset[i + 1]; ++e)
    {
      int dest_slice = get_slice_num(_asic->_neighbor[e].dst_id);
      if (src_slice != dest_slice)
        ++worst_case_extra_traffic;
      if (src_slice != dest_slice && !done.test(dest_slice))
      {
        bslice.push_back(dest_slice);
        done.set(dest_slice);
        ++tot_extra_traffic;
      }
    }
    if (bslice.size() > 0)
    {
      _map_boundary_to_slice.push_back(make_pair(i, bslice));
    }
  }
  cout << "Count of mapped vertices: " << _map_boundary_to_slice.size() << endl;
  cout << "Total extra memory accesses: " << tot_extra_traffic << endl;
  cout << "Worst case extra memory accesses: " << worst_case_extra_traffic << endl;
  sgu_slice_fill();
}

// number of unique vertices that have incoming edges from different sources (add all those sources)
void scratch_controller::sgu_slice_fill()
{
  vector<int> x;
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    _map_boundary_to_slice.push_back(make_pair(i, x));
  }
  int tot_extra_traffic = 0;
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    int src_slice = get_slice_num(i);
    for (int e = _asic->_offset[i]; e < _asic->_offset[i + 1]; ++e)
    {
      int dest_slice = get_slice_num(_asic->_neighbor[e].dst_id);
      if (src_slice != dest_slice && !_done[_asic->_neighbor[e].dst_id].test(src_slice))
      {
        _map_boundary_to_slice[_asic->_neighbor[e].dst_id].second.push_back(src_slice);
        _done[_asic->_neighbor[e].dst_id].set(src_slice);
      }
    }
  }
  for (int i = 0; i < _asic->_graph_vertices; ++i)
  {
    tot_extra_traffic += _map_boundary_to_slice[i].second.size();
  }
  cout << "Count of mapped vertices: " << tot_extra_traffic << endl;
  exit(0);
}
