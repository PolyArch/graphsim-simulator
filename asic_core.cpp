#include "asic_core.hh"

asic_core::asic_core()
{
  for (int i = 0; i < LANE_WIDTH; ++i)
  {
    _in_order_flag[i] = true;
    _waiting_count[i] = 0;
  }
  for (int t = 0; t < NUM_TQ_PER_CORE; ++t)
  {
    _pending_task[t].vid = -1;
  }
}

// The buffer from memory is a vector port; others are dfg port -- can be of variable length
bool asic_core::can_push_in_prefetch(int lane_id)
{
  return (_pref_lsq[lane_id].size() < _agg_fifo_length);
}

bool asic_core::can_push_in_gcn_bank()
{
  return ((_hit_gcn_updates.size() < _agg_fifo_length) && (_miss_gcn_updates.size() < _agg_fifo_length));
  // return ((_hit_gcn_updates.size() < 0.5*FIFO_DEP_LEN) && (_miss_gcn_updates.size() < 1.5*FIFO_DEP_LEN));
}

bool asic_core::can_push_in_process(int lane_id)
{
  return (_prefetch_process[lane_id].size() < _agg_fifo_length);
}

bool asic_core::can_push_in_reduce(int lane_id)
{
  return (_process_reduce[lane_id].size() < _agg_fifo_length);
}

bool asic_core::can_push_to_cgra_event_queue()
{
  bool flag = true;
  int num_dfg = _fine_grain_throughput / _asic->_config->_process_thr;
  for (int i = 0; i < num_dfg; ++i)
  {
    if (_cgra_event_queue[i].size() > _agg_fifo_length)
    {
      flag = false;
    }
  }
  return flag; // (_cgra_event_queue.size() < FIFO_DEP_LEN);
}

// @vidushi: triangle counting: func(edge list of cur_tuple.dst_id and edge list of cur_tuple.src_id)
void asic_core::insert_vector_task(pref_tuple cur_tuple, bool spawn, int dfg_id)
{
  if (_asic->_config->_tc && MAX_DFGS > 1)
  {
    assert(dfg_id > -1 && "we want to implement parallel dfgs");
    _last_dfg_id = dfg_id;
  }
  // total finished edges should have been more right??
  // cout << "Pushing a vector task for vid: " << cur_tuple.edge.dst_id << " at core: " << _core_id << " cycle: " << _asic->_cur_cycle << endl;
  DTYPE prio = 0;
  // int ready_cycle = _cur_free_cycle[_last_dfg_id] + DFG_LENGTH + bus_width/VEC_LEN; // cur_tuple.update_width/VEC_LEN;
  int throughput = _asic->get_update_operation_throughput(cur_tuple.src_id, cur_tuple.edge.dst_id);
  int latency = _asic->get_update_operation_latency();
  int ready_cycle = _cur_free_cycle[_last_dfg_id] + latency + throughput;
  int dst_id = cur_tuple.edge.dst_id;
  if (_asic->_config->_pull_mode)
  {
    dst_id = cur_tuple.src_id;
  }
  gcn_update new_up(dst_id, spawn, cur_tuple, ready_cycle, prio);
  new_up.second_buffer = cur_tuple.second_buffer;
  if (_asic->_config->_algo == gcn)
  {
    assert(spawn == false && new_up.spawn == false);
  }
  if (_asic->_config->_tc && MAX_DFGS > 1)
  {
    new_up.thr = throughput;
    _asic->_gcn_updates[dfg_id] += new_up.thr;
  }
  _cgra_event_queue[_last_dfg_id].push_back(new_up);
  _cur_free_cycle[_last_dfg_id] = ready_cycle - latency; // pipelined issue
  // cout << "Pushing a new event for gcn updates with dst_id: " << dst_id << " throughput " << throughput << " at core: " << _core_id << " cycle: " << _asic->_cur_cycle << " ready cycle: " << ready_cycle << " new free cycle: " << _cur_free_cycle << " and dfg id: " << _last_dfg_id << endl;
  _asic->_stats->_stat_pushed_to_ps++;
  if (!(_asic->_config->_tc && MAX_DFGS > 1))
  {
    _last_dfg_id = (_last_dfg_id + 1) % (_fine_grain_throughput / _asic->_config->_process_thr);
  }
}

// TODO: add count for how many elements are added. (and check that only)
void asic_core::push_gcn_cache_miss(pref_tuple cur_tuple)
{
  if (_asic->_config->_update_coalesce)
  {
    if (_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id] == 0)
    {
      _miss_gcn_updates.push_back(cur_tuple);
    }
    else
    { // here it should do compute-coalescing (push into cgra_event_queue)
      insert_vector_task(cur_tuple, false);
      ++_asic->_mem_ctrl->_l2accesses;
      ++_asic->_mem_ctrl->_l2hits;
      return;
    }
    ++_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id];
  }
  else
  {
    _miss_gcn_updates.push_back(cur_tuple);
  }
}

void asic_core::insert_prefetch_process(int lane_id, int priority, pref_tuple cur_tuple)
{
  if (_asic->_config->_algo != gcn)
    priority = 0;

  // if(_asic->_cur_cycle<210) {
  //   cout << "In cycle: " << _asic->_cur_cycle << " core: " << _core_id << " pushed update from src_id: " << cur_tuple.src_id << " and dst: " << cur_tuple.edge.dst_id << endl;
  // }
  // assert(cur_tuple.src_id>=0 && cur_tuple.src_id<V);
  auto it = _prefetch_process[lane_id].find(priority);
  if (it == _prefetch_process[lane_id].end())
  {
    list<pref_tuple> x;
    x.push_back(cur_tuple);
    _prefetch_process[lane_id].insert(make_pair(priority, x));
  }
  else
  {
    it->second.push_back(cur_tuple);
  }
}

void asic_core::pop_from_prefetch_process()
{
  auto it = _prefetch_process[0].begin();
  assert(it != _prefetch_process[0].end() && "should have entry while popping from it it");
  (it->second).pop_front();
  if ((it->second).empty())
  {
    _prefetch_process[0].erase(it->first);
  }
}

void asic_core::push_gcn_cache_hit(pref_tuple cur_tuple)
{
  _priority_hit_gcn_updates.push_back(cur_tuple);
  ++_asic->_mem_ctrl->_l2hits;
  return;
  // ++_asic->_mem_ctrl->_l2accesses;
  // ++_asic->_mem_ctrl->_l2hits;
  // _hit_gcn_updates.push_back(cur_tuple);
  /*
  if(_asic->_config->_update_coalesce) {
    if (_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id]==0) {
      _hit_gcn_updates.push_back(cur_tuple);
    }
    ++_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id];
  } else {
    _hit_gcn_updates.push_back(cur_tuple);
  }*/
  // _hit_gcn_updates.push_back(cur_tuple);
}

// Step1: access the task queue to kill if this vid task is at the top of
// the queue (task queue prefetch rate should be slow for this case to be
// true)
// Step2: discard all the remaining updates (prevent computation somehow)
void asic_core::trigger_break_operations(int vid)
{
  // search for vid at the priority=-1 to kill it
  for (int tqid = 0; tqid < NUM_TQ_PER_CORE; ++tqid)
  {
    if (_pending_task[tqid].vid == vid)
    {
      _pending_task[tqid].vid = -1;
    }
    _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(vid);
    break;
    /*if(_task_queue[tqid][0].empty()) continue;
    auto it=_task_queue[tqid][0].begin();
    task_entry cur_task = (it->second).front(); //_task_queue[t].front();
    if(cur_task.vid==vid) { // pop the task
      (it->second).pop_front();
      if((it->second).empty()) {
        _task_queue[tqid][0].erase(it->first);
      }
      _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(vid);
      _asic->_task_ctrl->_remaining_task_entries[_core_id][tqid]++;
      break;
    }*/
  }
  // discard the pending updates in the pipeline (skip it for now)
}

// What if the bank queue is full? then the reduce can pop the data. Push over
// the network, and at the net_out_buffer, when it cannot pop -- it will apply
// a backpressure in the network?
bool asic_core::can_push_in_local_bank_queue(int bank_id)
{
  return (_local_bank_queues[bank_id].size() < FIFO_DEP_LEN);
}

// FIXME: not sure if required!!
void asic_core::set_dist_at_dequeue(task_entry &cur_task, pref_tuple &next_tuple)
{
  if (_asic->_config->_domain == tree)
  {
    return;
  }
  if (_asic->_config->_algo == pr)
  {
    return;
  }
  if ((_asic->_config->is_async() || _asic->_switched_async) && !_asic->_switched_sync && !_asic->_config->is_vector())
  {
    cur_task.dist = _asic->_scratch[cur_task.vid];
  }
  /*if(_asic->_config->_algo==pr) {
     // assert(cur_task.dist!=0 && "a task should not be created at src dist to be 0");
     _asic->_scratch[cur_task.vid]=0;
     if(_asic->_switched_async) {
       next_tuple.src_dist = cur_task.dist; // _asic->_scratch[cur_task.vid];// it->first;
       assert(next_tuple.src_dist>=0 && "correct src dist was assigned");
     }
  }*/
}

// FIXME: is it fixed again when data is returned from memory? (why is it set
// here?)
void asic_core::set_src_dist(task_entry cur_task, pref_tuple &next_tuple)
{
  // TODO: vidushi: it should use the dist at creation, not dequeue
  if (_asic->_config->_algo == pr)
  {
    // cout << "Vid: " << cur_task.vid << " dist of cur task: " << cur_task.dist << "\n";
    next_tuple.src_dist = cur_task.dist;
    return;
  }
  int src_id = next_tuple.src_id;
  if (_asic->_config->is_sync() || _asic->_switched_sync)
  {
    if (_asic->_switched_async && _asic->_config->_domain != tree)
    {
      next_tuple.src_dist = _asic->_scratch[src_id];
    }
    else
    {
      next_tuple.src_dist = cur_task.dist; // it->first;
    }
  }
  else if (_asic->_config->is_async())
  {
    next_tuple.src_dist = cur_task.dist; // it->first;
  }
  else if (_asic->_config->_exec_model == graphlab)
  {
    // if(_asic->_cur_cycle<_asic->_atomic_issue_cycle[src_id]+2) return;
    next_tuple.src_dist = _asic->_scratch[src_id];
  }
  else if (_asic->_config->_exec_model == dijkstra)
  {
    next_tuple.src_dist = _asic->_scratch[src_id];
  }
}

void asic_core::push_live_task(DTYPE priority_order, task_entry new_task)
{
  _asic->_stats->_stat_online_tasks++;
  if (_asic->_config->_prac == 0 || _asic->_config->_work_stealing == 0)
  {
    _asic->_task_ctrl->insert_new_task(0, 0, _asic->_scratch_ctrl->get_local_scratch_id(new_task.vid), priority_order, new_task);
    return;
  }
  int check_index = _agg_push_index;
  // then push into the queue
  int tqid = check_index;
  /*
  int sample=check_index/2;
  int one = sample*2;
  int two = (sample*2) + 1;
  tqid = one;

  int p1=INF;
  if(!_task_queue[one][0].empty()) {
    p1 = _task_queue[one][0].begin()->first;
  }
  int p2=INF;
  if(!_task_queue[two][0].empty()) {
    p2 = _task_queue[two][0].begin()->first;
  }

  // push new task in a core with higher priority...but we should also
  // try to balance the number of elements (that's done when elements are
  // pulled from the FIFO queue)
  if(p1<p2) tqid=two;
  if(p2<p1) tqid=one;
  if(_asic->_config->_num_tq_per_core==1) tqid=0;
  check_index=tqid;
  */
  _aggregation_buffer[check_index].push_back(make_pair((priority_order), new_task));
  _agg_push_index = (check_index + 1) % _asic->_config->_num_tq_per_core;
}

void asic_core::create_async_task(DTYPE priority_order, task_entry new_task)
{
  int dest_core_for_task = _asic->_scratch_ctrl->get_local_scratch_id(new_task.vid);
  if (_asic->_config->_perfect_lb == 0 && _asic->_config->_net == mesh)
  {
    assert(dest_core_for_task == _core_id && "computation is near data");
  }
  if (_asic->_config->_domain == tree)
  {
    dest_core_for_task = 0;
  }
  // int degree = _asic->_offset[new_task.vid+1]-_asic->_offset[new_task.vid];

  int slice_num = _asic->_scratch_ctrl->get_slice_num(new_task.vid);

  // statistics
  if (slice_num != _asic->_current_slice && !_asic->_switched_cache)
  {
    if (_asic->_config->_algo == pr)
    { // not sure why?
      _asic->_stats->_stat_tot_num_copy_updates++;
    }
  }
  else
  { // get stats for the percentage used in the current slice..
    if (_asic->_task_ctrl->_check_vertex_done_this_phase.test(new_task.vid) != 1)
    {
      _asic->_stats->_stat_owned_tasks_this_phase++;
      _asic->_task_ctrl->_check_vertex_done_this_phase.set(new_task.vid);
    }
  }

  // No need to push in task queue if 1. belong to other slice 2. it is the
  // finishing phase
  if ((slice_num != _asic->_current_slice && !_asic->_switched_cache) || _asic->_async_finishing_phase)
  {
    _asic->_slice_fill_addr[slice_num]++; // FIXME: What is this?

    if (_asic->_task_ctrl->_check_worklist_duplicate[slice_num].test(new_task.vid) != 1)
    {
      // cout << "Pushed a task to a different global task queue\n";
      _asic->_task_ctrl->push_task_into_worklist(slice_num, priority_order, new_task);
      _asic->_task_ctrl->_check_worklist_duplicate[slice_num].set(new_task.vid);
      // _asic->_stats->_stat_remote_tasks_this_phase++;
    }
    else
    {
      if (slice_num != _asic->_current_slice)
      {
        _asic->_stats->_stat_num_dupl_copy_vertex_updates++;
      }
    }
  }
  else
  { // push into the task queue here...
    _asic->_asic_cores[dest_core_for_task]->push_live_task(priority_order, new_task);
  }
}

// I think this is for multicast: we would like to wait for all dst_id so that
// we can multicast?
int asic_core::access_all_edges(int line_addr, task_entry cur_task, pref_tuple next_tuple)
{
  // TODO: this is the vectorized access of src vector, a different type of access
  // this seems like accessing all edges for this source vector? shouldn't this
  // be true for all edges?
  // next_tuple.arob_entry = cur_task.start_offset;
  int cur_task_start_offset = cur_task.start_offset;
  // cout << "Setting edge arob entry to: " << next_tuple.arob_entry << endl;
  int num_cache_lines_per_src = _asic->_offset[cur_task.vid + 1] - cur_task.start_offset;

  num_cache_lines_per_src = ceil(num_cache_lines_per_src * message_size / (float)line_size);
  assert(num_cache_lines_per_src != 0);

  if (_asic->_config->_algo == gcn && _asic->_config->_ladies_gcn == 0)
  {
    num_cache_lines_per_src = min(SAMPLE_SIZE, num_cache_lines_per_src);
  }
  int edges_served = _asic->_compl_buf[_core_id]->_entries_remaining; // 1 entry for each..

  assert(edges_served > 0 && "entries should be remaining in the completion buffer");
  if (num_cache_lines_per_src <= edges_served)
  { // cool, good to go!
    edges_served = _asic->_offset[cur_task.vid + 1] - cur_task.start_offset;
  }
  else
  { // serve only the edges that are left..
    num_cache_lines_per_src = edges_served;
    edges_served = edges_served * (line_size / message_size);
  }

  // num_cache_lines_per_src = ceil(num_cache_lines_per_src*4/(float)line_size);
  //  next_tuple.cb_entry = _asic->_compl_buf[_core_id]->allocate_cb_entry(num_cache_lines_per_src);
  if ((_asic->_config->_exec_model == sync || _asic->_config->_exec_model == sync_slicing || _asic->_switched_sync) && cur_task.start_offset == _asic->_offset[cur_task.vid])
  {
    // assuming I just assidned it for src vertices...!!!
    int cb_push = _asic->_compl_buf[_core_id]->_cur_cb_push_ptr;
    cb_push = (cb_push - 1) % COMPLETION_BUFFER_SIZE;
    if (cb_push < 0)
      cb_push += COMPLETION_BUFFER_SIZE;
    next_tuple.cb_entry = cb_push;
    _asic->_compl_buf[_core_id]->_reorder_buf[cb_push].waiting_addr += num_cache_lines_per_src;
    // cout << "Current waiting addr: " << _asic->_compl_buf[_core_id]->_reorder_buf[cb_push].waiting_addr << " at entry " << cb_push << endl;
  }
  else
  {
    next_tuple.cb_entry = _asic->_compl_buf[_core_id]->allocate_cb_entry(num_cache_lines_per_src);
  }
  // FIXME: is it sure that the updated entry will have this thing.. (if edge
  // is served first?)
  // next_tuple.entry_cycle = next_tuple.arob_entry + edges_served;
  next_tuple.start_edge_served = cur_task_start_offset;
  next_tuple.end_edge_served = cur_task_start_offset + edges_served;
  next_tuple.core_type = fineMem;
  // cout << "Sent memory request for start_edge: " << next_tuple.start_edge_served << " end edge: " << next_tuple.end_edge_served << endl;
  next_tuple.req_core = _core_id;
  // cout << "Sending edge src: " << next_tuple.src_id << " src_core: " << next_tuple.req_core << " cb_entry: " << next_tuple.cb_entry << " lines: " << num_cache_lines_per_src << " cycle: " << _asic->_cur_cycle << endl;
  for (int i = 0; i < num_cache_lines_per_src; ++i)
  {
    _asic->_mem_ctrl->send_mem_request(false, line_addr, next_tuple, EDGE_SID);
    line_addr += line_size;
  }
  // cout << "Sending edge access request for vid: " << next_tuple.src_id << " num cache lines: " << num_cache_lines_per_src << " core: " << _core_id << " cycle: " << _asic->_cur_cycle << endl;
  return edges_served;
  // int src_id = cur_task.vid;
  // cur_task.start_offset = _asic->_offset[src_id+1];
}

void asic_core::access_src_vertex_prop(task_entry cur_task, pref_tuple next_tuple)
{
  // return; // idea is that first edge should use the same entry
  // uint64_t addr_offset=E*4/line_size;
  // TODO: For GCN, it should send based on the current layer feature
  // size and the number of cache lines involved
  int num_cache_line_requests = 1;
  uint64_t vid_addr = (_asic->_mem_ctrl->_vertex_offset + cur_task.vid * 4 / line_size) * line_size;
  // FIXME: since it has more requests, some CB entries are not dropped and this is not the waiting_addr of the just the last element
  /*
#if GCN==1 || CF==1
  num_cache_line_requests = FEAT_LEN1*message_size/line_size;
  vid_addr = _asic->_mem_ctrl->_vertex_offset + (_asic->_scratch_ctrl->_mapping[cur_task.vid]*num_cache_line_requests)*line_size;
#endif
  */
  next_tuple.cb_entry = _asic->_compl_buf[_core_id]->allocate_cb_entry(num_cache_line_requests);
  assert((_asic->_config->_heter_cores == 0 || _core_id == _asic->_scratch_ctrl->get_local_scratch_id(next_tuple.src_id)) && "fine task should have affinity to its core");
  // cout << "Sending src_vertex_requests for entry: " << next_tuple.cb_entry << " req_core: " << _core_id << " for src: " << next_tuple.src_id << " lines: " << num_cache_line_requests << " cycle: " << _asic->_cur_cycle << endl;
  next_tuple.req_core = _core_id;
  // next_tuple.arob_entry = cur_task.start_offset;
  next_tuple.end_edge_served = -1;
  next_tuple.core_type = dummyCB;
  // cout << "Setting arob entry to: " << cur_task.start_offset << endl;
  for (int c = 0; c < num_cache_line_requests; ++c)
  {
    _asic->_mem_ctrl->send_mem_request(false, vid_addr, next_tuple, VERTEX_SID);
    vid_addr += line_size;
  }
}

int asic_core::access_edge(task_entry cur_task, int edge_id, pref_tuple next_tuple)
{
  if ((_asic->_config->_exec_model == sync || _asic->_config->_exec_model == sync_slicing || _asic->_switched_sync) && cur_task.start_offset == _asic->_offset[cur_task.vid] && !_asic->_config->_pull_mode)
  {
    access_src_vertex_prop(cur_task, next_tuple);
  }
  if (_asic->_config->_exec_model == async || _asic->_config->_exec_model == sync_slicing)
  {
    // TODO: access scratchpad for reading original vertex ID and store the
    // output in memory
  }

  // cout << "Remaining entries after serving source vertex prop: " << _asic->_compl_buf[_core_id]->_entries_remaining << endl;

  // Creating the memory tuple it is waiting for!!
  // Step 1: return entry (TODO: Why does it need extra entry if coalesced, any
  // specific access?)
  // int line_addr = _asic->_mem_ctrl->get_edge_addr(edge_id);
  int line_addr = _asic->_mem_ctrl->get_edge_addr(edge_id, cur_task.vid);

  next_tuple.req_core = _core_id;
  // _asic->_stats->_stat_dist_tasks_dequed[_core_id]++;
  _asic->_stats->_stat_dist_edges_dequed[_core_id]++;

  // Step2: copy the data already
  next_tuple.edge = _asic->_neighbor[edge_id];
  int edges_served = 1;
  // Step 1: access edges
  // #if ALL_EDGE_ACCESS==0 // FIXME: I am not sure what is this doing...
  if (_asic->_config->_all_edge_access == 0)
  {
    // this is for edge access -- this is for a single cache line???
    next_tuple.cb_entry = _asic->_compl_buf[_core_id]->allocate_cb_entry(1);
    next_tuple.core_type = fineMem;
    // cout << "Sending memory request for src_id: " << next_tuple.src_id << " and dest: " << next_tuple.edge.dst_id << endl;
    /*if(_asic->_config->_domain==tree) {
    // TODO: just push to completion buffer directly (ideally I should do a scratch read)
    // TODO: just want to load the location of the next memory location (that can be derived from vprop only)
    _asic->_scratch_ctrl->send_scratch_request(_core_id, line_addr, next_tuple); // should be scratch_addr
  } else {
    _asic->_mem_ctrl->send_mem_request(false, line_addr, next_tuple, EDGE_SID);
  }*/
    _asic->_mem_ctrl->send_mem_request(false, line_addr, next_tuple, EDGE_SID);

    // TODO: one by one? (so this function should be called by dispatch? why
    // different for sgu_gcn_reorder? can't we vectorize without it?) -- 1 cb
    // entry per edge (too small)
    // Oh some issue here
    /*if(next_tuple.src_id==38036) {
      cout << "Sending edge access request1 for vid: " << next_tuple.src_id << " num cache lines: " << 1 << " core: " << _core_id << " cycle: " << _asic->_cur_cycle << " and cb entry: " << next_tuple.cb_entry << endl;
  }*/
  }
  else
  {
    // TODO: why the above thing doesn't work for GCN?
    edges_served = access_all_edges(line_addr, cur_task, next_tuple);
  }
  assert(edges_served > 0 && "we should serve positive number of edges for gcn");
  return edges_served;
}

void asic_core::push_lsq_data(pref_tuple cur_tuple)
{
  // assert(_pref_lsq[0].size() < FIFO_DEP_LEN && "cannot push more data in pref lsq core");
  _pref_lsq[0].push(cur_tuple); // TODO: skipping lane id for now
}

void asic_core::send_scalar_packet(pref_tuple cur_tuple)
{
  // TODO: FIX for this case as well...
  int max_edges = _asic->_offset[cur_tuple.src_id + 1] - _asic->_offset[cur_tuple.src_id];
#if GCN == 1 && LADIES_GCN == 0
  max_edges = min(SAMPLE_SIZE, max_edges);
#endif
  int dst_core = -1;
  int src_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.src_id);

  // src_id, dst_id, lane_id=0
  net_packet multicast_net_packet[core_cnt]; // packet to each destination
  bitset<core_cnt> _dest_done;
  _dest_done.reset();

  for (int i = _asic->_offset[cur_tuple.src_id]; i < (_asic->_offset[cur_tuple.src_id] + max_edges); ++i)
  {
    // cout << "Issuing packets for src: " << cur_tuple.src_id << " dst: " << _asic->_neighbor[i].dst_id << endl;
    dst_core = _asic->_scratch_ctrl->get_local_scratch_id(_asic->_neighbor[i].dst_id);

    // Here destinations may not equal to the number of dst_id, FIXME: NET: do
    // they know this???
    // multicast_net_packet[dst_core].multicast_dst.push_back(_asic->_neighbor[i].dst_id);
    multicast_net_packet[dst_core].multicast_dst_wgt.push_back(make_pair(_asic->_neighbor[i].dst_id, _asic->_neighbor[i].wgt));
    _dest_done.set(dst_core);
  }

  // now push those packets
  for (int i = 0; i < core_cnt; ++i)
  {
    if (!_dest_done.test(i))
      continue;
    // if(net_packet[i].src_id==-1) continue;
    // cout << "i: " << i << " dst_core: " << net_packet[i].dest_core_id << endl;
    // assert(net_packet[i].dest_core_id==i);

    if (i == src_core || _asic->_config->_perfect_net)
    { // push to the current core for all required entries
      _asic->_network->_stat_local_updates++;
      for (unsigned j = 0; j < multicast_net_packet[i].multicast_dst_wgt.size(); ++j)
      {

        pref_tuple return_to_arob;
        return_to_arob.src_id = cur_tuple.src_id;
        return_to_arob.second_buffer = cur_tuple.second_buffer;
        return_to_arob.edge.dst_id = multicast_net_packet[i].multicast_dst_wgt[j].first;
        return_to_arob.edge.wgt = multicast_net_packet[i].multicast_dst_wgt[j].second;
        // return_to_arob.lane_id=0;
        int dest_core = _asic->_scratch_ctrl->get_local_scratch_id(multicast_net_packet[i].multicast_dst_wgt[j].first);
        // cout << "(Match) Src_id: " << cur_tuple.src_id << " and degree: " << _asic->get_degree(cur_tuple.src_id) << " and dst_id: " << return_to_arob.edge.dst_id << endl;
        // _asic->_asic_cores[dest_core]->_prefetch_process[0].push_back(return_to_arob);
        _asic->_asic_cores[dest_core]->insert_prefetch_process(0, 0, return_to_arob);
      }
    }
    else
    { // push to the network which does the same thing

      // in the real case, push N gcn packess for each combination and do the
      // same thing at the destination
#if PERFECT_NET == 0

      multicast_net_packet[i] = _asic->_network->create_vector_update_packet(cur_tuple.tid, cur_tuple.src_id, multicast_net_packet[i].multicast_dst_wgt, cur_tuple.vertex_data);
      // net_packet[i].dest_core_id = i;
      // net_packet[i].dst_id = -1; // _asic->_neighbor[i].dst_id;
      // net_packet[i].src_id = cur_tuple.src_id;

      // net_packet[i].packet_type=vectorized;
      // net_packet[i].req_core_id=-1;

      // for(int f=0; f<FEAT_LEN; ++f) multicast_net_packet[i].vertex_data[f] = cur_tuple.vertex_data[f];

      for (int l = 0; l < num_packets; ++l)
      {
        _asic->_network->push_net_packet(_core_id, multicast_net_packet[i]);
      }
#else
      // this is also if perfect_net=1
      // this should directly push to prefetch process queue
      // cout << "At core i: " << i << " multicast dest: " << net_packet[i].multicast_dst.size() << endl;
      // num_pops++;
      for (unsigned j = 0; j < multicast_net_packet[i].multicast_dst_wgt.size(); ++j)
      {
        // this should be for number of packets right?
        for (int l = 0; l < num_packets; ++l)
        {
          pref_tuple return_to_arob;
          return_to_arob.src_id = cur_tuple.src_id;
          return_to_arob.second_buffer = cur_tuple.second_buffer;
          return_to_arob.edge.dst_id = multicast_net_packet[i].multicast_dst_wgt[j].first;
          return_to_arob.edge.wgt = multicast_net_packet[i].multicast_dst_wgt[j].second;
          // return_to_arob.lane_id=0;
          return_to_arob.update_width = bus_width;
          int dest_core = i;

          for (int f = 0; f < FEAT_LEN; ++f)
            return_to_arob.vertex_data[f] = cur_tuple.vertex_data[f];
#if GRAPHMAT_SLICING == 1 && REUSE > 1
          if (_asic->_finishing_phase)
          { // this should actually be access to the cache
            _asic->_asic_cores[dest_core]->insert_prefetch_process(0, 0, return_to_arob);
          }
          else
          {
            int dest_slice = _asic->get_slice_num(cur_tuple.edge.dst_id);
            // cout << " current slice: " << _asic->_current_slice << " dest slice: " << dest_slice << endl;
            if (_asic->_current_slice != dest_slice)
            {
              _asic->_delta_updates[_asic->_current_reuse].push_back(make_pair(cur_tuple.edge.dst_id, -1));
            }
            else
            {
              _asic->_asic_cores[dest_core]->insert_prefetch_process(0, 0, return_to_arob);
            }
          }
#else
          // cout << "Src_id: " << cur_tuple.src_id << " and degree: " << _asic->get_degree(cur_tuple.src_id+1) << endl;
          _asic->_asic_cores[dest_core]->insert_prefetch_process(0, 0, return_to_arob);
#endif
        }
      }
#endif
    }
  }
}

void asic_core::send_multicast_packet(pref_tuple cur_tuple, int start_offset, int end_offset)
{
  net_packet net_tuple;
  vector<iPair> mcast_dst_wgt;
  /*for(int i=start_offset; i<end_offset; ++i) {
     cout << "Sending edges from start: " << start_offset << " end: " << end_offset << " for src: " << cur_tuple.src_id << endl;
   }*/
  // there have been second buffer tasks as well (what if something happened
  // over network?)
  // cout << "Serving source id: " << cur_tuple.src_id << " start_offset: " << start_offset << " end_offset: " << end_offset << " buffer: " << cur_tuple.second_buffer << endl;
  int src_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.src_id);
  int dst_core = -1;
  int max_edges = end_offset - start_offset;
  for (int i = start_offset; i < end_offset; ++i)
  {
    _asic->_edges_served++; // FIXME: here there are more than the things there.. (edges_served)

    int slice_num = _asic->_scratch_ctrl->get_slice_num(_asic->_neighbor[i].dst_id);

    // just stats calculation for the l2 hits
    if (slice_num != _asic->_current_slice)
    {
      _asic->_mem_ctrl->_l2accesses++;
      uint64_t paddr = _asic->_neighbor[i].dst_id * message_size * FEAT_LEN;
      bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0); // check cache statistics
      if (l2hit)
        _asic->_mem_ctrl->_l2hits++;
    }

    // cout << " src_id: " << cur_tuple.src_id << " src_core: " << src_core << " current core: " << _core_id << endl;

    assert((_asic->_config->_heter_cores == 0 || _core_id == src_core) && "source vertex data should be ready at its data location");
    dst_core = _asic->_scratch_ctrl->get_local_scratch_id(_asic->_neighbor[i].dst_id);

    // Okay, correct till here...
    if (src_core == dst_core)
      _asic->_debug_local++;
    else
      _asic->_debug_remote++;

    net_tuple.multicast_dst_wgt.push_back(make_pair(_asic->_neighbor[i].dst_id, _asic->_neighbor[i].wgt));
    mcast_dst_wgt.push_back(make_pair(_asic->_neighbor[i].dst_id, _asic->_neighbor[i].wgt));
#if PERFECT_NET == 0

#if GRAPHMAT_SLICING == 1 && REUSE > 1 // multicast & network & sliced push
    if (_asic->_finishing_phase)
    { // this should actually be access to the cache
      net_tuple.multicast_dst_core_id.push_back(dst_core);
    }
    else
    {
      int dest_slice = _asic->_scratch_ctrl->get_slice_num(_asic->_neighbor[i].dst_id);
      if (_asic->_current_slice != dest_slice)
      {
        _asic->_delta_updates[_asic->_current_reuse].push_back(make_pair(_asic->_neighbor[i].dst_id, -1));
      }
      else
      {
        net_tuple.multicast_dst_core_id.push_back(dst_core);
      }
    }
#else // multicast & network & normal push
    net_tuple.multicast_dst_core_id.push_back(dst_core);
#endif

#else // for pefect_net==1
    // TODO: push only if it belongs to the current core -- this is when
    // perfect net=1
#if GRAPHMAT_SLICING == 1 && REUSE > 1
    int dest_slice = _asic->get_slice_num(_asic->_neighbor[i].dst_id);
    if (_asic->_finishing_phase || _asic->_current_slice == dest_slice)
    {
      net_tuple.multicast_dst_core_id.push_back(src_core);
    }
    else
    {
      _asic->_delta_updates[_asic->_current_reuse].push_back(make_pair(_asic->_neighbor[i].dst_id, -1));
    }
#else
    net_tuple.multicast_dst_core_id.push_back(src_core);
#endif
#endif
  }

  // TODO/FIXME: remove redundant calculations of dest core id
  net_tuple = _asic->_network->create_vector_update_packet(cur_tuple.tid, cur_tuple.src_id, mcast_dst_wgt, cur_tuple.vertex_data);
  net_tuple.second_buffer = cur_tuple.second_buffer;

  if (net_tuple.multicast_dst_wgt.size() > 0)
  {
    /*for(int i=0; i<net_tuple.multicast_dst_core_id.size(); ++i) {
       // okay dest here are only less
       cout << "Dst size: " << net_tuple.multicast_dst.size() << " dst core: " << net_tuple.multicast_dst_core_id.size() << endl;
     }*/
    for (int k = 0; k < num_packets; ++k)
    {
      net_tuple.push_cycle = k;
      // TODO: check how many pulled from multicast.. (till now I have not
      // put barrier here)
      _asic->_network->push_net_packet(_core_id, net_tuple);
    }
  }
}

void asic_core::access_cache_for_gcn_updates(pref_tuple cur_tuple, int start_offset, int end_offset)
{
  bool l2hit = true;
  int ratio = line_size / bus_width;
  for (int i = start_offset; i < end_offset; ++i)
  {
    // TODO: put a condition when dst core is full, if not, then send the index
    uint64_t base_addr = _asic->_mem_ctrl->_vertex_offset; // _asic->_neighbor[i].dst_id*message_size*FEAT_LEN;
    for (int j = 0; j < num_packets; ++j)
    {
      pref_tuple update_tuple;
      update_tuple.end_edge_served = j;
      update_tuple.update_width = bus_width;
      update_tuple.edge.dst_id = _asic->_neighbor[i].dst_id;
      // if(_asic->_neighbor[i].dst_id==2404)
      //   cout << "src was: " << cur_tuple.src_id << endl;
      // update_tuple.lane_id=0;
      update_tuple.src_id = cur_tuple.src_id;
      /*if(cur_tuple.second_buffer) {
          cout << "A second buffer agg was identified in the memory request queue\n";
      }*/
      update_tuple.second_buffer = cur_tuple.second_buffer;
      // so src_id makes it double, hence we can get 2x speedup..
      uint64_t paddr = base_addr + (update_tuple.edge.dst_id * message_size * FEAT_LEN) + (j * bus_width);
      // if(j%ratio==0)
      int dst_core = _asic->_scratch_ctrl->get_local_scratch_id(update_tuple.edge.dst_id);
      if (_asic->_config->_cache_hit_aware_sched)
      {
        update_tuple.src_dist = _asic->_cur_cycle;
        int priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, 0, -1, update_tuple.edge.dst_id, _asic->_local_coalescer[_core_id]->_batch_width, -1);
        _asic->_asic_cores[dst_core]->insert_prefetch_process(0, priority, update_tuple);
        continue;
      }
      bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
      // cout << "Access request at address: " << paddr << " and dst_id: " << cur_tuple.edge.dst_id << " hit? " << l2hit << endl;
      // TODO: I could use the same algorithm to push in the task queue -- could even
      // use the same function (just need to update the data-structure for
      // prefetch_process)

      if (!l2hit)
      {
        if (_asic->_config->_cache_hit_aware_sched)
        {
          // TODO: If same dst_id is available in the miss_gcn queue, merge it and increase the numbers of edges executed.
          bool found = false;
          /*for(auto it=_miss_gcn_updates.begin(); it!=_miss_gcn_updates.end(); ++it) {
                if(it->edge.dst_id==update_tuple.edge.dst_id) {
                    found=true;
                    ++_asic->_stats->_stat_tot_finished_edges;
                    ++_asic->_mem_ctrl->_l2hits;
                    ++_asic->_mem_ctrl->_l2accesses;
                    if(!update_tuple.second_buffer) {
                        --_asic->_correct_vertex_data[update_tuple.edge.dst_id];
                    } else {
                        --_asic->_correct_vertex_data_double_buffer[update_tuple.edge.dst_id];
                    }
                    // break;
                }
            }*/
          if (!found)
          {
            update_tuple.src_dist = _asic->_cur_cycle;
            push_gcn_cache_miss(update_tuple);
          }
          else
          {
            cout << "Merged an update from miss queue GCN for dst: " << update_tuple.edge.dst_id << "\n";
          }
        }
        else
        {
          update_tuple.cb_entry = -1; // unordered packet
          update_tuple.req_core = _core_id;
          bool sent_request = _asic->_mem_ctrl->send_mem_request(false, paddr, update_tuple, VERTEX_SID);
          // FIXME: even will all miss, this is the coalescing which is leading
          // to so many hits..(So what is this condition??)
          // if(j%ratio==0) {
          if (!sent_request)
            _asic->_mem_ctrl->_l2hits++;
          // else cout << "Sent scratch request for vid: " << update_tuple.src_id << endl;
          _asic->_mem_ctrl->_l2accesses++;
          // }
          _asic->_stats->_stat_pending_cache_misses++;
          _asic->_mem_ctrl->_num_pending_cache_reqs++;
        }
      }
      else
      {
        // cout << "Sending update for edge i: " << i << endl;
        // if(j%ratio==0)
        if (_asic->_config->_cache_hit_aware_sched)
        {
          update_tuple.src_dist = _asic->_cur_cycle;
          // _asic->_asic_cores[dst_core]->_hit_gcn_updates.push_back(update_tuple);
          push_gcn_cache_hit(update_tuple);
        }
        else
        {
          _asic->_mem_ctrl->_l2hits++;
          _asic->_mem_ctrl->_l2accesses++;
          // _asic->_asic_cores[dst_core]->_prefetch_process[0].push_back(update_tuple);
          _asic->_asic_cores[dst_core]->insert_prefetch_process(0, 0, update_tuple);
        }
      }

      // just push to delta updates as well, because we do not want to apply it
      if (_asic->_config->_exec_model == sync_slicing && _asic->_config->_reuse > 1 && !_asic->_finishing_phase)
      {
        int dest_slice = _asic->_scratch_ctrl->get_slice_num(update_tuple.edge.dst_id);
        if (_asic->_current_slice != dest_slice)
        {
          _asic->_delta_updates[_asic->_current_reuse].push_back(make_pair(update_tuple.edge.dst_id, -1));
        }
      }
    }
  }
}

void asic_core::generate_gcn_net_packet(pref_tuple cur_tuple, int start_offset, int end_offset)
{
  int max_edges = end_offset - start_offset;

  // TODO: for working cache, do as it would be for perfect network case or
  // when src_id matches core_id (WE WOULD ALSO LIKE TO LIMIT BANDWIDTH)
  if (_asic->_config->_working_cache)
  {
    // scan over dst id, find hits and push those in arob
    // if not matching, send requests for misses (and this would be multiple
    // cache lines, if half are available?)
    // accumulate these in send_mem_request...no cmpl_buf -- but something to
    // make sure accumulation is available...
    access_cache_for_gcn_updates(cur_tuple, start_offset, end_offset);
    return;
  }

  // Oh, it is creating packets for all its edges here
  if (_asic->_config->_net_traffic == real_multicast || _asic->_config->_net_traffic == path_multicast)
  {
    send_multicast_packet(cur_tuple, start_offset, end_offset);
  }
  else
  {
    send_scalar_packet(cur_tuple);
  }
}

void asic_core::consume_lsq_responses(int lane_id)
{

  if ((_asic->_config->_net != crossbar && _asic->_config->_net == hrc_xbar) && _asic->_cur_cycle % _asic->_xbar_bw == 0)
    return;
  // TODO: Different kind of responses, based on how it was consumed

  if (!_asic->_config->is_vector() || (_asic->_config->is_vector() && _asic->_config->_pull_mode == 1))
  {
    // these are responses corresponding to the edges
    int elem_done = 0;
    while (!_pref_lsq[lane_id].empty() && can_push_in_process(lane_id))
    {

      // cout << "Core: " << _core_id << " consuming a value from lsq cycle: " << _asic->_cur_cycle << endl;
      ++elem_done;
      pref_tuple cur_tuple = _pref_lsq[lane_id].front();

      if (_asic->_config->_pr == 0 && _asic->_config->_domain == graphs)
      { // could be src_dist for synchronous execution
        if ((_asic->_config->_exec_model == sync || _asic->_config->_exec_model == sync_slicing || _asic->_switched_sync) && !_asic->_switched_async)
        {
          cur_tuple.src_dist = _asic->_vertex_data[cur_tuple.src_id];
        }
        else
        {
          cur_tuple.src_dist = _asic->_scratch[cur_tuple.src_id];
        }
      }
      assert(cur_tuple.src_dist >= 0);
      if (_asic->_config->_pull_mode)
      {
        cur_tuple.start_edge_served = _asic->_offset[cur_tuple.src_id];
        _prefetch_scratch.push(cur_tuple);
      }
      else
      {
        insert_prefetch_process(lane_id, 0, cur_tuple);
      }
      _pref_lsq[lane_id].pop();
    }
    // cout << "In consume lsq responses, elem done: " << elem_done << " at cycle: " << _asic->_cur_cycle << endl;
  }
  else
  { // Step 2: do something different for vectorized values? (no computation, directly send reduce packet over the network)

    // these are responses corresponding to the edges
    // If perfect_net=1 for the purposes of crossbar, source should not be
    // allowed to issue more than 8*8 requests more cycle (so 1)
    int num_pops = 0;
    int max_pops = 1;
    // FIXME: @vidushi: Why more max_ops increase execution time? (does HOL come
    // into picture?)
    if (_asic->_config->_tc == 1 && MAX_DFGS > 1)
    {
      max_pops = MAX_DFGS;
    }
    // if(_asic->_config->_core_cnt==1) max_pops=16; // ideal
    // FIXME: checking: remove
    // max_pops=16; // ideal
    /*if(!_asic->_network->can_push_in_network(_core_id)) {
    cout << "Could not push in network\n";
  }*/
    while (!_pref_lsq[lane_id].empty() && num_pops < max_pops && _asic->_network->can_push_in_network(_core_id))
    {
      // cout << "Came in to send network packet for GCN\n";
#if WORKING_CACHE == 1
      // FIXME: should this be proportional to the number of cache misses which
      // might be sent in this case?
      if (_asic->_mem_ctrl->_num_pending_cache_reqs >= MAX_CACHE_MISS_ALLOWED)
        return;
#endif

      // cout << "number of packets to send: " << num_packets << endl;
      pref_tuple cur_tuple = _pref_lsq[lane_id].front();
      // cout << "Reading from memory, src_id: " << cur_tuple.src_id << " and edge: " << cur_tuple.edge.dst_id << " end edge: " << cur_tuple.end_edge_served << endl;
      int max_edges = _asic->_offset[cur_tuple.src_id + 1] - _asic->_offset[cur_tuple.src_id];
      num_pops++;

      // push this packet into the network
      // Step1: allocate an entry into the atomic rob
      int src_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.src_id);

      // combined tuple for a source vertex
      int dst_core = -1; // _asic->get_local_scratch_id(cur_tuple.edge.dst_id);

#if PERFECT_NET == 1 // TODO: if ALL_EDGE_ACCESS
      // TODO: if this is more than 1, maybe i need to run in loop and just change start_edge_served
      cur_tuple.edge = _asic->_neighbor[cur_tuple.start_edge_served];
      dst_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.edge.dst_id);
      // TODO: FIXME: this is the new part for the cache hit aware sched...
      if (_asic->_config->_working_cache && !_asic->_config->_cache_hit_aware_sched)
      { // this is perfect net
        if (_asic->_asic_cores[dst_core]->can_push_in_gcn_bank())
        {
          access_cache_for_gcn_updates(cur_tuple, cur_tuple.start_edge_served, cur_tuple.end_edge_served);
          _pref_lsq[lane_id].pop();
        }
        continue;
      }

      int s = cur_tuple.start_edge_served;
      for (s = cur_tuple.start_edge_served; s < cur_tuple.end_edge_served; ++s)
      {
        cur_tuple.edge = _asic->_neighbor[s];
        dst_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.edge.dst_id);
        // FIXME: checking: remove
        // dst_core=0;
        assert(_asic->_asic_cores[dst_core]->_fine_grain_throughput > 0 && "should push only to active cores");
        // cout << "In cycle: " << _asic->_cur_cycle << " pushed update from src_id: " << cur_tuple.src_id << " and dst: " << cur_tuple.edge.dst_id << " core: " << _core_id << " cycle: " << _asic->_cur_cycle << endl;
        // TODO: if it cannot push, we should push it to another core. This needs to allow remote updates at any core; with a little extra network traffic
        if (_asic->_asic_cores[dst_core]->can_push_in_process(0))
        { // } || _asic->_reconfig_flushing_phase) {
          // _asic->_asic_cores[dst_core]->_prefetch_process[0].push_back(cur_tuple);
          for (int f = 0; f < FEAT_LEN / 16; ++f)
          { // this is like sending the source feature vector
            int cache_priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, 0, -1, cur_tuple.edge.dst_id, -1, 4);
            _asic->_asic_cores[dst_core]->insert_prefetch_process(0, cache_priority, cur_tuple);
          }
        }
        else
        {
          // this is to avoid HOL blocking...doesn't hurt for 1024 cycles..
          // this should the next unblocked core
          // dst_core = (dst_core+1)%core_cnt;
          // int min_pref_size=_agg_fifo_length;
          for (int x = (dst_core + 1) % core_cnt, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
          {
            bool cond = false;
            if (_asic->_config->_prefer_tq_latency)
            {
              cond = (_asic->_fine_alloc[x] == 1);
            }
            else
            {
              cond = (_asic->_asic_cores[x]->_fine_grain_throughput > 0);
            }
            // it should find the core with most empty prefetch--process
            // int cur_pref_size = _asic->_asic_cores[x]->_prefetch_process[0].size();
            if (cond && _asic->_asic_cores[x]->can_push_in_process(0))
            { // } && cur_pref_size<min_pref_size) {
              // min_pref_size = cur_pref_size;
              dst_core = x;
              break;
            }
          }
          // FIXME: this has pushed too many extra things but it should not block that core..
          // it should push in memory and get it back when that prefetch process queue frees up
          // it is like an overflow buffer for the prefetch process -- but with an overflow buffer, there will not be any redistribution
          // flushing of the pipeline need not serve the overflow updates

          if (_asic->_asic_cores[dst_core]->can_push_in_process(0))
          { // } || _asic->_reconfig_flushing_phase) {
            // cout << "Distributed to other core: " << dst_core << " cycle: " << _asic->_cur_cycle << " flushing phase: " << _asic->_reconfig_flushing_phase << endl;
            for (int f = 0; f < FEAT_LEN / 16; ++f)
            { // this is like sending the source feature vector
              int cache_priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, 0, -1, cur_tuple.edge.dst_id, -1, 4);
              _asic->_asic_cores[dst_core]->insert_prefetch_process(0, cache_priority, cur_tuple);
            }
          }
          else
          {
            if (_asic->_reconfig_flushing_phase)
            { // this must have been wasting a lot of cycles..in the initial phase
              _asic->_task_ctrl->_pending_updates.push(cur_tuple);
            }
            else
            {
              // old implementation
              _pref_lsq[lane_id].front().start_edge_served = s;
              break;
            }
          }
        }
      }
      if (s == cur_tuple.end_edge_served)
      {
        /*if(cur_tuple.second_buffer) {
          cout << "Served src: " << cur_tuple.src_id << " start_edge: " << cur_tuple.start_edge_served << " end_edge: " << cur_tuple.end_edge_served << endl;
        }*/
        _pref_lsq[lane_id].pop();
      }
      continue;
#endif

      // FIXME: req_core_id is not the correct location for the returned edges
      dst_core = -1;
      if (_asic->_config->_sgu_gcn_reorder == 1)
      {
        int req_to_serve = cur_tuple.end_edge_served - cur_tuple.start_edge_served;
        int can_serve = min(MULTICAST_LIMIT, req_to_serve);
        if (can_serve == req_to_serve)
        {
          generate_gcn_net_packet(cur_tuple, cur_tuple.start_edge_served, cur_tuple.end_edge_served);
          _pref_lsq[lane_id].pop();
        }
        else
        {
          // possible it was not popping out...
          generate_gcn_net_packet(cur_tuple, cur_tuple.start_edge_served, cur_tuple.start_edge_served + can_serve);
          _pref_lsq[lane_id].front().start_edge_served = cur_tuple.start_edge_served + can_serve;
        }
        break;
      }
      _pref_lsq[lane_id].pop();

#if GRAPHMAT_CF == 0
      if (src_core == dst_core)
      {
        int priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, 0, -1, cur_tuple.edge.dst_id, _asic->_remote_coalescer[_core_id]->_batch_width, -1);
        _asic->_asic_cores[dst_core]->insert_prefetch_process(0, priority, cur_tuple);
        continue;
      }
#endif

#if PERFECT_NET == 1 || NETWORK == 0

#if GRAPHMAT_CF == 1 // correctly manage crossbar...
      assert(src_core == _core_id);
      // cur_tuple.arob_entry = _asic->_asic_cores[dst_core]->_cur_arob_push_ptr;
      _asic->_asic_cores[src_core]->_crossbar_input.push(cur_tuple);
#else
      dst_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.edge.dst_id);
      // _asic->_asic_cores[dst_core]->_prefetch_process[0].push_back(cur_tuple);
      int cache_priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, 0, -1, cur_tuple.edge.dst_id, -1, 4);
      _asic->_asic_cores[dst_core]->insert_prefetch_process(0, cache_priority, cur_tuple);
#endif
#else // for a mesh with a scalar access
      for (int i = 0; i < num_packets; ++i)
      {
        // push a special type of reduction tuple into the network which has a reference to an entry into some (completion buffer type) which finally pushes to the prefetch_process
        // source->mem_ctrl, dst->core where this dst_id resides
        net_packet net_tuple = _asic->_network->create_noopt_vector_update_packet(cur_tuple.src_id, cur_tuple.edge.dst_id, cur_tuple.vertex_data);
        // int src_core = _asic->get_mc_core_id(cur_tuple.src_id);
        _asic->_network->push_net_packet(_core_id, net_tuple);
      }
#endif

      // _asic->_asic_cores[dst_core]->_cur_arob_push_ptr = (_asic->_asic_cores[dst_core]->_cur_arob_push_ptr+1)%AROB_SIZE;

      // _prefetch_process[lane_id].push(cur_tuple);
    }
  }
}

// deal with index, index+1
void asic_core::arbit_aggregation_buffer(int index)
{
  // if odd cycle, send smaller to index else send smaller to index+1

  int p1 = INF;
  int p2 = INF;

  auto it1 = _aggregation_buffer[index].begin();
  if (it1 != _aggregation_buffer[index].end())
  {
    p1 = it1->first;
  }

  auto it2 = _aggregation_buffer[index + 1].begin();
  if (it2 != _aggregation_buffer[index + 1].end())
  {
    p2 = it2->first;
  }

  int tqid1 = index;     // higher priority tqid
  int tqid2 = index + 1; // higher priority tqid

  if (!_odd_cycle)
  {
    tqid2 = index;
    tqid1 = index + 1;
  }

  if (p1 <= p2)
  { // tqid1 should get p1
    if (p1 != INF)
    {
      int dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id((*it1).second.vid);
      _asic->_task_ctrl->insert_new_task(tqid1, 0, dest_core_id, p1, (*it1).second);
      _asic->_stats->_stat_online_tasks++;
    }
    if (p2 != INF)
    {
      int dest_core_id2 = _asic->_scratch_ctrl->get_local_scratch_id((*it2).second.vid);
      _asic->_task_ctrl->insert_new_task(tqid2, 0, dest_core_id2, p2, (*it2).second);
      _asic->_stats->_stat_online_tasks++;
    }
  }

  if (p1 > p2)
  { // tqid1 should get p2
    int dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id((*it2).second.vid);
    _asic->_task_ctrl->insert_new_task(tqid1, 0, dest_core_id, p2, (*it2).second);
    _asic->_stats->_stat_online_tasks++;
    if (p1 != INF)
    {
      int dest_core_id1 = _asic->_scratch_ctrl->get_local_scratch_id((*it1).second.vid);
      _asic->_task_ctrl->insert_new_task(tqid2, 0, dest_core_id1, p1, (*it1).second);
      _asic->_stats->_stat_online_tasks++;
    }
  }

  _odd_cycle = !_odd_cycle;
}

// extract top and push the first element in the local task queue
void asic_core::drain_aggregation_buffer(int index)
{
  // cout << "Came in draining with index: " << index << endl;
  auto it = _aggregation_buffer[index].begin();
  if (it == _aggregation_buffer[index].end())
    return;
  _asic->_stats->_stat_online_tasks++;
  if (!_asic->_config->_presorter)
  {
    int dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id((*it).second.vid);
    _asic->_stats->_stat_enq_this_cycle[dest_core_id]++;
    _asic->_task_ctrl->insert_new_task(index, 0, dest_core_id, (*it).first, (*it).second);
    _aggregation_buffer[index].erase(it);
  }
  else
  {
    auto sorted_it2 = _aggregation_buffer[index].begin();
    DTYPE smallest_prio = 100000;
    /*
    bool go_in=false;
    for(auto it=_aggregation_buffer[index].begin();it!=_aggregation_buffer[index].end();++it) {
        DTYPE cur_prio = (*it).first;
        if(cur_prio<smallest_prio) {
            smallest_prio=cur_prio;
            sorted_it2=it;
            go_in=true;
        }
    }
    assert(go_in && "at least one element should be smaller");
    */
    int dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id((*sorted_it2).second.vid);
    _asic->_stats->_stat_enq_this_cycle[dest_core_id]++;
    _asic->_task_ctrl->insert_new_task(index, 0, dest_core_id, (*sorted_it2).first, (*sorted_it2).second);
    _aggregation_buffer[index].erase(sorted_it2);
  }
}

void asic_core::spec_serve_atomic_requests()
{ // int lane_id) {
  assert((_asic->_config->_exec_model == async || _asic->_config->_exec_model == async_slicing || ((_asic->_config->is_sync()) && _asic->_switched_async) && !_asic->_switched_sync) && "should not come in speculative for the case of graphmat");
  red_tuple cur_tuple;
  int total_requests_served = 0;
  for (int i = 0; i < _asic->_config->_num_banks; ++i)
  {
#if NETWORK == 1
    if (!_local_bank_queues[i].empty())
    {
      cur_tuple = _local_bank_queues[i].front();
#else
    if (!_asic->_bank_queues[i].empty())
    {
      ++total_requests_served;
#if PRIO_XBAR == 1
      auto it = _asic->_bank_queues[i].begin();
      cur_tuple = (it->second).front();
#else
      cur_tuple = _asic->_bank_queues[i].front();
#endif
#if XBAR_ABORT == 1
      assert(_asic->_scratch_ctrl->_present_in_bank_queue[i].test(cur_tuple.dst_id) == 1 && "bank bit should be set");
      _asic->_scratch_ctrl->_present_in_bank_queue[i].reset(cur_tuple.dst_id);
#endif
      /*
      DTYPE ewgt=-1;
      for(int a=_asic->_offset[cur_tuple.src_id]; a<_asic->_offset[cur_tuple.src_id+1]; ++a) {
          if(_asic->_neighbor[a].dst_id==cur_tuple.dst_id) {
              ewgt = _asic->_neighbor[a].wgt;
              break;
          }
      }
      assert(ewgt!=-1 && "should have found an edge");
      cur_tuple.new_dist = _asic->process(ewgt, _asic->_scratch[cur_tuple.src_id]);
      */
      // cout << "dst: " << cur_tuple.dst_id << " update: " << cur_tuple.new_dist << endl;
#endif
      // cout << "At bank, source: " << cur_tuple.src_id << " src_dist: " << _asic->_scratch[cur_tuple.src_id] << " and dest: " << cur_tuple.dst_id
      // << " dest_distance: " << _asic->_scratch[cur_tuple.dst_id] << " new dist: " << cur_tuple.new_dist << endl;
      _asic->_stats->_stat_fine_gran_lb_factor_atomic[_core_id]++;
      _asic->_gcn_updates[_core_id * _asic->_config->_num_banks + i]++;

#if SWARM == 1
      // reduce waiting count of the source core
      int src_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.src_id);
      _asic->_asic_cores[src_core]->_waiting_count[cur_tuple.lane_id]--;
      assert(_asic->_asic_cores[src_core]->_waiting_count[cur_tuple.lane_id] >= 0);
      // which in-order core this dst_id belonged to?
      if (_asic->_asic_cores[src_core]->_waiting_count[cur_tuple.lane_id] == 0)
      {
        // cout << "Came here to reset in order flag\n";
        _asic->_asic_cores[src_core]->_in_order_flag[cur_tuple.lane_id] = true;
        int task_time = (_asic->_cur_cycle - _asic->_asic_cores[src_core]->_prev_start_time[cur_tuple.lane_id]);
        _asic->_stats->_stat_tot_tasks_dequed++;
        _asic->_avg_task_time += task_time;
        /*if(src_core==0 && cur_tuple.lane_id==0) {
          cout << "ENDING TASK at time: " << _asic->_cur_cycle << endl;
          cout << "task time: " << task_time << endl;
        }*/
      }
#endif

#if NETWORK == 1 && ASTAR == 1
      // cout << "COME IN ASTART REGION\n";
      if (cur_tuple.req_core_id != -1)
      { // read request
        cur_tuple.dest_core_id = cur_tuple.req_core_id;
        cur_tuple.req_core_id = -1;
        _asic->_network->push_net_packet(_core_id, cur_tuple);
        _local_bank_queues[i].pop_back();
        continue;
      }

#endif

      bool found = false; // already there, no need to create task
      if (!_asic->should_spawn_task(_asic->_scratch[cur_tuple.dst_id], cur_tuple.new_dist))
      {
        _asic->_stats->_stat_abort_with_distance++;
        _asic->_stats->_stat_tot_aborted_edges++;
        found = true;
      }
      else
      {
        if (_asic->_scratch[cur_tuple.dst_id] == _asic->_correct_vertex_data[cur_tuple.dst_id])
        {
          _asic->_tot_useful_edges_per_iteration++;
        }
      }

      if (_asic->_config->_domain == tree)
      { // should always create new task for a tree
        found = false;
      }

      /*if(!found) {
       cout << "(NOT) Edge not forked for vid: " << cur_tuple.dst_id << " current dist: " << cur_tuple.new_dist << endl;
     } else {
       cout << "(YES) Edge not forked for vid: " << cur_tuple.dst_id << " current dist: " << cur_tuple.new_dist << endl;
     }*/

      // if sliced-scratchpad, send copy vertex updates to aggregation cache
      /* if(_asic->_config->_slice_count>1 && !_asic->_switched_cache) {
       int dst_slice = _asic->_scratch_ctrl->get_slice_num(cur_tuple.dst_id);
       if(dst_slice!=_asic->_current_slice) { // required for a copy vertex heuristic
         _asic->_stats->_stat_tot_num_copy_updates++;
       } 
       if(dst_slice!=_asic->_current_slice) { // send to aggregation cache
         int new_loc = E + _asic->_scratch_ctrl->_old_vertex_to_copy_location[_asic->_current_slice][cur_tuple.dst_id];
         uint64_t paddr = new_loc*message_size;
         bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0); // check cache statistics
        _asic->_mem_ctrl->_l2accesses++;
        if(!l2hit) {
          pref_tuple temp;
          _asic->_mem_ctrl->send_mem_request(true, paddr, temp, VERTEX_SID);
          _asic->_mem_ctrl->_cur_writes--;
        } else {
          _asic->_mem_ctrl->_l2hits++;
        }
      }
    }*/

      // Reduction
      DTYPE old_scratch = _asic->_scratch[cur_tuple.dst_id];
      _asic->_scratch[cur_tuple.dst_id] = _asic->reduce(_asic->_scratch[cur_tuple.dst_id], cur_tuple.new_dist);
      // cout << "After process, reducing dst vertex: " << cur_tuple.dst_id << " final value: " << _asic->_scratch[cur_tuple.dst_id] << endl;
      DTYPE diff = abs(old_scratch - _asic->_scratch[cur_tuple.dst_id]);
      if (old_scratch == MAX_TIMESTAMP)
      {
        // diff = 1;
        diff = -_asic->_scratch[cur_tuple.dst_id];
      }
      // _asic->_prev_gradient += diff;
      _asic->_prev_gradient += abs(_asic->_correct_vertex_data[cur_tuple.dst_id] - _asic->_scratch[cur_tuple.dst_id]);
      if (_asic->_task_ctrl->_present_in_queue.test(cur_tuple.dst_id) == 0)
      {
        _asic->_unique_vertices++;
      }
      _asic->_task_ctrl->_present_in_queue.set(cur_tuple.dst_id);

      // cout << "Final dist at the destination: " << cur_tuple.dst_id << " is: " << _asic->_scratch[cur_tuple.dst_id] << " and new dist used to update it: " << cur_tuple.new_dist << endl;
      _asic->_scratch_ctrl->_num_spec[cur_tuple.dst_id]++;
      _asic->_stats->_stat_num_req_per_bank[i]++;
      // When reduced in banks for sgu
      _asic->_stats->_stat_tot_finished_edges++;

#if PROFILING == 1
      int eid = 0;
      bool found_edge = false;
      // collect index of vid pair in neighbor array for extra work info
      for (int k = _asic->_offset[cur_tuple.src_id]; k < _asic->_offset[cur_tuple.src_id + 1]; ++k)
      {
        if (_asic->_neighbor[k].dst_id == cur_tuple.dst_id)
        {
          found_edge = true;
          eid = k;
          break;
        }
      }
      assert(found_edge && "Didn't find edge -- bad!!");
      _asic->_edge_freq[eid]++;
#endif

// META_INFO UPDATE
#if SWARM == 1 || ESPRESSO == 1
      int parent_tid = cur_tuple.tid;
      int vid = cur_tuple.dst_id;
      for (int c = 0; c < core_cnt; ++c)
      {
        auto itm = _asic->_asic_cores[c]->_meta_info_queue.find(parent_tid);
        if (itm != _meta_info_queue.end())
        { // found

          if (c != _core_id)
            _asic->_stats->_stat_conf_miss++;
          else
            _asic->_stats->_stat_conf_hit++;

          itm->second.finished_left--;
          if (_asic->_scratch_ctrl->_is_visited[vid])
          { // if already visited
            // cout << "ALREADY _graph_verticesISITED\n";
            itm->second.committed_left--;
#if NETWORK == 1
            _local_bank_queues[i].pop_front();
#else
            _asic->_bank_queues[i].pop_front();
#endif
            return;
          }
          found = true;
          break;
        }
      }

      // if(found) cout << "Found parent tid: " << parent_tid << " in the meta info queue\n";
      // else cout << "Didn't find parent id: " << parent_tid << " in the info queue\n";
      if (!found)
      {
        _asic->_stats->_stat_tot_aborted_edges++;
        // cout << "Couldn't find in meta-infor queue\n";
        continue;
      }
      // CONFLICT SEARCH ACROSS ALL QUEUES (BROADCAST)
      found = false;
      for (int c = 0; c < core_cnt; ++c)
      {
        // auto it = _conflict_queue.find(vid);
        auto it = _asic->_asic_cores[c]->_conflict_queue.find(vid);
        if (it != _asic->_asic_cores[c]->_conflict_queue.end())
        { // found
          // if(it!=_conflict_queue.end()) { // found
          // cout << "Found a conflict with current vid: " << vid << " and core id: " << c << "with current core: " << _core_id << "\n";
          // cout << "Found a conflict with core id: " << c << " with current core: " << _core_id << "\n";
          // TODO: add this later
          // if(c!=_core_id) _asic->_stats->_stat_conf_miss++;
          // else _asic->_stats->_stat_conf_hit++;

          if (it->second > cur_tuple.new_dist)
          {
            // _conflict_queue.erase(vid);
            _asic->_asic_cores[c]->_conflict_queue.erase(vid);
            // TODO: erase all the dependent tasks from meta info queue
            // _asic->_dep_check_depth=0;
            // recursively_erase_dependent_tasks(cur_tuple.dst_id, cur_tuple.tid, it->second, 0); // no recurrence by default
          }
          else
          {
            found = true; // found lower timestamp
            _asic->_stats->_stat_tot_aborted_edges++;
          }
          break;
        }
      }
#endif

      // Push new entry -- New task creation
      if (!found)
      {

#if SWARM == 1 || ESPRESSO == 1
        int new_tid = _asic->_virtual_finished_counter;
        commit_info x(cur_tuple.dst_id, cur_tuple.new_dist, cur_tuple.tid, new_tid);
        auto it = _commit_queue.find(cur_tuple.new_dist);
        if (it == _commit_queue.end())
        { // not found, new timestamp
          queue<commit_info> a;
          a.push(x);
          _commit_queue.insert(make_pair(cur_tuple.new_dist, a));
        }
        else
        {
          it->second.push(x);
        }
#endif

        task_entry new_task(cur_tuple.dst_id, _asic->_offset[cur_tuple.dst_id]);
        new_task.tid = cur_tuple.tid;
        // task_entry new_task(cur_tuple.dst_id,_asic->_offset[cur_tuple.dst_id], _asic->_virtual_finished_counter,0,cur_tuple.tid);

        // not used now
        // new_task.parent_vid = cur_tuple.src_id;
        new_task.dist = cur_tuple.new_dist;

        DTYPE priority_order = _asic->apply(cur_tuple.new_dist, cur_tuple.dst_id);

        // If it is finishing phase, push the new task in working list
        // otherwise push in the task queue
        // TODO: check if it works
        // Oh it choses just 1 edge -- which should have been pushed here.
        // cout << "vid: " << new_task.vid << " priority: " << priority_order << endl;
        // cout << "Dst id: " << new_task.vid << " its scratch: " << _asic->_scratch[new_task.vid] << endl;
        if (_asic->_config->_domain == tree && _asic->is_leaf_node(new_task.vid))
        { // create new task for leaf nodes otherwise normal task
          // Which core? mult core right? (I should make a mult object with a parameter)
          // check if this task has already been created?
          int leaf_id = new_task.vid - pow(2, _asic->kdtree_depth) + 1; // 2^6 = -65*
          assert(leaf_id < _asic->num_kdtree_leaves && "leaf id should not be more than half the vertices");
          assert(leaf_id < _asic->num_kdtree_leaves && "leaf id should not be more than half the vertices");
          // cout << "Creating a data parallel task: " << new_task.vid << " and leaf id: " << leaf_id << " with the updated count: " << _asic->_stats->_stat_tot_data_parallel_tasks << endl;
          // cout << "At core: " << leaf_id%core_cnt << " and cycle: " << _asic->_cur_cycle << endl;
          // cout << "Tasks execution time of tid: " << cur_tuple.tid << " is: " << (_asic->_cur_cycle - _asic->_stats->_start_cycle_per_query[cur_tuple.tid]) << endl;
          _asic->_task_ctrl->insert_local_coarse_grained_task(leaf_id % core_cnt, leaf_id, 0, false);
          if (_asic->_is_leaf_done.test(leaf_id) == 0)
          {
            ++_asic->_unique_leaves;
            // cout << "Leaf id: " << leaf_id << " unique leaves: " << _asic->_unique_leaves << " at cycle: " << _asic->_cur_cycle << endl;
            _asic->_is_leaf_done.set(leaf_id);
          }
        }
        else
        {
          if (_asic->_config->_domain == tree)
          {
            assert(_asic->_scratch[new_task.vid] < _asic->kdtree_depth && "should not go below leaf");
          }
          // cout << "Created a new task for qid: " << new_task.tid  << " and depth: " << _asic->_scratch[new_task.vid] << " in time: " << (_asic->_cur_cycle-_asic->_stats->_start_cycle_per_query[cur_tuple.tid]) << endl;
          // " core: " << _core_id << " cycle: " << _asic->_cur_cycle << endl;
          //  new_task.tid << " with priority: " << priority_order << endl;
          create_async_task(priority_order, new_task);
        }

        _asic->_virtual_finished_counter++;
      }

// PUSH IN CONFLICT QUEUE
#if SWARM == 1 || ESPRESSO == 1
      _conflict_queue.insert(make_pair(cur_tuple.dst_id, cur_tuple.new_dist));
#endif
      // cout << "Insert in conflict queue\n";
      // POP FROM THE BANK QUEUE
      // #if NETWORK == 1
      if (_asic->_config->_network)
      {
        _local_bank_queues[i].pop();
      }
      else
      {
        // if(_asic->_config->_prio_xbar) {
#if PRIO_XBAR == 1
        (it->second).pop_front();
        if ((it->second).empty())
        {
          _asic->_bank_queues[i].erase(it->first);
        }
#else
        // } else {
        _asic->_bank_queues[i].pop_front();
#endif
        // }
      }
    }
  }
  // cout << "Total requests served this cycle: " << total_requests_served << endl;
}

void asic_core::commit()
{ // int lane_id) {
#if CF == 0
  bool found = false;

  for (int c = 0; c < core_cnt; ++c)
  {
    auto it = _asic->_asic_cores[c]->_meta_info_queue.find(_asic->_prev_commit_id);
    if (it != _asic->_asic_cores[c]->_meta_info_queue.end())
    {
      found = true;
      if (it->second.finished_left != 0)
      {
        /*if(_asic->_cur_cycle%10000==0) {
          cout << "Finished left is: " << it->second.finished_left <<" for dst_id: " << _asic->_prev_commit_timestamp << " and prev tid: " << _asic->_prev_commit_id << endl;
        }*/
        return;
      }
    }
  }
  if (!found)
  {
    cout << "Cannot find id in meta info queue: " << _asic->_prev_commit_id << " at core: " << _asic->_prev_commit_core_id << endl;
    exit(0);
    return;
  }

  int gvt = MAX_TIMESTAMP;
  int lowest_cid = -1;
  int cur_lowest_ts = -1;
  for (int c = 0; c < core_cnt; ++c)
  {
    if (_asic->_asic_cores[c]->_commit_queue.empty())
      continue;
    auto it = _asic->_asic_cores[c]->_commit_queue.begin();
    // cout << "Current timestamp: " << it->first << " at core_id: " << _core_id << endl;
    assert(!(it->second).empty());
    cur_lowest_ts = it->first;
    if (cur_lowest_ts < gvt)
    {
      gvt = cur_lowest_ts;
      lowest_cid = c;
    }
  }
  if (lowest_cid == -1)
    return;
  // cout << "LOWEST CID IS: " << lowest_cid << endl;
  // cout << "Is queue empty? " << (_asic->_asic_cores[lowest_cid]->_commit_queue).empty() << endl;
  // Delete commit queue entry
  // _asic->_asic_cores[lowest_cid]->_commit_queue.erase(cur
  // Update in memory
  auto it2 = (_asic->_asic_cores[lowest_cid]->_commit_queue).begin();
  commit_info x = (it2->second).front(); // first element of vector if it is not 0
  // cout << "Going to commit with lowest timestamp: " << gvt << " commit vertex is: " << x.vid << " and elements in queue: " << (it2->second).size() << endl;
  assert(!(it2->second).empty());
  // should erase from both conflict and commit queue
  // Step1: remove first element of queue
  (it2->second).pop();
  // Step2: if the remaining vector is empty, erase entry from commit queue
  if ((it2->second).empty())
  {
    _asic->_asic_cores[lowest_cid]->_commit_queue.erase(gvt);
    // cout << "Deleted timestamp: " << gvt << "entry from commit queue\n";
  }
  // Step3: remove entry from conflict queue
  _asic->_asic_cores[lowest_cid]->_conflict_queue.erase(x.vid);
  //----------------------------------------------------------------
  // FIXME: this might be the case for some reason
  if (_asic->_scratch_ctrl->_is_visited[x.vid])
    return;
  assert(!_asic->_scratch_ctrl->_is_visited[x.vid]);
  _asic->_scratch[x.vid] = it2->first; // x.timestamp;

  // cout << "Going to commit with lowest timestamp: " << gvt << " commit vertex is: " << x.vid << " and elements in queue: " << (it2->second).size() << endl;
  _asic->_stats->_stat_vertices_done++;
  // cout << "COMMIT NEW _graph_verticesERTEX: " << x.vid << " AT SHORTEST DISTANCE: " << it2->first << " vertices done: " << _asic->_stats->_stat_vertices_done << endl;
  // META-INFORMATION UPDATE
  _asic->_prev_commit_core_id = lowest_cid;
  _asic->_prev_commit_id = x.own_tid;
  /*if(_asic->_prev_commit_id==168792) {
    exit(0);
  }*/
  _asic->_prev_commit_timestamp = x.vid; // for debugging purposes
  _asic->_scratch_ctrl->_is_visited[x.vid] = true;
#endif
}

// this is received -- why is src and dst here inversed? (should this src_id be
// still be 1??)
void asic_core::push_scratch_data(pref_tuple cur_tuple)
{
  // cout << "Pushed remote scratch read data to interface: " << cur_tuple.src_id << endl;
  _scratch_process.push(cur_tuple);
}

// TODO: this should basically push
void asic_core::push_scratch_data_to_pipeline()
{
  while (!_scratch_process.empty() && can_push_in_process(0))
  {
    pref_tuple next_tuple = _scratch_process.front();
    // if(next_tuple.src_id==3805) cout << "Pushed remote scratch read data to pipeline: " << next_tuple.src_id << " and dst+id: " << next_tuple.edge.dst_id << endl;
    insert_prefetch_process(0, 0, next_tuple);
    _scratch_process.pop();
  }
}

// FIXME: is it not coming in order?
int asic_core::allocate_cb_entry_for_partitions(bool first_entry, int dfg_id, int index, bool last_entry)
{
  int cb_entry = -1;
  auto rob = _asic->_coarse_reorder_buf[_core_id][dfg_id];
  if (_asic->_config->_algo != gcn)
  {
    rob = _asic->_fine_reorder_buf[_core_id];
  }
  if (first_entry)
  { // if first
    int first = 0;
    first = rob->_free_cb_parts.front().first;
    assert(rob->_free_cb_parts.front().second == false && "cannot assign new entry for already pending free cb parts");
    rob->_free_cb_parts.front().second = true;
  }
  int num_cache_lines_per_weight = 1;
  cb_entry = rob->_free_cb_parts.front().first + index; // (index*num_cache_lines_per_weight) + c;

  /*if(_core_id==12 && index==0) { // index should always increase until last is rceived (if last did not come and index is less, error)
     cout << "Allowing core_id: 12 with cb_entry: " << cb_entry << " at cycle: " << _asic->_cur_cycle << endl;
   }
   if(_core_id==12 && last_entry) {
     cout << "Popping core_id: 12 with cb_entry: " << cb_entry << " start: " << rob->_free_cb_parts.front().first << " at cycle: " << _asic->_cur_cycle << endl;
   }*/
  assert(rob->_free_cb_parts.front().second == true && "should be the pending one");
  cb_entry = cb_entry % COMPLETION_BUFFER_SIZE;

  if (last_entry)
  {
    rob->_free_cb_parts.pop(); // nobody else should use it...
    // set all others cb entries in the partition to be valid or mark!!
    rob->_reorder_buf[cb_entry].last_entry = true;
  }
  if (first_entry & last_entry == 0)
  {
    assert(index == 0 && "both should not be true in a single cycle");
  }

  assert(rob->_reorder_buf[cb_entry].waiting_addr == -1 && "cb should have default waiting addr");
  assert(!rob->_reorder_buf[cb_entry].valid && "cb should be free while allocating");
  rob->_reorder_buf[cb_entry].waiting_addr = 1;
  rob->_entries_remaining--;
  return cb_entry;
}

void asic_core::pull_scratch_read()
{
  // TODO: put a condition to check whether free cb partitions are present (for
  // idealized experiment, should be infinite)
  int elem_done = 0;
  bool cb_entries_avail = _asic->_fine_reorder_buf[_core_id]->can_push();
  if (_asic->_fine_reorder_buf[_core_id]->_cb_start_partitions[0].size() > 0)
  {
    cb_entries_avail = _asic->_fine_reorder_buf[_core_id]->_free_cb_parts.size() > 0;
  }
  while (elem_done < _fine_grain_throughput && !_prefetch_scratch.empty() && cb_entries_avail && _asic->_network->can_push_in_network(_core_id))
  { // FIXME: it should be according to required packets..
    pref_tuple cur_tuple = _prefetch_scratch.front();
    _prefetch_scratch.pop();
    ++elem_done;
    // cur_tuple.new_dist=-1;
    if (!_asic->_config->is_vector())
    {
      // cur_tuple.cb_entry = _asic->_fine_reorder_buf[_core_id]->allocate_cb_entry(1);
      cur_tuple.req_core = _core_id;
      cur_tuple.core_type = fineScratch;

      if (_asic->_config->_inter_task_reorder == 0)
      {
        cur_tuple.cb_entry = _asic->_fine_reorder_buf[_core_id]->allocate_cb_entry(1);
      }
      else
      {
        int in_degree = (_asic->_offset[cur_tuple.src_id + 1] - _asic->_offset[cur_tuple.src_id]);
        // cout << "Incoming data for  dst_id, src_id: " << cur_tuple.src_id << " and edge dst_id: " << cur_tuple.edge.dst_id << " in_degree: " << in_degree << " core: " << _core_id << " cycle: " << _asic->_cur_cycle << " edges_to_read: " << _edges_read_sent << endl;
        bool first = true;
        if (cur_tuple.src_id == _last_requested_src_id)
        { // same vid again?
          first = false;
        }
        else
        {
          // these updates are lost here
          // assert(_last_requested_src_id==-1 && "we set it during last entry"); // 81 is not coming in sequence?? (pref_lsq?)
          _last_requested_src_id = cur_tuple.src_id;
          _edges_read_sent = 0;
        }
        ++_edges_read_sent;
        assert(_edges_read_sent < MAX_DEGREE && "cannot allocated more entries than available");

        // cur_tuple.last = (_edges_read_sent==in_degree);
        // if(cur_tuple.last) _last_requested_src_id=-1;
        cur_tuple.last = first;
        if (_edges_read_sent == in_degree)
          _last_requested_src_id = -1;
        cur_tuple.cb_entry = -1;
      }
      _asic->_scratch_ctrl->send_scratch_request(_core_id, cur_tuple.edge.dst_id, cur_tuple);
    }
    else
    {
      cur_tuple.req_core = _core_id;
      cur_tuple.core_type = fineScratch;
      // ordered currently
      bool last = false;
      ++_current_edges;
      int edges_to_accumulate = _asic->_offset[cur_tuple.src_id + 1] - _asic->_offset[cur_tuple.src_id];
      // cout << "Src_id: " << cur_tuple.src_id << " current edges: " << _current_edges << " edges_to_accumulate: " << edges_to_accumulate << endl;
      if (_current_edges == edges_to_accumulate)
      {
        _current_edges = 0;
        last = true;
      }
      for (int i = 0; i < num_packets; ++i)
      { // address -- assume same core
        cur_tuple.cb_entry = _asic->_fine_reorder_buf[_core_id]->allocate_cb_entry(1);
        if (i == num_packets - 1)
        {
          cur_tuple.last = last;
        }
        else
        {
          cur_tuple.last = false;
        }
        cur_tuple.end_edge_served = i;
        // cout << "Sending scratch read request at core: " << _core_id << " dst/incoming vertex: " << cur_tuple.edge.dst_id << " src: " << cur_tuple.src_id << endl;
        _asic->_scratch_ctrl->send_scratch_request(_core_id, cur_tuple.edge.dst_id, cur_tuple);
      }
    }
    cb_entries_avail = _asic->_fine_reorder_buf[_core_id]->can_push();
    if (_asic->_fine_reorder_buf[_core_id]->_cb_start_partitions[0].size() > 0)
    {
      cb_entries_avail = _asic->_fine_reorder_buf[_core_id]->_free_cb_parts.size() > 0;
    }
  }

  // TODO: check which of the things is not setting up here...or better
  // calculate cycles when this core could not send scratchpad requests
  if (elem_done == 0)
  {
    if (_prefetch_scratch.empty())
    {
      _cycles_mem_edge_empty++;
    }
    else if (_asic->_fine_reorder_buf[_core_id]->can_push())
    {
      _cycles_reorder_full++;
    }
    else if (_asic->_network->can_push_in_network(_core_id))
    {
      _cycles_net_full++;
    }
  }
}

// processing (put application specific things here)
// current throughput according to scratch b/w edge per core per cycle
// compute throughout = 1 edge per cycle
void asic_core::process(int lane_id)
{
  red_tuple next_tuple;
  pref_tuple cur_tuple;
  int elem_done = 0;

  while (elem_done < _fine_grain_throughput && !_prefetch_process[lane_id].empty() && can_push_in_reduce(lane_id))
  {

    // ++_asic->_stats->_stat_finished_edges_per_core[_core_id];
    auto it = _prefetch_process[lane_id].begin();
    cur_tuple = (it->second).front();

    // cout << "Core: " << _core_id << " dispatching a task with vid: " << cur_tuple.src_id << " cycle: " << _asic->_cur_cycle << endl;
    // cur_tuple.src_id = cur_tuple.edge.src_id;
    // cur_tuple = _prefetch_process[lane_id].front();
    if (_break_this_cycle && cur_tuple.src_id != _last_break_id)
    {
      return;
    }

    // cycle level breakdown
    if (_asic->_scratch[cur_tuple.src_id] == _asic->_correct_vertex_data[cur_tuple.src_id])
    {
      _asic->_stats->_stat_correct_edges[_core_id]++;
    }
    else
    {
      _asic->_stats->_stat_incorrect_edges[_core_id]++;
    }

    if (_asic->_compl_buf[_core_id]->_entries_remaining > 0)
    {
      _asic->_stats->_stat_mem_stall_thr[_core_id]--;
    }

    // next_task.edge_id = cur_tuple.edge_id;
    // next_task.lane_id = cur_tuple.lane_id;

    DTYPE first_input = cur_tuple.edge.wgt;
    if (_asic->_config->_algo == pr)
    {
      assert(cur_tuple.src_id >= 0 && cur_tuple.src_id < V);
      int degree = _asic->_offset[cur_tuple.src_id + 1] - _asic->_offset[cur_tuple.src_id];
      assert(degree > 0);
      first_input = (float)1 / (float)degree;
      // assert(next_tuple.new_dist>=0);
    }

    // TODO: vidushi: use second input from information passed in the task for pr
    DTYPE second_input = cur_tuple.src_dist;
    if (_asic->_config->_algo != pr && _asic->_config->is_async() && !_asic->_switched_sync && _asic->_config->_domain != tree)
    {
      second_input = _asic->_scratch[cur_tuple.src_id];
    }

    // cout << "src: " << cur_tuple.src_id << " edge: " << cur_tuple.edge.wgt << " src_idst: " << second_input << endl;

    if (!_asic->_config->_pull_mode)
    {
      next_tuple.new_dist = _asic->process(first_input, second_input);
    }

    next_tuple.dst_id = cur_tuple.edge.dst_id;
    next_tuple.src_id = cur_tuple.src_id;
    next_tuple.tid = cur_tuple.tid;

    // _prefetch_process[lane_id].pop_front();
    (it->second).pop_front();
    if ((it->second).empty())
    {
      _prefetch_process[lane_id].erase(it->first);
    }
    elem_done++;

    // cout << "Core: " << _core_id << " src: " << cur_tuple.src_id << " cycle: " << _asic->_cur_cycle << " last break id: " << _last_break_id <<  endl;
    int edges_to_accumulate = (_asic->_offset[cur_tuple.src_id + 1] - _asic->_offset[cur_tuple.src_id]);

    // Another task corresponding to new break should not continue
    if (cur_tuple.src_id == _last_break_id)
    {
      continue;
    }
    else
    {
      _last_break_id = -1;
    }

#if INTERTASK_REORDER == 0
    // HACK
    if (cur_tuple.src_id != _last_processed_id && _current_edges != 0)
    {
      red_tuple prev_tuple;
      prev_tuple.src_id = _last_processed_id;
      _process_reduce[lane_id].push(prev_tuple);
      int last_degree = _asic->_offset[_last_processed_id + 1] - _asic->_offset[_last_processed_id];
      _asic->_stat_extra_edges_during_pull_hack += (last_degree - _current_edges);
      _current_edges = 0;
    }

    ++_current_edges;
    _last_processed_id = next_tuple.src_id;
    assert(_current_edges <= edges_to_accumulate && "current edges should always be less than total edges");
#endif

    // FIXME: is it missed because its in-degree is 1??
    // TODO: if this is last, then push. No need to current edges.
    /*if(_core_id==0) {
      cout << "src: " << cur_tuple.src_id << " degree: " << (_asic->_offset[cur_tuple.src_id+1]-_asic->_offset[cur_tuple.src_id]) << " cycle: " << _asic->_cur_cycle << " last:  " << cur_tuple.last << endl;
    }*/
#if INTERTASK_REORDER == 0
    if (!_asic->_config->_pull_mode || (_asic->_config->_pull_mode && _current_edges == edges_to_accumulate))
    {
#else
    if (!_asic->_config->_pull_mode || (_asic->_config->_pull_mode && cur_tuple.last == true))
    {
#endif
      if (0)
      { // _asic->_config->_pull_mode) {
        // _asic->_scratch[cur_tuple.src_id] = _asic->reduce(_asic->_scratch[cur_tuple.src_id], _asic->process(cur_tuple.edge.wgt, _asic->_vertex_data[cur_tuple.edge.dst_id]));
      }
      else
      {
        _process_reduce[lane_id].push(next_tuple);
      }
      _current_edges = 0;
    }
    else
    {
      // #if INTERTASK_REORDER==0
      int edge_id = _asic->_offset[cur_tuple.src_id] + _current_edges;
      // cout << "Current source: " << cur_tuple.src_id << " edges: " << _current_edges << endl;
      if (_asic->should_break_in_pull(_asic->_neighbor[edge_id].dst_id))
      {
        trigger_break_operations(next_tuple.src_id);
        _last_break_id = next_tuple.src_id;
        // there was no break at current edges is 0
        // cout << "Core: " << _core_id << " break at: " << _last_break_id << " at cycle: " << _asic->_cur_cycle << endl;
        _process_reduce[lane_id].push(next_tuple);
        _asic->_stats->_stat_edges_dropped_with_break += (edges_to_accumulate - _current_edges);
        _current_edges = 0; // from now ignore all edges of this src_id
        _break_this_cycle = true;
      }
      // #endif
    }
  }
  // cout << "In process, elem done at cycle: " << _asic->_cur_cycle << " is: " << elem_done << endl;

  if (_asic->_compl_buf[_core_id]->_entries_remaining < COMPLETION_BUFFER_SIZE)
  {
    _asic->_stats->_stat_mem_stall_thr[_core_id]++;
  }
}

// Note: current throughput = 1 element communicate per core per cycle
// Send a network remote request
void asic_core::reduce(int lane_id)
{

  if ((_asic->_config->_net != crossbar && _asic->_config->_net == hrc_xbar) && _asic->_cur_cycle % _asic->_xbar_bw == 0)
    return;
  red_tuple cur_tuple;
  int elem_done = 0;

  assert(_active_process_thr <= _fine_grain_throughput);

  net_packet net_tuple;
  if (_asic->_config->_net_traffic == real_multicast)
  {
    assert(_asic->_config->_net == mesh && "multicast only possible for mesh");
    assert(_asic->_config->_algo != sssp && "multicast not allowed for shortest path");
    vector<iPair> dest;
    DTYPE x[FEAT_LEN];
    // FIXME: should this be a gcn packet???
    net_tuple = _asic->_network->create_vector_update_packet(-1, -1, dest, x);
  }

  while (elem_done < _active_process_thr && !_process_reduce[lane_id].empty() && _asic->_network->can_push_in_network(_core_id) && _asic->can_push_to_bank_crossbar_buffer())
  {
    if (_asic->_config->_working_cache)
    {
      if (_asic->_mem_ctrl->_num_pending_cache_reqs >= (MAX_CACHE_MISS_ALLOWED - num_banks))
        break;
      if (_asic->_stats->_stat_pending_colaesces >= (MAX_COALESCES_ALLOWED - num_banks))
        break;
    }
    cur_tuple = _process_reduce[lane_id].front();
    // cout << "dst_id: " << cur_tuple.dst_id << " dist: " << _asic->_scratch[cur_tuple.dst_id] << endl;
    _process_reduce[lane_id].pop();
    elem_done++;
    if (_asic->_config->_pull_mode)
    {
      int src_to_reduce = cur_tuple.src_id;
      DTYPE old_dist = _asic->_scratch[cur_tuple.src_id];
      if (_asic->_config->is_sync())
      {
        if (_asic->_config->_entry_abort)
        {
          _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_tuple.src_id);
        }
        // TODO: use only the not broken edges to ensure that it is correct..
        // TODO: We are reading input data from scratch and writing to memory right? (which data-structure is holding the correct data?)
        /*if(cur_tuple.src_id==789187) {
            cout << "BREAK, it comes here\n";
          }*/
        // FIXME: this is too much..!!! let me just not push and copy this..
        /*if(_core_id==0) {
            cout << "Serving for src_id: " << cur_tuple.src_id << " for degree: " << (_asic->_offset[cur_tuple.src_id+1]-_asic->_offset[cur_tuple.src_id]) << " at cycle: " << _asic->_cur_cycle << endl;
          }*/
        /*if(src_to_reduce==820735 || src_to_reduce==820737 || (src_to_reduce >= 820755 && src_to_reduce <= 820775)) {
            cout << "Src_to_reduce: " << src_to_reduce << " Old distance: " << _asic->_scratch[src_to_reduce] << " at cycle: " << _asic->_cur_cycle << endl;
          }*/
        if (_asic->_config->_algo == pr)
        {
          _asic->_scratch[src_to_reduce] = (1 - alpha);
        }
        for (int e = _asic->_offset[cur_tuple.src_id]; e < _asic->_offset[cur_tuple.src_id + 1]; ++e)
        {
          ++_asic->_stats->_stat_finished_edges_per_core[_core_id];
          int dst_from_mem = _asic->_neighbor[e].dst_id;
          DTYPE first_input = _asic->_neighbor[e].wgt;
          if (_asic->_config->_algo == pr)
          {
            int degree = _asic->_csr_offset[dst_from_mem + 1] - _asic->_csr_offset[dst_from_mem];
            assert(degree > 0);
            first_input = (float)1 / (float)degree;
          }
          // cout << "old scratch: " << _asic->_scratch[src_to_reduce] << " first input: " << first_input << " vertex data: " << _asic->_vertex_data[dst_from_mem] << endl;
          _asic->_scratch[src_to_reduce] = _asic->reduce(_asic->_scratch[src_to_reduce], _asic->process(first_input, _asic->_vertex_data[dst_from_mem]));
          ++_asic->_stats->_stat_tot_finished_edges;
          // cout << "Accumulating src_id: " << src_to_reduce << " updated distanced: " << _asic->_scratch[src_to_reduce] << endl;
        }
        /*if(src_to_reduce==820735 || (src_to_reduce >= 820755 && src_to_reduce <= 820775)) {
            cout << "Updated distance: " << _asic->_scratch[src_to_reduce] << endl;
          }*/
        // cout << "Accumulated src_id: " << cur_tuple.src_id << endl;

        // this should have been fixed to 6 and it should have activated its
        // outgoing vertices

        /*if(_asic->_scratch[src_to_reduce]>=MAX_TIMESTAMP) {
              cout << "src:  " << src_to_reduce << " in-degree: " << _asic->_offset[cur_tuple.src_id] << endl;
          }*/
        // assert(_asic->_scratch[src_to_reduce]<MAX_TIMESTAMP && "a vertex should not be activated unless any incoming neighbor is updated");
      }
      else
      { // Diff1: reduce using scratch data, Diff2: if updated, insert new task
        if (_asic->_config->_entry_abort)
        {
          _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_tuple.src_id);
        }
        if (_asic->_config->_algo == pr)
        {
          _asic->_scratch[src_to_reduce] = (1 - alpha);
        }
        for (int e = _asic->_offset[cur_tuple.src_id]; e < _asic->_offset[cur_tuple.src_id + 1]; ++e)
        {
          int dst_from_mem = _asic->_neighbor[e].dst_id;
          DTYPE first_input = _asic->_neighbor[e].wgt;
          if (_asic->_config->_algo == pr)
          {
            int degree = _asic->_csr_offset[dst_from_mem + 1] - _asic->_csr_offset[dst_from_mem];
            assert(degree > 0);
            first_input = (float)1 / (float)degree;
          }
          // cout << "old scratch: " << _asic->_scratch[src_to_reduce] << " first input: " << first_input << endl;
          _asic->_scratch[src_to_reduce] = _asic->reduce(_asic->_scratch[src_to_reduce], _asic->process(first_input, _asic->_scratch[dst_from_mem]));
          ++_asic->_stats->_stat_tot_finished_edges;
        }
        bool should_spawn = _asic->should_spawn_task(old_dist, _asic->_scratch[src_to_reduce]);
        if (should_spawn)
        {
          task_entry new_task(src_to_reduce, _asic->_offset[src_to_reduce]);
          assert(_asic->_scratch[src_to_reduce] < MAX_TIMESTAMP && "a vertex should not be activated unless any incoming neighbor is updated");
          _asic->_task_ctrl->insert_new_task(0, 0, _asic->_scratch_ctrl->get_local_scratch_id(new_task.vid), _asic->_scratch[src_to_reduce], new_task);
          // push_live_task(_asic->_scratch[src_to_reduce], new_task);
        }
      }

      if (old_dist != _asic->_scratch[cur_tuple.src_id] && _asic->_scratch[cur_tuple.src_id] == _asic->_correct_vertex_data[cur_tuple.src_id])
      {
        _asic->_tot_useful_edges_per_iteration++;
      }

      continue;
    }

    int bank_id = _asic->_scratch_ctrl->get_local_bank_id(cur_tuple.dst_id); // %num_banks; // same as graphicionado
    if (_asic->_config->_net == crossbar)
      bank_id = _asic->_scratch_ctrl->get_global_bank_id(cur_tuple.dst_id);
    bool send_over_net = true;

    // TODO: should i move dest core_id calc here?
#if SWARM == 1 || TESSERACT == 1
    // TODO: do this only if it is a compulsory miss
    // int cache_line_id = _asic->get_cache_line_id(cur_tuple.dst_id);
    int cache_line_id = cur_tuple.dst_id / 16;
    _asic->_stats->_stat_num_accesses++;
    // if(!_is_hot_miss[cache_line_id]) { // if compulsory miss
    if (0)
    { // if compulsory miss
      // if(true) { // if compulsory miss
      // cout << "Compulsory miss pushed\n";
      _asic->_stats->_stat_num_compulsory_misses++;
      // _in_order_flag[lane_id]=false;
      _active_process_thr--;

      cur_tuple.req_core_id = _core_id;
      cur_tuple.lane_id = lane_id;
      cur_tuple.dest_core_id = _asic->get_mc_core_id(cur_tuple.dst_id);
      _is_hot_miss[cache_line_id] = true;
    }
    else
    { // hit in the cache: no need to go over the network
      // cout << "Hit pushed in the banks\n";
      _asic->_network->_stat_local_updates++;
      cur_tuple.lane_id = lane_id;
      // cout << "pushing dst_id: " << cur_tuple.dst_id << " at lane: " << lane_id << " of core: " << _core_id << endl;

      // cur_tuple.src_id << " new dist: " << cur_tuple.new_dist << endl;
#if CF == 1
      assert(0 && "cf is not supposed to push in bank queues");
#endif
      // _asic->graphmat_slice_configs(cur_tuple); // swarm
      _asic->execute_func(_core_id, BANK_TO_XBAR, cur_tuple);
      send_over_net = false;
    }
#endif

    int dst_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id);
    // Currently if destination core is same, it should always send to the
    // same place
    if (dst_core == _core_id || _asic->_config->_perfect_net)
    {
      // cout << "cycle: " << _asic->_cur_cycle << " cur core: " << _core_id << " net update to the same core, vid: " << cur_tuple.dst_id << endl;
      // cout << "Core: " << _core_id << " executing an update with vid: " << cur_tuple.dst_id << " cycle: " << _asic->_cur_cycle << endl;
      _asic->execute_func(_core_id, BANK_TO_XBAR, cur_tuple);
      send_over_net = false;
    }
    if (send_over_net)
    {
      // cout << "cycle: " << _asic->_cur_cycle << " cur core: " << _core_id << " net update to the other core, vid: " << cur_tuple.dst_id << endl;
      if (_asic->_config->_net == mesh)
      {
        if (_asic->_config->_net_traffic == real_multicast)
        {
          // int max_packets_on_bus = bus_width/message_size;
          bool new_packet = ((net_tuple.src_id != cur_tuple.src_id) && (net_tuple.new_dist != cur_tuple.new_dist)) || (net_tuple.multicast_dst_wgt.size() >= _asic->_network->_bus_vec_width); // 16 packets allowed max
          if (new_packet)
          { // send the collected ones
            if (net_tuple.multicast_dst_core_id.size() > 0)
            {
              // cout << "Sending new packet at source: " << _core_id << " with size: " << net_tuple.multicast_dst_core_id.size() << endl;
              _asic->_network->push_net_packet(_core_id, net_tuple);
            }
            net_tuple.src_id = cur_tuple.src_id;
            net_tuple.new_dist = cur_tuple.new_dist;
            net_tuple.tid = cur_tuple.tid;
            net_tuple.multicast_dst_wgt.clear();
            net_tuple.multicast_dst_core_id.clear();
          }
          // TODO: @vidushi: this wgt should not be used anywhere else
          net_tuple.multicast_dst_wgt.push_back(make_pair(cur_tuple.dst_id, 0));
          net_tuple.multicast_dst_core_id.push_back(dst_core);
        }
        else
        {
          net_packet scalar_update = _asic->_network->create_noopt_scalar_update_packet(cur_tuple.src_id, cur_tuple.dst_id, cur_tuple.new_dist);
          _asic->_network->push_net_packet(_core_id, scalar_update);
          // cur_tuple.dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id);
          // _asic->_network->push_net_packet(_core_id, cur_tuple);
        }
      }
      else
      { // crossbar with a cache
        _asic->execute_func(_core_id, BANK_TO_XBAR, cur_tuple);
      }
    }
  }
  // cout << " Num elem in reduce: " << elem_done << " at cycle: " << _asic->_cur_cycle << endl;
  if (_asic->_config->_net_traffic == real_multicast && net_tuple.multicast_dst_core_id.size() > 0)
  {
    _asic->_network->push_net_packet(_core_id, net_tuple);
  }
}

void asic_core::serve_atomic_requests()
{
#if PRIO_XBAR == 0
  red_tuple cur_tuple;
  // execute data from banks
  for (int i = 0; i < num_banks; ++i)
  {
#if NETWORK == 1
    if (!_local_bank_queues[i].empty())
    {
      cur_tuple = _local_bank_queues[i].front();
#else
    if (!_asic->_bank_queues[i].empty())
    {
      cur_tuple = _asic->_bank_queues[i].front();
#endif
      _asic->_gcn_updates[i]++;

#if NETWORK == 1 && ASTAR == 1
      // cout << "COME IN ASTART REGION\n";
      if (cur_tuple.req_core_id != -1)
      { // read request
        cur_tuple.dest_core_id = cur_tuple.req_core_id;
        cur_tuple.req_core_id = -1;
        _asic->_network->push_net_packet(_core_id, cur_tuple);
        _local_bank_queues[i].pop();
        // _local_bank_queues[i].pop_back();
        continue;
      }

#endif
      // cout << "Old distance: " << _asic->_scratch[cur_tuple.dst_id] << endl;
      _asic->_stats->_stat_num_req_per_bank[i]++;
      _asic->_stats->_stat_tot_finished_edges++;
      // FIXME: Is it not required for Expresso as well?
#if GRAPHLAB == 1
      // cout << "COme inside bank queue condition\n";
      _asic->_atomic_issue_cycle[cur_tuple.dst_id] = _asic->_cur_cycle;
      // TODO: put this according to reduce condition
      if (cur_tuple.new_dist >= _asic->_scratch[cur_tuple.dst_id])
      {
        _asic->_update[cur_tuple.dst_id] = false;
      }
      else
      {
        // _asic->_is_visited[cur_tuple.dst_id] = true;
        // cout << "NEW PARENT COMMITTED: " << cur_tuple.dst_id << endl;
        // cout << _asic->_offset[cur_tuple.dst_id] << " " << _asic->_offset[cur_tuple.dst_id+1] << endl;
        // push new tasks into the global task queue
        task_entry new_task(cur_tuple.dst_id, _asic->_offset[cur_tuple.dst_id]);
        _asic->_task_ctrl->insert_new_task(0, new_task);
      }
#endif
#if DIJKSTRA == 1
      // cout << "FINISHED TASK, src_id: " << cur_tuple.src_id << " dest_id: " << cur_tuple.dst_id << " at the old timestamp: " << cur_tuple.tid << " new timestamp: " << cur_tuple.new_dist << endl;
      // reduce_finished(cur_tuple.tid); // has to maintain meta_info queue here also
      // TODO: might need to move this code to common function
      int parent_tid = cur_tuple.tid;
      for (int c = 0; c < core_cnt; ++c)
      {
        auto itm = _asic->_asic_cores[c]->_meta_info_queue.find(parent_tid);
        if (itm != _meta_info_queue.end())
        { // found
          itm->second.finished_left--;
        }
      }

      assert(_asic->_config->_domain == graphs && "for tree, currently we do not support multiple copies of scratch update");
      // FIXME: find a neater way to do this
      if (_asic->reduce(cur_tuple.new_dist, _asic->_scratch[cur_tuple.dst_id]) == cur_tuple.new_dist)
      {
        // push this in commit queue
        commit_info x(cur_tuple.dst_id, cur_tuple.new_dist, cur_tuple.tid, _asic->_virtual_finished_counter);
        auto it = _commit_queue.find(cur_tuple.new_dist);
        if (it == _commit_queue.end())
        { // not found, new timestamp
          queue<commit_info> a;
          a.push(x);
          _commit_queue.insert(make_pair(cur_tuple.new_dist, a));
        }
        else
        {
          it->second.push(x);
        }
        // _asic->_virtual_finished_counter++;
      }
#else
      // non-speculative update, this is our main focus
      int dest_slice_id = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id);
      // cout << "in bank queues, served\n";
      if (_asic->_config->_exec_model == sync_slicing && _asic->_config->_reuse > 1 && !_asic->_finishing_phase && dest_slice_id != _asic->_current_slice)
      {
        _asic->_delta_updates[_asic->_current_reuse].push_back(make_pair(cur_tuple.dst_id, cur_tuple.new_dist));
        _asic->_stats->_stat_tot_num_copy_updates++;
      }
      else
      {
        DTYPE old_dist = _asic->_scratch[cur_tuple.dst_id];
        _asic->_scratch[cur_tuple.dst_id] = _asic->reduce(_asic->_scratch[cur_tuple.dst_id], cur_tuple.new_dist);

        if (old_dist != _asic->_scratch[cur_tuple.dst_id] && _asic->_scratch[cur_tuple.dst_id] == _asic->_correct_vertex_data[cur_tuple.dst_id])
        {
          _asic->_tot_useful_edges_per_iteration++;
        }

        if (_asic->_config->_algo == pr)
        {
          assert(_asic->_scratch[cur_tuple.dst_id] >= 0 && "after reduce, scratch should be good");
        }

        if (_asic->_task_ctrl->_present_in_queue.test(cur_tuple.dst_id) == 0)
        {
          _asic->_unique_vertices++;
        }
        _asic->_task_ctrl->_present_in_queue.set(cur_tuple.dst_id);
      }
#endif
      if (_asic->_config->_net == mesh)
      {
        _local_bank_queues[i].pop();
      }
      else
      {
        _asic->_bank_queues[i].pop_front();
      }
    }
  }
#endif
}

// local task queue -> process
// TODO: internal load balancing, prefetch latency
// max 1 vertex per cycle, also only 1 core should get this chance
// prioritizing lower timestamp

// new task was a higher priority, and got pushed before this pending task..
// ok may be i should then update the pending task priority to highest...
void asic_core::dispatch(int tqid, int lane_id)
{

  _busy_cycle++;
  int i = 0, src_id = 0;
  pref_tuple next_tuple;

  int num_pops = 0;
  int num_cache_access = 0;
  assert(!_task_queue[tqid][lane_id].empty() || _pending_task[tqid].vid != -1);

  int num_task_queue_pops = 0;
  int local_elem_disp = 0;
  int max_task_queue_pops = 1;
  int total_edges_requested = 0;
  int max_local_elem_dispatch = max(process_thr, _fine_grain_throughput);
  if (_asic->_config->_prac == 0)
  {
    max_task_queue_pops = _fine_grain_throughput;
    max_local_elem_dispatch = _fine_grain_throughput;
  }

  while (num_task_queue_pops < max_task_queue_pops && local_elem_disp < max_local_elem_dispatch && _asic->_elem_dispatch < _asic->_max_elem_dispatch_per_core && (!_task_queue[tqid][lane_id].empty() || _pending_task[tqid].vid != -1) && num_pops < _fine_grain_throughput && num_cache_access < _max_cache_bw && _asic->_compl_buf[_core_id]->_entries_remaining > 2)
  { // TODO: check for dispatch condition here as well!!

    // Dequeue logic
    task_entry cur_task;
    auto it = _task_queue[tqid][lane_id].begin();
    if (_pending_task[tqid].vid != -1)
    {
      cur_task = _pending_task[tqid];
    }
    else
    {
      // cout << "Total tasks in orignal: " << _task_queue[0][0].begin()->second.size() << endl;
      cur_task = (it->second).front();
    }
    // cout << "Core: " << _core_id << " dispatching a task with vid: " << cur_task.vid << " cycle: " << _asic->_cur_cycle << endl;
    if (_asic->_config->_algo == pr)
    {
      assert(cur_task.dist > 0 && "pr residual should ideally be greater than epsilon for pushing");
      if (_asic->_config->_blocked_async && _asic->_scratch[cur_task.vid] != 0)
      {
        cur_task.dist = _asic->_scratch[cur_task.vid];
        _asic->_scratch[cur_task.vid] = 0;
      }
    }
    /*if(_core_id==3) {
       cout << "At cycle: " << _asic->_cur_cycle << " issued a task: " << cur_task.vid << " with start offset: " << cur_task.start_offset << " end offset: " << _asic->_offset[cur_task.vid+1] << endl;
     }*/
    if (_asic->_config->_domain == tree && cur_task.dist == 0)
    {
      _asic->_stats->_start_cycle_per_query[cur_task.tid] = _asic->_cur_cycle;
    }
    else
    {
      // cout << "Issuing a new task for qid: " << cur_task.tid  << " in time: " << (_asic->_cur_cycle-_asic->_stats->_start_cycle_per_query[cur_task.tid]) << endl;
    }
    // Probably latency is too high..
    // FIXME: how do I maintain variable distances for each vertex of the tree??? should use distance stored in it??
    // cout << "Issuing vid: " << cur_task.vid << " with distance: " << _asic->_scratch[cur_task.vid]
    // << " tid: " << cur_task.tid << " at cycle: " << _asic->_cur_cycle << " core: " << _core_id << endl;
    // cout << "kdtree depth: " << _asic->kdtree_depth << " leaves: " << _asic->num_kdtree_leaves << endl;
    // cout << "Cycle: " << _asic->_cur_cycle << " vid: " << cur_task.vid << endl;
    // cout << "Current priority: " << it->first << " and vertex: " << cur_task.vid << " and start offset: " << cur_task.start_offset << endl;
    // cout << "core: " << _core_id << " TQID: " << tqid << " Serving task for vertex: " << cur_task.vid << " and start offset: " << cur_task.start_offset << " cycle: " << _asic->_cur_cycle <<  endl;

    if (cur_task.start_offset == _asic->_offset[cur_task.vid])
    {
      _asic->_stats->_stat_dist_tasks_dequed[_core_id]++;
    }
    if (_asic->_config->_entry_abort)
    {
      if (!_asic->_config->_pull_mode || _asic->_config->is_sync())
      {
        _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_task.vid);
      }
    }
    // if(new_task.vid==11348) {
    // cout << "Cycle: " << _asic->_cur_cycle << " task: " << cur_task.vid << " start offset: " << cur_task.start_offset << " priority: " << it->first <<  endl;
    /*if(_core_id==4) {
    }*/
    // set_dist_at_dequeue();

    if (_asic->_config->_prac == 1 || _asic->_config->_swarm == 1)
    {
      // _local_min_dist = it->first;
      num_task_queue_pops++;
    }
    // we allow fine_grain_throughput number of pops for prac=0?
    if (_asic->_config->_prac == 0)
    {
      num_task_queue_pops++;
    }
    /*if(!_asic->_config->no_swarm()) {
      next_tuple.tid = cur_task.tid;
    }*/
    src_id = cur_task.vid; // send for prefetch

    set_src_dist(cur_task, next_tuple);

    next_tuple.tid = cur_task.tid;
    next_tuple.src_id = src_id;
    next_tuple.second_buffer = cur_task.second_buffer;
    i = cur_task.start_offset;
    int min_cb = 1;
    int edges_served = 0;
    if (_asic->_config->is_sync())
      min_cb = 2;
    assert(cur_task.start_offset <= _asic->_offset[src_id + 1] && "cannot start afte end of edge list");
    for (i = cur_task.start_offset; _asic->_elem_dispatch < _asic->_max_elem_dispatch_per_core && local_elem_disp < max_local_elem_dispatch && i < _asic->_offset[src_id + 1] && num_pops < _fine_grain_throughput && num_cache_access < _max_cache_bw && !_swarm_pending_request[lane_id] && _in_order_flag[lane_id] && _asic->_compl_buf[_core_id]->_entries_remaining > min_cb; ++i)
    {

      ++total_edges_requested;

      if (_asic->_config->_reuse_stats)
      {
        _asic->_stats->_stat_edge_freq_per_itn[i]++;
      }

      // TODO: Why setting cur_task.dist? (this is being modified after task
      // is issued from task queue? this is kind of duplicate when reading
      // from memory)
      set_dist_at_dequeue(cur_task, next_tuple);
      set_src_dist(cur_task, next_tuple); // TODO: for sequential consistency??
      // this is for graphsage...
      // #if GCN==1 && LADIES_GCN==0
      //         if(i>=_asic->_offset[src_id] + SAMPLE_SIZE) {
      //           cur_task.start_offset = _asic->_offset[src_id+1];
      //           i = cur_task.start_offset;
      //           break;
      //         }
      // #endif

      num_pops++;
      // next_tuple.edge_id = i;
      next_tuple.edge = _asic->_neighbor[i]; // assuming prefetch latency=0
      if (_asic->_config->_dramsim_enabled == 1)
      {
        if (_asic->_config->_domain == tree)
        {

          int degree = _asic->_offset[cur_task.vid + 1] - _asic->_offset[cur_task.vid];
          // i = _asic->_offset[cur_task.vid] + rand()%degree; // TODO: this random should be skewed
          // default_random_engine generator;
          // normal_distribution<double> distribution(0.5,0.5); // mean, variance
          int depth = _asic->_scratch[cur_task.vid] + 1;
          // TODO: should be the query_id*depth
          // srand(_asic->_stats->_stat_tot_finished_edges);
          int seed = cur_task.tid * depth;
          srand(cur_task.tid * depth);
          // TODO: this will need to traversed throughput whole pipeline (normal random should be easier!!)
          // cout << "Created qid: " << cur_task.tid << " depth: " << depth << " random seed: " << seed
          // << " cycle: " << _asic->_cur_cycle << " core: " << _core_id << endl;

          // ++_asic->_stats->_stat_tot_data_parallel_tasks;
          int value = rand();                                // distribution(generator);
          i = _asic->_offset[cur_task.vid] + value % degree; // TODO: this random should be skewed
          edges_served = access_edge(cur_task, i, next_tuple);
          // cur_task.start_offset = _asic->_offset[cur_task.vid+1];
          cur_task.start_offset = _asic->_offset[cur_task.vid] + edges_served;
          i = cur_task.start_offset;
          _asic->_elem_dispatch++;
          break;
        }
        else
        {
          edges_served = access_edge(cur_task, i, next_tuple);
          cur_task.start_offset += edges_served;
          /*if(cur_task.vid==81) {
              cout << "Served for i: " << i << " cycle: " << _asic->_cur_cycle << " new start offset: " << cur_task.start_offset << endl;
            }*/
        }
        if (_asic->_config->_all_edge_access == 1)
        { // FIXME: very weirdly I access vectorized edges here
          // cur_task.start_offset = _asic->_offset[cur_task.vid+1];
          i = cur_task.start_offset;
          break;
        }
      }
      // FIXME: not sure why is this here...
      if (_asic->_config->_algo == astar && _asic->_config->_net == mesh)
      {
        // TODO: Put remote access request on the network
        net_packet ind_tuple;
        ind_tuple.req_core_id = _core_id;
        ind_tuple.packet_type = astarUpdate;
        ind_tuple.dest_core_id = _asic->_scratch_ctrl->get_mc_core_id(cur_task.start_offset);
        ind_tuple.dst_id = cur_task.start_offset;
        _asic->_network->push_net_packet(_core_id, ind_tuple);
      }

      // instead of prefetch_process -- push to an intermediate stage
      if (_asic->_config->_dramsim_enabled == 0)
      {
        if (_asic->_config->_prac)
        {
          next_tuple.start_edge_served = _asic->_cur_cycle;
          _pref_lsq[lane_id].push(next_tuple);
        }
        else
        {
          insert_prefetch_process(lane_id, 0, next_tuple);
        }
      }

      _asic->_elem_dispatch++;
      local_elem_disp++;
      int dest_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_task.vid);
    }

    // cout << "Number of dispatch pops in cycle: " << _asic->_cur_cycle << " are: " << num_pops << " total edges requested: " << total_edges_requested << endl;
    // cout << "Total tasks left: " << _task_queue[0][0].begin()->second.size() << endl;
    // future: 1) do I want to take care of cache line here also or maybe leave
    // it? 2) I didn't consider any offset?
    // push the request to the network with the flag of being inactive
    // and requesting core id not -1 (inactive means when it returns back to
    // the net out buffer, don't do anything -- this was to measure network
    // contention)
    // HACK TO MEASURE NETWORK CONTENTION FOR MEMORY REQUESTS -----------------
    // also, this should be a wider request
    /*
      pref_tuple.req_core_id = _core_id;
      pref_tuple.packet_type = dummy;
      pref_tuple.dst_id = cur_task.start_offset;
      */
    // if(_asic->_config->_tesseract) { // Tesseract was a NUMA system: TODO:
    // @vidushi: see if we some things are implemented...
    if (_asic->_config->_numa_contention)
    {
      int dest_core_id = cur_task.start_offset / (32 * 16);
      net_packet pref_tuple = _asic->_network->create_dummy_request_packet(0, cur_task.start_offset, _core_id);
      if (pref_tuple.dest_core_id != _core_id)
      {
        // for(int i=0; i<line_size/bus_width; ++i) { // LINK_BW
        for (int i = 0; i < edges_served / (line_size / bus_width); ++i)
        { // LINK_BW
          _asic->_network->push_net_packet(_core_id, pref_tuple);
        }
      }
    }

    // cout << "Served edge: " << i << " src_id: " << src_id << " end index of the edge: " << _asic->_offset[src_id+1] << endl;
    if (i == _asic->_offset[src_id + 1])
    {
      /*if(src_id==81) {
          cout << "Final offset is: " << _asic->_offset[src_id+1];
        }*/
      _asic->_stats->_stat_tot_completed_tasks++;
      /* if((_asic->_config->_algo==pr) && (_asic->_config->_exec_model==sync || _asic->_switched_sync)) {
          // TODO: vidushi: set only 1 and also at the spawn task, uncomment it!!
           _asic->_scratch[cur_task.vid]=0;
           _asic->_vertex_data[cur_task.vid]=0;
          }*/
      if (_pending_task[tqid].vid != -1)
      {
        _pending_task[tqid].vid = -1;
      }
      else
      {
        (it->second).pop_front();
        if ((it->second).empty())
        {
          _task_queue[tqid][lane_id].erase(it->first);
        }
        _asic->_task_ctrl->_remaining_task_entries[_core_id][tqid]++;

        // this means if we can pull a task in the task queue
        bool should_put_in_tq = false;
        if (_asic->_task_ctrl->_remaining_task_entries[_core_id][tqid] >= HIGH_PRIO_RESERVE && _asic->_task_ctrl->can_pull_from_overflow(_core_id))
        {
          should_put_in_tq = true;
        }
        else if (_asic->_task_ctrl->_remaining_task_entries[_core_id][tqid] < HIGH_PRIO_RESERVE && _fifo_task_queue.size() > 0)
        { // just so as i don't break the order of fifo
          auto itx = _task_queue[tqid][0].begin();
          task_entry pending_tuple = _fifo_task_queue.front();
          // 1 entry will be reserved for priority=-1 task
          if (itx->first > pending_tuple.dist && _asic->_task_ctrl->_remaining_task_entries[_core_id][tqid] > 1)
          {
            should_put_in_tq = true;
          }
        }
        if (should_put_in_tq)
        { // popping from task queue here...
          _asic->_task_ctrl->pull_tasks_from_fifo_to_tq(_core_id, tqid);
        }
      }
      // cout << "Core id: " << _core_id << " POPPED TASK: " << src_id << endl;
      /*if(!_asic->_config->_pull_mode) {
              _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_task.vid);
          }*/

      /*if(_asic->_config->no_swarm()) {
            if(_task_queue[tqid][0].empty()) {
              if(_asic->_task_ctrl->_remaining_task_entries[_core_id][tqid]!=TASK_QUEUE_SIZE) {
                // cout << "remaining entry: " << _asic->_task_ctrl->_remaining_task_entries[_core_id][tqid] << endl;
                assert(0 && "task queue is empty but still remaining entries are not equal to TQ size");
              }
            }
          }*/
    }
    else
    {
      // do this for all scenarios??
      if (1)
      { // !_asic->_config->_pull_mode) {
        if (_pending_task[tqid].vid != -1)
        {
          _pending_task[tqid].start_offset = i;
        }
        else
        {
          (it->second).front().start_offset = i;
          _pending_task[tqid] = (it->second).front();
          (it->second).pop_front();
          if ((it->second).empty())
          {
            _task_queue[tqid][lane_id].erase(it->first);
          }
          // (it->second).front().start_offset=i;
          _asic->_task_ctrl->_remaining_task_entries[_core_id][tqid]++;

          // this means if we can pull a task in the task queue
          bool should_put_in_tq = false;
          if (_asic->_task_ctrl->_remaining_task_entries[_core_id][tqid] >= HIGH_PRIO_RESERVE && _asic->_task_ctrl->can_pull_from_overflow(_core_id))
          {
            should_put_in_tq = true;
          }
          else if (_asic->_task_ctrl->_remaining_task_entries[_core_id][tqid] < HIGH_PRIO_RESERVE && _fifo_task_queue.size() > 0)
          { // just so as i don't break the order of fifo
            auto itx = _task_queue[tqid][0].begin();
            task_entry pending_tuple = _fifo_task_queue.front();
            // 1 entry will be reserved for priority=-1 task
            if (itx->first > pending_tuple.dist && _asic->_task_ctrl->_remaining_task_entries[_core_id][tqid] > 1)
            {
              should_put_in_tq = true;
            }
          }
          if (should_put_in_tq)
          { // popping from task queue here...
            _asic->_task_ctrl->pull_tasks_from_fifo_to_tq(_core_id, tqid);
          }
        }
      }
      else
      { // re-push with updated priority
        (it->second).pop_front();
        if ((it->second).empty())
        {
          _task_queue[tqid][lane_id].erase(it->first);
        }
        if (_asic->_config->is_sync())
        {
          _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_task.vid);
        }
        _asic->_task_ctrl->_remaining_task_entries[_core_id][tqid]++;
        task_entry new_task(src_id, i);
        new_task.dist = cur_task.dist;
        // cout << "Core: " << _core_id << "Re-inserted pull task with priority -1 for vid: " << new_task.vid << " cycle: " << _asic->_cur_cycle << " and new offset: " << i << endl;
        /*if(new_task.vid==11348) {
          }*/
        _asic->_task_ctrl->insert_local_task(tqid, 0, _core_id, -1, new_task);
        return;
      }
    }
  }
  _asic->_stats->_stat_fine_gran_lb_factor[_core_id] = local_elem_disp;
}

bool asic_core::local_pipeline_inactive(bool show)
{
  if (!_asic->_fine_reorder_buf[_core_id]->pipeline_inactive(show))
  {
    if (show)
      cout << "Local fine reorder buf is not empty: " << _asic->_fine_reorder_buf[_core_id]->_entries_remaining << endl;
    return false;
  }
  if (_asic->_stats->_stat_pending_cache_misses != 0)
  {
    if (show)
      cout << "Pending cache misses\n";
    return false;
  }
  // check if cgra event queue is empty
  for (int d = 0; d < MAX_DFGS; ++d)
  {
    if (!_cgra_event_queue[d].empty())
    {
      if (show)
        cout << "CGRA event queue is not empty: " << _cgra_event_queue[d].size() << "\n";
      return false;
    }
  }

  if (!_priority_hit_gcn_updates.empty())
  {
    if (show)
      cout << "Priority hit GCN update queue is not empty: " << _priority_hit_gcn_updates.size() << "\n";
    return false;
  }
  if (!_hit_gcn_updates.empty())
  {
    if (show)
      cout << "Hit GCN update queue is not empty: " << _hit_gcn_updates.size() << "\n";
    return false;
  }
  if (!_miss_gcn_updates.empty())
  {
    if (show)
      cout << "Miss GCN update queue is not empty: " << _miss_gcn_updates.size() << "\n";
    return false;
  }
  if (!_coarse_cgra_event_queue.empty())
  {
    if (show)
      cout << "Multiply CGRA event queue is not empty: " << _coarse_cgra_event_queue.size() << "\n";
    return false;
  }

  if (_asic->_config->_algo == cf && _asic->_stats->_stat_pushed_to_ps != 0)
  {
    if (show)
    {
      cout << "pushed: " << _asic->_stats->_stat_pushed_to_ps << " edges finished: " << _asic->_stats->_stat_tot_finished_edges << " matrix mult finished: " << _asic->_stats->_stat_vec_mat << endl;
      cout << "Pending store entries not served\n";
    }
    return false;
  }
  if (!_asic->_compl_buf[_core_id]->pipeline_inactive(show))
  {
    return false;
  }
  if (!_hol_conflict_queue.empty())
  {
    if (show)
      cout << "Conflict queue for gcn is not empty" << endl;
    return false;
  }
  for (int i = 0; i < _asic->_config->_lane_width; ++i)
  {
    for (int j = 0; j < NUM_TQ_PER_CORE; ++j)
    {
      if (!_task_queue[j][i].empty())
      {
        if (show)
          cout << "Local task queue not empty with size: " << _task_queue[j][i].size() << " at lane id: " << i << " tq id: " << j << " and inorder flag: " << _in_order_flag[i] << endl;
        return false;
      }
    }
    if (!_prefetch_process[i].empty())
    {
      if (show)
        cout << "prefetch_process queue not empty: " << _prefetch_process[i].size() << endl;
      return false;
    }
    if (!_pref_lsq[i].empty())
    {
      if (show)
        cout << "prefetch_lsq queue not empty: " << _pref_lsq[i].size() << endl;
      return false;
    }
    if (!_process_reduce[i].empty())
    {
      if (show)
        cout << "_process_reduce queue not empty: " << _process_reduce[i].size() << endl;
      return false;
    }
  }
  if (!_prefetch_scratch.empty())
  {
    if (show)
      cout << "fine prefetch_scratch queue not empty: " << _prefetch_scratch.size() << endl;
    return false;
  }
  if (!_scratch_process.empty())
  {
    if (show)
      cout << "fine scratch_process queue not empty: " << _scratch_process.size() << endl;
    return false;
  }

  if (_asic->_config->_prac == 1)
  {
    for (int i = 0; i < NUM_TQ_PER_CORE; ++i)
    {
      if (!_aggregation_buffer[i].empty())
      {
        if (show)
          cout << "aggregation buffer queue not empty: " << _aggregation_buffer[i].size() << endl;
        return false;
      }
    }
  }

  for (int i = 0; i < num_banks; ++i)
  {
    if (_asic->_config->_net == crossbar)
    {
      if (!_asic->_bank_queues[i].empty())
      {
        if (show)
          cout << "global bank queues not empty with index: " << i << " and size: " << _asic->_bank_queues[i].size() << "\n";
        return false;
      }
    }
    else
    {
      if (!_local_bank_queues[i].empty())
      {
        if (show)
          cout << "local bank queues not empty with index: " << i << " and size: " << _local_bank_queues[i].size() << "\n";
        return false;
      }
    }
  }
  if (!_fifo_task_queue.empty())
  {
    if (show)
      cout << "FIFO task queues not empty\n";
    return false;
  }
  return true;
}

// true if no pending work
bool asic_core::pipeline_inactive(bool show)
{
  if (!_asic->_task_ctrl->_pending_updates.empty())
  {
    if (show)
      cout << "updates are pending in an overflow buffer: " << _asic->_task_ctrl->_pending_updates.size() << endl;
    return false;
  }
  if (!_asic->_fine_reorder_buf[_core_id]->pipeline_inactive(show))
  {
    if (show)
      cout << "Local fine reorder buf is not empty: " << _asic->_fine_reorder_buf[_core_id]->_entries_remaining << endl;
    return false;
  }
  if (_asic->_delayed_net_tasks != 0)
  {
    if (show)
      cout << "Tasks in the network delay buffer\n";
    return false;
  }
  if (_asic->_mem_ctrl->_cur_writes != 0)
  {
    if (show)
      cout << "Pending scatter writes in SGU slicing\n";
    return false;
  }
  if (_asic->_stats->_stat_pending_cache_misses != 0)
  {
    if (show)
      cout << "Pending cache misses\n";
    return false;
  }
  // check if cgra event queue is empty
  for (int d = 0; d < MAX_DFGS; ++d)
  {
    if (!_cgra_event_queue[d].empty())
    {
      if (show)
        cout << "CGRA event queue is not empty: " << _cgra_event_queue[d].size() << "\n";
      return false;
    }
  }
  if (!_priority_hit_gcn_updates.empty())
  {
    if (show)
      cout << "Priority hit GCN update queue is not empty: " << _priority_hit_gcn_updates.size() << "\n";
    return false;
  }
  if (!_hit_gcn_updates.empty())
  {
    if (show)
      cout << "Hit GCN update queue is not empty: " << _hit_gcn_updates.size() << "\n";
    return false;
  }
  if (!_miss_gcn_updates.empty())
  {
    if (show)
      cout << "Miss GCN update queue is not empty: " << _miss_gcn_updates.size() << "\n";
    return false;
  }
  if (!_coarse_cgra_event_queue.empty())
  {
    if (show)
      cout << "Multiply CGRA event queue is not empty: " << _coarse_cgra_event_queue.size() << "\n";
    return false;
  }

  if (_asic->_config->_algo == cf && _asic->_stats->_stat_pushed_to_ps != 0)
  {
    if (show)
    {
      cout << "pushed: " << _asic->_stats->_stat_pushed_to_ps << " edges finished: " << _asic->_stats->_stat_tot_finished_edges << " matrix mult finished: " << _asic->_stats->_stat_vec_mat << endl;
      cout << "Pending store entries not served\n";
    }
    return false;
  }
  if (!_asic->_compl_buf[_core_id]->pipeline_inactive(show))
  {
    return false;
  }
  if (!_hol_conflict_queue.empty())
  {
    if (show)
      cout << "Conflict queue for gcn is not empty" << endl;
    return false;
  }
  if (_asic->_config->_sgu_gcn_reorder == 0 && _cur_arob_push_ptr != _cur_arob_pop_ptr)
  {
    if (show)
      cout << "Atomic update ROB not empty with push ptr: " << _cur_arob_push_ptr << " and pop ptr: " << _cur_arob_pop_ptr << endl;
    return false;
  }
  if (!_asic->_task_ctrl->_global_task_queue.empty())
  {
    if (show)
      cout << "Global task queue not empty with size: " << _asic->_task_ctrl->_global_task_queue.size() << endl;
    return false;
  }
  if (!_crossbar_input.empty())
  {
    if (show)
      cout << "Crossbar input for graphicionado not empty with size: " << _crossbar_input.size() << endl;
    return false;
  }

  for (int i = 0; i < _asic->_config->_lane_width; ++i)
  {
    for (int j = 0; j < NUM_TQ_PER_CORE; ++j)
    {
      if (!_task_queue[j][i].empty())
      {
        if (show)
          cout << "Local task queue not empty with size: " << _task_queue[j][i].size() << " at lane id: " << i << " tq id: " << j << " and inorder flag: " << _in_order_flag[i] << endl;
        return false;
      }
    }
    if (!_pending_swarm[i].empty())
    {
      if (show)
        cout << "pending swarm queue not empty: " << _prefetch_process[i].size() << endl;
      return false;
    }
    if (!_prefetch_process[i].empty())
    {
      if (show)
        cout << "prefetch_process queue not empty: " << _prefetch_process[i].size() << endl;
      return false;
    }
    if (!_pref_lsq[i].empty())
    {
      if (show)
        cout << "prefetch_lsq queue not empty: " << _pref_lsq[i].size() << endl;
      return false;
    }

    if (!_process_reduce[i].empty())
    {
      if (show)
        cout << "_process_reduce queue not empty: " << _process_reduce[i].size() << endl;
      return false;
    }
  }
  if (!_prefetch_scratch.empty())
  {
    if (show)
      cout << "fine prefetch_scratch queue not empty: " << _prefetch_scratch.size() << endl;
    return false;
  }
  if (!_scratch_process.empty())
  {
    if (show)
      cout << "fine scratch_process queue not empty: " << _scratch_process.size() << endl;
    return false;
  }

  // NEW QUEUES FOR SGU ONLY
  if (_asic->_config->_exec_model == async)
  {
    if (!_stall_buffer.empty())
    {
      if (show)
        cout << "stall buffer queue not empty: " << _stall_buffer.size() << endl;
      return false;
    }
  }
  if (_asic->_config->_prac == 1)
  {
    for (int i = 0; i < NUM_TQ_PER_CORE; ++i)
    {
      if (!_aggregation_buffer[i].empty())
      {
        if (show)
          cout << "aggregation buffer queue not empty: " << _aggregation_buffer[i].size() << endl;
        return false;
      }
    }
  }
  /*
#if SWARM==1
    if(_meta_info_queue.size()!=0) { 
      if(show) cout << "reduce_commit queue not empty: " << _meta_info_queue.size() << endl; 
      return false;
    }
    if(_conflict_queue.size()!=0) return false;
#endif
    */

  if (_commit_queue.size() != 0)
  {
    if (show)
      cout << "Commit queue size not empty\n";
    return false;
  }
  if (_asic->_mem_ctrl->_outstanding_mem_req.size() != 0)
  {
    if (show)
      cout << "Outstanding mem req size not empty\n";
    return false;
  }

  for (int i = 0; i < num_banks; ++i)
  {
    if (_asic->_config->_net == crossbar)
    {
      if (!_asic->_bank_queues[i].empty())
      {
        if (show)
          cout << "global bank queues not empty with index: " << i << " and size: " << _asic->_bank_queues[i].size() << "\n";
        return false;
      }
    }
    else
    {
      if (!_local_bank_queues[i].empty())
      {
        if (show)
          cout << "local bank queues not empty with index: " << i << " and size: " << _local_bank_queues[i].size() << "\n";
        return false;
      }
    }
  }
  if (!_fifo_task_queue.empty())
  {
    if (show)
      cout << "FIFO task queues not empty\n";
    return false;
  }
  return true;
}

bool graphmat::cycle()
{

  _break_this_cycle = false;
  int feat_len = FEAT_LEN1;
  int x = feat_len / (VEC_LEN / message_size);
  if (_asic->_config->is_vector())
  {
    assert(x != 0 && "we are assuming feature length is a multiplier of vector length\n");
  }

  if (_asic->_config->_graphmat_cf && _asic->_cur_cycle % x == 0)
  {
    empty_crossbar_input();
  }
  if (_asic->_config->is_vector())
  {
    // _cur_cycle_ptr = (_cur_cycle_ptr+1)%STORE_LEN; // FIXME: not sure if it is used??
    for (int i = 0; i < _fine_grain_throughput / process_thr; ++i)
    { // this is 1
      for (int j = 0; j < _asic->_config->_lane_width; ++j)
      { // this is 1 for accel
        forward_atomic_updates(i, j);
        forward_multiply_opn(j);
      }
      if (1)
      { // _asic->_cur_cycle%x==0) {
        // FIXME: cannot push to it... but can pop
        for (int i = 0; i < _asic->_config->_lane_width; ++i)
        {
          cf_reduce(i);
        }
        split_updates_in_cache();
      }
    }
    arob_to_pref(); // just let it go, quite approximate: FIXME
  }
  else
  {
    if (_in_order_flag[0])
    {
      reduce(0);
      process(0);
    }
  }
  consume_lsq_responses(0);
  while (!_asic->_compl_buf[_core_id]->_pref_lsq.empty())
  {
    pref_tuple cur_tuple = _asic->_compl_buf[_core_id]->receive_lsq_responses();
    // cout << "Start edge: " << cur_tuple.start_edge_served << " end edge: " << cur_tuple.end_edge_served << endl;
    push_lsq_data(cur_tuple);
  }
  // consuming prefetched and reordered data in the computation pipeline
  _asic->_compl_buf[_core_id]->cb_to_lsq();
  if (_asic->_config->_gcn_swarm)
  {
    for (int i = 0; i < _asic->_config->_lane_width; ++i)
    {
      issue_requests_from_pending_buffer(i);
    }
  }
  if (_asic->_config->_prac == 1 && _asic->_switched_async)
  {
    if (_asic->_cur_cycle % 2)
    { // this should be drained every 2 cycles...
      for (int j = 0; j < NUM_TQ_PER_CORE; ++j)
      {
        drain_aggregation_buffer(j);
      }
    }
  }

  if (_asic->_config->_pull_mode)
  {
    pull_scratch_read();
    push_scratch_data_to_pipeline();
    while (!_asic->_fine_reorder_buf[_core_id]->_pref_lsq.empty())
    {
      pref_tuple cur_tuple = _asic->_fine_reorder_buf[_core_id]->receive_lsq_responses();
      push_scratch_data(cur_tuple);
    }
    _asic->_fine_reorder_buf[_core_id]->cb_to_lsq();
    _asic->_scratch_ctrl->receive_scratch_request();
  }

  // bool is_done = pipeline_inactive(false, true);
  bool net_done = _asic->_network->buffers_not_empty(false);
  bool is_done = pipeline_inactive(false) && !net_done;
  return !is_done;
}

// GRAPHLAB simulator here

bool graphlab::cycle()
{

  reduce(0);
  process(0);

  bool is_done = _asic->no_update() && pipeline_inactive(false) && !_asic->_network->buffers_not_empty(false);
  return !is_done;
}

//---------------------------------------------------------------
// DIJKSTRA simulator here

// pop value from top of the priority queue (in terms of ASIC?)
// have some structure in hardware which can do the work of priority queue
// (find_min is O(logn) instead of O(n) -- I will assume 1 cycle really?
// it can pop just after it's _prev_commit_id->finished_left==0, and start from timestamp
void dijkstra::commit()
{ // int lane_id) {
  // find lowest timestamp
  // cout << "prev_commit vertex: " << _asic->_prev_commit_timestamp << endl;
  bool found = false;
  for (int c = 0; c < core_cnt; ++c)
  {
    auto it = _asic->_asic_cores[c]->_meta_info_queue.find(_asic->_prev_commit_id);
    if (it != _asic->_asic_cores[c]->_meta_info_queue.end())
    {
      found = true;
      if (it->second.finished_left != 0)
      {
        // cout << "Finished left is: " << it->second.finished_left <<" for dst_id: " << _asic->_prev_commit_timestamp << " and prev tid: " << _asic->_prev_commit_id << endl;
        return;
      }
    }
  }
  if (!found)
  {
    cout << "Cannot find id in meta info queue: " << _asic->_prev_commit_id << " at core: " << _asic->_prev_commit_core_id << endl;
    exit(0);
    return;
  }

  int gvt = MAX_TIMESTAMP;
  int lowest_cid = -1;
  int cur_lowest_ts = -1;
  for (int c = 0; c < core_cnt; ++c)
  {
    if (_asic->_asic_cores[c]->_commit_queue.empty())
      continue;
    auto it = _asic->_asic_cores[c]->_commit_queue.begin();
    cur_lowest_ts = it->first;
    if (cur_lowest_ts < gvt)
    {
      gvt = cur_lowest_ts;
      lowest_cid = c;
    }
  }
  if (lowest_cid == -1)
    return;
  auto it2 = (_asic->_asic_cores[lowest_cid]->_commit_queue).begin();
  commit_info x = (it2->second).front(); // first element of vector if it is not 0
  assert(!(it2->second).empty());
  // Step1: remove first element of queue
  (it2->second).pop();
  // Step2: if the remaining vector is empty, erase entry from commit queue
  if ((it2->second).empty())
  {
    _asic->_asic_cores[lowest_cid]->_commit_queue.erase(gvt);
    // cout << "Deleted timestamp: " << gvt << "entry from commit queue\n";
  }
  if (_asic->_scratch_ctrl->_is_visited[x.vid])
    return;
  assert(!_asic->_scratch_ctrl->_is_visited[x.vid]);
  _asic->_scratch[x.vid] = it2->first; // x.timestamp;

  // cout << "Going to commit with lowest timestamp: " << gvt << " commit vertex is: " << x.vid << " and elements in queue: " << (it2->second).size() << endl;
  // cout << "COMMIT NEW _graph_verticesERTEX: " << x.vid << " AT SHORTEST DISTANCE: " << it2->first << endl;
  // META-INFORMATION UPDATE
  _asic->_prev_commit_core_id = lowest_cid;
  _asic->_prev_commit_id = x.own_tid;
  _asic->_prev_commit_timestamp = x.vid; // for debugging purposes
  _asic->_scratch_ctrl->_is_visited[x.vid] = true;

  // spawn new tasks -- last not required for non-speculative
  // task_entry new_task(x.vid,_asic->_offset[x.vid], x.own_tid,_core_id,-1);
  task_entry new_task(x.vid, _asic->_offset[x.vid]);
  _asic->_task_ctrl->insert_global_task(it2->first, new_task);
}

bool dijkstra::cycle()
{

  // commit(); // in that case, only 1 core should call commit (the main one)
  reduce(0);  // reduce in the compute-enabled memory
  process(0); // find the sum
  // dispatch(); // prefetch: read all the edges

  // not true for unconnected graphs -- condition is priority queue should be
  // empty
  bool is_done = _asic->_scratch_ctrl->_is_visited.all();
  // bool is_done = pipeline_inactive(false, false); // _asic->_is_visited.all();
  // bool is_done = pipeline_inactive(false, false) && !_asic->_network->buffers_not_empty();
  if (is_done)
  { // if all visited, copy to memory and send done
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      _asic->_vertex_data[i] = _asic->_scratch[i];
    }
  }
  return !is_done;
}

// SWARM simulator here ---------------------------------------------

// returns false if not done
bool swarm::cycle()
{

  for (int i = 0; i < _asic->_config->_lane_width; ++i)
  {
    /*if(_core_id==0 && i==0) {
      cout << "Came for core: " << _core_id << " lane_id: " << i << endl;
    }*/
    reduce(i);
    process(i);
    /*if(_in_order_flag[i]) {
      reduce(i);
      process(i);
    }*/
  }
  consume_lsq_responses(0);
  while (!_asic->_compl_buf[_core_id]->_pref_lsq.empty())
  {
    push_lsq_data(_asic->_compl_buf[_core_id]->receive_lsq_responses());
  }
  // consuming prefetched and reordered data in the computation pipeline
  _asic->_compl_buf[_core_id]->cb_to_lsq();

#if CF_SWARM == 1
  arob_to_pref();
  int x = FEAT_LEN / VEC_LEN;
  // if(_asic->_cur_cycle%x==0) {
  if (1)
  {
    for (int i = 0; i < _asic->_config->_lane_width; ++i)
    {
      cf_reduce(i);
    }
  }
  split_updates_in_cache();
  for (int i = 0; i < _asic->_config->_lane_width; ++i)
  {
    forward_atomic_updates(i);
  }
#endif
  for (int i = 0; i < _asic->_config->_lane_width; ++i)
  {
    issue_requests_from_pending_buffer(i);
  }
  // dispatch();

  bool is_done = pipeline_inactive(false) && !_asic->_network->buffers_not_empty(false);

  if (is_done)
  { // if all visited, copy to memory and send done
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      _asic->_vertex_data[i] = _asic->_scratch[i];
    }
  }
  return !is_done;
}

// returns false if not done
bool sgu::cycle()
{
  fill_task_queue();

  if (_asic->_config->_prac == 1 && _asic->_cur_cycle % 2)
  { // this should be drained every 2 cycles...
    for (int j = 0; j < NUM_TQ_PER_CORE; ++j)
    {
      drain_aggregation_buffer(j);
    }
  }

  if (_asic->_config->is_vector())
  {
    // TODO: can set for maximum parallelism and rest can be taken care of by
    // the ready cycle (which can be float)...Shouldn't this be called for all
    // dfg ids? MAX_DFGS=process_thr
#if TC == 0 || MAX_DFGS == 1
    for (int i = 0; i < _asic->_config->_lane_width; ++i)
    {
      cf_reduce(0);
      forward_atomic_updates(0, i);
    }
#else
    int start = rand() % MAX_DFGS;
    for (int i = start, j = 0; j < MAX_DFGS; ++j, i = (i + 1) % MAX_DFGS)
    {
      cf_reduce(0, i);
      forward_atomic_updates(i, 0);
    }
#endif
    split_updates_in_cache();
    arob_to_pref();
  }
  else
  {
    reduce(0);
    process(0);
  }
  consume_lsq_responses(0);
  int new_core = _core_id;
  int group = _core_id / 4;
  if (_asic->_config->_edge_load_bal)
  {
    new_core = ((group * 4) + _asic->_last_cb_core[group]);
  }
  while (!_asic->_compl_buf[_core_id]->_pref_lsq.empty() && _asic->_asic_cores[new_core]->can_push_in_prefetch(0))
  {
    if (!_asic->_config->_edge_load_bal)
    {
      push_lsq_data(_asic->_compl_buf[_core_id]->receive_lsq_responses());
    }
    else
    {
      _asic->_asic_cores[new_core]->push_lsq_data(_asic->_compl_buf[_core_id]->receive_lsq_responses());
      _asic->_last_cb_core[group] = (_asic->_last_cb_core[group] + 1) % 4;
      new_core = ((group * 4) + _asic->_last_cb_core[group]);
      /*if(!_asic->_compl_buf[_core_id]->_pref_lsq.empty() && !_asic->_asic_cores[new_core]->can_push_in_prefetch(0)) {
        cout << "Cannot push to core: " << endl;
      //  ++_cycles_blocked;
      }*/
    }
  }
  // consuming prefetched and reordered data in the computation pipeline
  // cout << "Pushing memory cb to lsq from core: " << _core_id << endl;
  _asic->_compl_buf[_core_id]->cb_to_lsq();

  if (_asic->_config->_pull_mode)
  {
    pull_scratch_read();
    push_scratch_data_to_pipeline();
    while (!_asic->_fine_reorder_buf[_core_id]->_pref_lsq.empty())
    {
      pref_tuple cur_tuple = _asic->_fine_reorder_buf[_core_id]->receive_lsq_responses();
      push_scratch_data(cur_tuple);
      /*if(_core_id==4 && cur_tuple.src_id==38036) {
          cout << "Pushing at cycle: " << _asic->_cur_cycle << endl;
      }*/
    }
    // cout << "Pushing scratch cb to lsq from core: " << _core_id << endl;
    _asic->_fine_reorder_buf[_core_id]->cb_to_lsq();
    _asic->_scratch_ctrl->receive_scratch_request();
  }

  bool done1 = pipeline_inactive(false);
  // cout << "Is done1? " << done1 << endl;

  // bool is_done = done1 && !_asic->_network->buffers_not_empty();
  bool is_done = pipeline_inactive(false) && !_asic->_network->buffers_not_empty(false);

  const int max_tree_nodes = pow(2, _asic->kdtree_depth + 1);
  if (is_done && !_asic->_switched_sync && !_asic->_config->is_vector())
  { // if all visited, copy to memory and send done
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      _asic->_vertex_data[i] = _asic->_scratch[i];
      if (_asic->_config->_domain == tree && i == max_tree_nodes)
      {
        break;
      }
    }
  }
  return !is_done;
}

bool espresso::cycle()
{
  reduce(0);
  process(0);

  bool done1 = pipeline_inactive(false);
  bool is_done = done1 && !_asic->_network->buffers_not_empty(false);
  if (is_done)
  {
    for (int i = 0; i < _asic->_graph_vertices; ++i)
    {
      _asic->_vertex_data[i] = _asic->_scratch[i];
    }
  }
  return !is_done;
}

bool asic_core::check_atomic_conflict(int lane_id, int feat_len, int cur_vid)
{
  return false;
#if GCN_SWARM == 1 || CF_SWARM == 1
  return false; // pull implementation
#endif
  if (_asic->_config->_pull_mode)
    return false;
  // return false;
  // search for vid available in event queue
  for (int d = 0; d < MAX_DFGS; ++d)
  {
    for (auto it = _cgra_event_queue[d].begin(); it != _cgra_event_queue[d].end(); ++it)
    {
      if (it->vid == cur_vid)
      {
        _asic->_stats->_stat_blocked_with_conflicts++;
        _asic->_stats->_stat_incorrect_edges[_core_id]++;
        // cout << "Atomic conflict found\n";
        return true;
      }
    }
  }

  /*for(int i=0; i<STORE_LEN; i+=(feat_len/VEC_LEN)) { 
    if(_pending_stores[lane_id][i].first==cur_vid) {
      // if(_asic->_stats->_stat_tot_finished_edges==65640)
      // cout << "blocked for entry: " << i << endl;
      // cout << " conflict with vid: " << cur_vid << endl;
      _asic->_stats->_stat_blocked_with_conflicts++;
      _asic->_stats->_stat_incorrect_edges[_core_id]++;
      return true;
    }
  }*/
  return false;
}

// TODO: another pipeline stage to push to hit and miss
// When two outgoing queues, it always blocks one of them...
// Why are all misses always?? tasks are pushed here after they are put in here. Hence, we need to push
// misses directly to miss_gcn instead of sending to memory and hits to hit_gcn.
// Here this should be all hits...
void asic_core::split_updates_in_cache()
{
  return;
  /*while(!_prefetch_process[0].empty()) { // } && can_push_in_local_bank_queue(0)) {
      pref_tuple cur_tuple = _prefetch_process[0].front();
      uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id*message_size*FEAT_LEN);
      bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
      if(l2hit) {
        push_gcn_cache_hit(cur_tuple);
      } else {
        push_gcn_cache_miss(cur_tuple);
      }
      _prefetch_process[0].pop_front();
    }*/
}

bool asic_core::can_serve_hit_updates()
{
  while (!_hit_gcn_updates.empty() && _hit_gcn_updates.front().invalid)
  {
    _hit_gcn_updates.pop_front();
  }
  // TODO: use the is_queue_head_func()
  return is_queue_head_hit(&_hit_gcn_updates);
  if (!_hit_gcn_updates.empty())
  {
    ++_asic->_stats->_stat_cycles_hit_queue_is_chosen;
    pref_tuple cur_tuple = _hit_gcn_updates.front();
    uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.src_id * message_size * FEAT_LEN);
    bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
    if (l2hit)
    {
      // cout << "Correct hit, enqueue cycle: " << cur_tuple.src_dist << " dequeue cycle: " << _asic->_cur_cycle << " vid: " << cur_tuple.src_id << endl;
      ++_asic->_stats->_stat_correct_cache_hit_tasks;
      return true;
    }
    else
    {
      ++_asic->_stats->_stat_wrong_cache_hit_tasks;
      // cout << "Incorrect hit, enqueue cycle: " << cur_tuple.src_dist << " dequeue cycle: " << _asic->_cur_cycle  << " vid: " << cur_tuple.src_id << endl;
      _hit_gcn_updates.pop_front();
      while (!_hit_gcn_updates.empty() && _hit_gcn_updates.front().invalid)
      {
        _hit_gcn_updates.pop_front();
      }
      push_gcn_cache_miss(cur_tuple);
      if (!_hit_gcn_updates.empty())
      {
        cur_tuple = _hit_gcn_updates.front();
        paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.src_id * message_size * FEAT_LEN);
        l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
        return l2hit;
      }
      return false;
    }
  }
  return false;
}

bool asic_core::can_serve_miss_updates()
{
  while (!_miss_gcn_updates.empty() && _miss_gcn_updates.front().invalid)
  {
    _miss_gcn_updates.pop_front();
  }
  if (!_miss_gcn_updates.empty())
  {
    ++_asic->_stats->_stat_cycles_miss_queue_is_chosen;
    pref_tuple cur_tuple = _miss_gcn_updates.front();
    assert(!cur_tuple.invalid && "no need to serve invalid entries");

    // uint64_t paddr = base_addr + (update_tuple.edge.dst_id*message_size*FEAT_LEN) + (j*bus_width);
    uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id * message_size * FEAT_LEN);
    bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
    if (!l2hit)
    {
      // cout << "Correct miss, enqueue cycle: " << cur_tuple.src_dist << " dequeue cycle: " << _asic->_cur_cycle  << " vid: " << cur_tuple.src_id << endl;
      ++_asic->_stats->_stat_correct_cache_hit_tasks;
      pref_tuple update_tuple;
      update_tuple.update_width = bus_width;
      update_tuple.edge.dst_id = cur_tuple.edge.dst_id;
      update_tuple.src_id = cur_tuple.src_id;
      update_tuple.second_buffer = cur_tuple.second_buffer;
      update_tuple.cb_entry = -1; // unordered packet
      ++_mem_requests_this_slice;
      bool sent_request = _asic->_mem_ctrl->send_mem_request(false, paddr, update_tuple, VERTEX_SID);
      // if(!sent_request) _asic->_mem_ctrl->_l2hits++;
      if (sent_request)
      {
        --_asic->_mem_ctrl->_l2hits;
        // cout << "Sent request for dst: " << cur_tuple.edge.dst_id << " at cycle: " << _asic->_cur_cycle << endl;
      }
      // else cout << "Sent scratch request for vid: " << update_tuple.src_id << endl;
      _asic->_stats->_stat_pending_cache_misses++;
      _asic->_mem_ctrl->_num_pending_cache_reqs++;
      _miss_gcn_updates.pop_front();
      return !_hit_gcn_updates.empty();
    }
    else
    {
      // cout << "Incorrect miss, enqueue cycle: " << cur_tuple.src_dist << " dequeue cycle: " << _asic->_cur_cycle << " vid: " << cur_tuple.src_id << endl;
      ++_asic->_stats->_stat_wrong_cache_hit_tasks;
      // push this in hit queue, send true
      // _asic->_mem_ctrl->_l2hits++;
      _miss_gcn_updates.pop_front();
      _priority_hit_gcn_updates.push_back(cur_tuple);
      return true;
    }
  }
  return false;
}
// TODO: collect stats on the %age of time when enqueue classification is true
// If miss queue, it should send memory requests, and when done, push the update in hits queue.
// Otherwise, do the normal process.
// queue<pref_tuple>* asic_core::select_queue_for_cache_hit() {
bool asic_core::select_queue_for_cache_hit()
{
  while (!_miss_gcn_updates.empty() && _miss_gcn_updates.front().invalid)
  {
    _miss_gcn_updates.pop_front();
  }
  while (!_hit_gcn_updates.empty() && _hit_gcn_updates.front().invalid)
  {
    _hit_gcn_updates.pop_front();
  }
  // if memory responses after switch to this slice is greater than a constant, it is execution phase
  // check for the switching condition
  // if(_mem_responses_this_slice>100) { // this should memory requests this slices
  // _asic->_mem_ctrl->_slice_execution=false;
  // TODO: it should check the phase changing condition for every 200 cycles or so
  _slice_execution = false;
  /*if(!_slice_execution && _mem_requests_this_slice>8192) { // this should memory requests this slices
     // if(!_slice_execution && _mem_requests_this_slice>8192) { // this should memory requests this slices
        _slice_execution=true;
        cout << "Core id: " << _core_id << " Switched to slice execution at cycle: " << _asic->_cur_cycle << endl;
    } // FIXME: does not switch after the first go, what happens to the miss queue? (deprioritized?) -- condition: do not serve miss when hit is empty..
    // if(_slice_execution && (_edges_done_this_slice>E/4 || _hit_queue_empty_cycles>20)) { // } || _miss_gcn_updates.size()>10*_hit_gcn_updates.size())) { // to prevent stall/lower hit queue throughput due to dependencies
    if(_slice_execution && _hit_queue_empty_cycles>10) {
      // if(_slice_execution && (_edges_done_this_slice>100 || _hit_queue_empty_cycles>2 || _miss_gcn_updates.size()>10*_hit_gcn_updates.size())) { // to prevent stall/lower hit queue throughput due to dependencies
        _slice_execution=false;
        _mem_responses_this_slice=0;
        _mem_requests_this_slice=0;
        _edges_done_this_slice=0;
        cout << "Core id: " << _core_id << " Switched to slice switching at cycle: " << _asic->_cur_cycle << endl;
    }*/
  // FIXME: does this work even if priority queue is still serving?
  if (_hit_gcn_updates.empty())
  {
    ++_hit_queue_empty_cycles;
    _hit_queue_was_empty_last_time = true;
  }
  else
  {
    _hit_queue_was_empty_last_time = false;
    _hit_queue_empty_cycles = 0;
  }
  if (_slice_execution)
  { // just serve hits
    bool can_serve_hit_requests = can_serve_hit_updates();
    // cout << "Core: " << _core_id << " Cycle: " << _asic->_cur_cycle << " identified slice execution phase" << " served: " << can_serve_hit_requests << " hit queue: " << _hit_gcn_updates.size() << " miss queue: " << _miss_gcn_updates.size() << endl;
    return can_serve_hit_requests;
    /*if(!can_serve_hit_requests) {
            return can_serve_miss_updates();
        } else {
            return true;
        }*/
  }
  else
  { // prioritize misses
    // bool can_serve_miss_requests;
    can_serve_miss_updates();
    // cout << "Core: " << _core_id << " Cycle: " << _asic->_cur_cycle << " identified slice switching phase served: " << can_serve_miss_requests << " hit queue: " << _hit_gcn_updates.size() << " miss queue: " << _miss_gcn_updates.size() << endl;
    return can_serve_hit_updates(); // served miss in parallel
                                    /*if(!can_serve_miss_requests) {
            return can_serve_hit_updates();
        } else {
            return true;
        }*/
  }
  return false;
  // return &_miss_gcn_updates;
  // If finishing phase
}

// TODO: if not hit, push to miss queue. Check the next element, if hit continue; else break;
bool asic_core::is_queue_head_hit(deque<pref_tuple> *target_queue)
{
  if (target_queue->empty())
    return false;
  pref_tuple cur_tuple = target_queue->front();
  uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id * message_size * FEAT_LEN);
  bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
  if (l2hit)
    return true;
  else
  {
    if (l2hit)
      return true;
    else
    {
      if (_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id] == 0)
      {
        _asic->_stats->_stat_pending_cache_misses++;
        _asic->_mem_ctrl->_num_pending_cache_reqs++;
        pref_tuple update_tuple;
        update_tuple.update_width = bus_width;
        update_tuple.edge.dst_id = cur_tuple.edge.dst_id;
        update_tuple.src_id = cur_tuple.src_id;
        update_tuple.second_buffer = cur_tuple.second_buffer;
        update_tuple.cb_entry = -1; // unordered packet
        _asic->_mem_ctrl->_l2accesses++;
        bool sent_request = _asic->_mem_ctrl->send_mem_request(false, paddr, update_tuple, VERTEX_SID);
        if (!sent_request)
          _asic->_mem_ctrl->_l2hits++;
      }
      ++_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id];
      target_queue->pop_front();
      if (!target_queue->empty())
      {
        cur_tuple = target_queue->front();
        paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id * message_size * FEAT_LEN);
        l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
        return l2hit;
      }
      else
      {
        return false;
      }
    }
  }
}

void asic_core::serve_prefetch_reqs()
{
  if (!_prefetch_process[0].empty())
  {
    // assert(!_asic->_config->_cache_hit_aware_sched && "should not come here for cache hit");
    // if miss, this will be HOL? No, just pop from it, and no return.
    // TODO: make a function if sufficient duplicate
    auto it = _prefetch_process[0].begin();
    pref_tuple cur_tuple = (it->second).front(); // _prefetch_process[0].front();
    uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id * message_size * FEAT_LEN);
    bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
    /*if(cur_tuple.edge.dst_id > ((_core_id*V/core_cnt) + 10000)) {
      l2hit = true;
    }*/
    if (!l2hit)
    {
      // if(_asic->_stats->_stat_pending_cache_misses)
      _asic->_stats->_stat_pending_cache_misses++;
      _asic->_mem_ctrl->_num_pending_cache_reqs++;
      pref_tuple update_tuple;
      update_tuple.update_width = bus_width;
      update_tuple.edge.dst_id = cur_tuple.edge.dst_id;
      update_tuple.src_id = cur_tuple.src_id;
      update_tuple.second_buffer = cur_tuple.second_buffer;
      update_tuple.cb_entry = -1;       // unordered packet
      update_tuple.req_core = _core_id; // unordered packet
      // cout << "Sending memory request with end edge served as: " << cur_tuple.end_edge_served << endl;
      update_tuple.end_edge_served = cur_tuple.end_edge_served;
      _asic->_mem_ctrl->_l2accesses++;
      bool sent_request = _asic->_mem_ctrl->send_mem_request(false, paddr, update_tuple, VERTEX_SID);
      if (!sent_request)
        _asic->_mem_ctrl->_l2hits++;
      // cout << "Miss request at core: " << _core_id << " and cycle: " << _asic->_cur_cycle << " and vid: " << cur_tuple.edge.dst_id << endl;
      pop_from_prefetch_process();
    }
  }
}

bool asic_core::serve_priority_hits()
{
  bool should_return = false;
  if (_priority_hit_gcn_updates.empty())
  {
    if (!_prefetch_process[0].empty())
    {
      // pref_tuple cur_tuple = _prefetch_process[0].front();
      auto it = _prefetch_process[0].begin();
      pref_tuple cur_tuple = (it->second).front();
      uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id * message_size * FEAT_LEN);
      bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
      /*if(cur_tuple.edge.dst_id > ((_core_id*V/core_cnt) + 10000)) {
        l2hit = true;
      }*/
      if (!l2hit)
      { // another miss
        // cout << "Stall at core: " << _core_id << " cycle: " << _asic->_cur_cycle << endl;
        should_return = true;
      }
      else
      { // first one is a hit
        _asic->_mem_ctrl->_l2accesses++;
        _asic->_mem_ctrl->_l2hits++;
        ++_asic->_stats->_stat_tot_hit_updates_served[_core_id];
        // Let me serve another miss request if it didn't go above
        if (!_prefetch_process[0].empty())
        { // checking if second is a miss
          auto it = _prefetch_process[0].begin();
          pref_tuple cur_tuple = (it->second).front(); // _prefetch_process[0].front();
          uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id * message_size * FEAT_LEN);
          bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
          if (!l2hit)
          {
            pref_tuple update_tuple;
            update_tuple.update_width = bus_width;
            update_tuple.edge.dst_id = cur_tuple.edge.dst_id;
            update_tuple.src_id = cur_tuple.src_id;
            update_tuple.second_buffer = cur_tuple.second_buffer;
            update_tuple.cb_entry = -1; // unordered packet
            _asic->_mem_ctrl->_l2accesses++;
            bool sent_request = _asic->_mem_ctrl->send_mem_request(false, paddr, update_tuple, VERTEX_SID);
            if (!sent_request)
              _asic->_mem_ctrl->_l2hits++;
            pop_from_prefetch_process();
          }
        }
      }
    }
    else
    {
      // cout << "Stall at cycle: " << _asic->_cur_cycle << " core: " << _core_id << endl;
      should_return = true;
    }
  }
  return should_return;
}

// TODO: this should schedule b/w two queues and pop task from there. (no access to prefetch process)
// TODO: (also how many times miss has to be scheduled
//  even after the cache is full: this is seen as switching slices)
// ---------------------------wider atomic update for CF implementation----
// if I want, I can also put this in the sgu::reduce, so that cf can use
// TODO: throughput of this is also fixed...
void asic_core::cf_reduce(int lane_id, int dfg_id)
{

  // Stats collection
  if (_asic->_config->_cache_hit_aware_sched)
  {
    _asic->_stats->_stat_tot_hit_updates_occupancy[_core_id] += _hit_gcn_updates.size();
    _asic->_stats->_stat_tot_priority_hit_updates_occupancy[_core_id] += _priority_hit_gcn_updates.size();
    _asic->_stats->_stat_tot_miss_updates_occupancy[_core_id] += _miss_gcn_updates.size();
  }
  else
  {
    _asic->_stats->_stat_tot_prefetch_updates_occupancy[_core_id] += _prefetch_process[0].size();
  }

  // cout << "Cycle: " << _asic->_cur_cycle << " l2hits: " << _asic->_mem_ctrl->_l2hits << endl;
  if (_asic->_config->_net != crossbar && _asic->_config->_net == hrc_xbar)
  {
    if (_asic->_cur_cycle % _asic->_xbar_bw == 0)
      return;
  }
  // TODO: I do not want to update all data-structures...
  deque<pref_tuple> *target_queue = &_hit_gcn_updates; // &_prefetch_process[0];
  bool is_prefetch = true;
  /*if(_core_id==0) {
    cout << "Cycle: " << _asic->_cur_cycle << " edges done this slice: " << _edges_done_this_slice << " phase: " << _slice_execution
         << " hit queue entries: " << _hit_gcn_updates.size() << " priority queue entries: " << _priority_hit_gcn_updates.size() << endl;
  }*/
  if (_asic->_config->_cache_hit_aware_sched && _asic->_config->_working_cache == 1)
  {
    if (!_priority_hit_gcn_updates.empty())
    {
      target_queue = &_priority_hit_gcn_updates;
      is_prefetch = false;
      if (!_slice_execution)
      { // when overlap doesn't cause thrashing
        can_serve_miss_updates();
      }
      // not a problem as we will put data in
      // if(!is_queue_head_hit(target_queue)) return;
      ++_asic->_stats->_stat_tot_priority_hit_updates_served[_core_id];
      _hit_queue_empty_cycles = 0;
      _hit_queue_was_empty_last_time = false;
    }
    else
    {
      serve_prefetch_reqs(); // to check if it is a hit/miss
      if (_prefetch_process[0].empty())
        return;
      bool should_return = serve_priority_hits();
      if (should_return)
        return;
      /*bool should_serve_hit = select_queue_for_cache_hit();
      if (should_serve_hit) {
        target_queue = &_hit_gcn_updates;
        is_prefetch = false;
        ++_asic->_stats->_stat_tot_hit_updates_served[_core_id];
      } else {
        return;
      }
      // for the case, when any new miss is now a hit in the cache
      if (!_priority_hit_gcn_updates.empty()) {
        target_queue = &_priority_hit_gcn_updates;
        is_prefetch = false;
      }*/
    }
  }
  else if (_asic->_config->_working_cache == 1)
  { // TODO: should it sent miss request in parallel? I guess Yes!!
    if (!_priority_hit_gcn_updates.empty())
    {
      target_queue = &_priority_hit_gcn_updates;
      is_prefetch = false;
      ++_asic->_stats->_stat_tot_priority_hit_updates_served[_core_id];
    }
    else
    {
      serve_prefetch_reqs(); // to check if it is a hit/miss
      if (_prefetch_process[0].empty())
        return;
      bool should_return = serve_priority_hits();
      if (should_return)
        return;
    }
  }
  assert(_asic->_config->_cf && "cf should be 1 for vectorized algorithm");
  /*if(*target_queue!=_priority_hit_gcn_updates) {
    cout << "Hit request at core: " << _core_id << " and cycle: " << _asic->_cur_cycle << endl;
  }*/

  // directly pop from dispatch -- process queue
  // cout << "came in cf reduce\n";
  pref_tuple cur_tuple;
  int elem_done = 0;

  int feat_len = FEAT_LEN;
  if (_asic->_config->_algo == gcn)
  {
    if (_asic->_stats->_stat_barrier_count == 0)
      feat_len = FEAT_LEN1;
    if (_asic->_stats->_stat_barrier_count == 1)
      feat_len = FEAT_LEN2;
  }
  bool go_with_prefetch = true;
  // Step1: decide which queue to go with
  if (!_hol_conflict_queue.empty())
  {
    cur_tuple = _hol_conflict_queue.front();
    // if(!check_atomic_conflict(cur_tuple.lane_id, cur_tuple.update_width, cur_tuple.edge.dst_id)) {
    if (!check_atomic_conflict(0, cur_tuple.update_width, cur_tuple.edge.dst_id))
    {
      go_with_prefetch = false;
      _hol_conflict_queue.pop();
    }
  }

  // FIXME: hot conflict queue can do in any lane id
  if (!_asic->_config->no_swarm())
  {
    if (!go_with_prefetch)
      lane_id = 0; // cur_tuple.lane_id;
  }
  else
  {
    assert(lane_id == 0);
  }

  // every alternate cycle, this has an entry for every core...
  // 16 cores need 32 bytes per cycle
  // 8 cores need 64 bytes every cycle
  // thus 512 bytes/cycle
  if (_asic->_config->_cache_hit_aware_sched)
  {
    assert((!(*target_queue).empty() || !_prefetch_process[0].empty()) && "target queue cannot be empty for cache hit aware");
  }

  // if(_asic->_cur_cycle<210) {
  //   cout << "Cycle: " << _asic->_cur_cycle << " core: " << _core_id << "go with prefetch: " << go_with_prefetch << " prefetch process empty: " << _prefetch_process[0].empty() << endl;
  // }

  if ((!go_with_prefetch || !(*target_queue).empty() || !_prefetch_process[0].empty()) && can_push_to_cgra_event_queue())
  {

    _asic->_gcn_updates[_core_id]++;

    // get an entry in the waiting list
    if (go_with_prefetch)
    {
      // TODO: if prefetch_process, then other way of reading it..
      if (is_prefetch)
      {
        // cout << "Selected prefetch process queue\n";
        auto it = _prefetch_process[0].begin();
        cur_tuple = (it->second).front();
      }
      else
      {
        // cout << "Selected priority hit gcn queue\n";
        assert(_priority_hit_gcn_updates.size() > 0 && "why is this queue larger?");
        cur_tuple = (*target_queue).front();
      }
      /*if(target_queue!=&_priority_hit_gcn_updates) {
        uint64_t paddr = _asic->_mem_ctrl->_vertex_offset + (cur_tuple.edge.dst_id * message_size * FEAT_LEN);
        bool l2hit = _asic->_mem_ctrl->is_cache_hit(_core_id, paddr, ACCESS_LOAD, true, 0);
        // assert(l2hit && "should not be working with non-hit");//except with priority_gcn
        // cout << "Hit request at core: " << _core_id << " and cycle: " << _asic->_cur_cycle << " for vid: " << cur_tuple.edge.dst_id << endl;
      }*/

      // if(!l2hit) return; // the next entry in hit queue is also a miss

      // the same thing is outstanding in memory too
      /*while(_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id]>0) {
        _asic->_asic_cores[_core_id]->_priority_hit_gcn_updates.push_back(cur_tuple);
        --_asic->_task_ctrl->_present_in_miss_gcn_queue[cur_tuple.edge.dst_id];
      }*/
    }
    assert(!cur_tuple.invalid && "update tuples cannot be invalid");
    // cout << "Core: " << _core_id << " Served request for dst: " << cur_tuple.edge.dst_id << " at cycle: " << _asic->_cur_cycle << endl;
    int cur_vid = cur_tuple.edge.dst_id;
    if (_asic->_config->_pull_mode)
    {
      cur_vid = cur_tuple.src_id;
    }
    // for pull, this is coming for the same id.
    // cout << "Found non-conflicting data in prefetch process for sending to cgra with vid: " << cur_vid << "\n";

    // check for conflicts in the pending queue
    bool conflict = check_atomic_conflict(lane_id, feat_len, cur_vid); // false;
    if (!go_with_prefetch)
      assert(!conflict);

    // if no conflict
    if (!conflict)
    {

      // FIXME: this one or the later one?
      bool spawn = false;
      bool should_push = true;
      if (_asic->_config->_algo == cf)
      { // TODO: for the pull mode, this is done in a loop for the incoming edges; and the condition is same for spawn
        int all_edges_received = num_packets - 1;
        // cout << "Src_id: " << cur_tuple.src_id << " dst_id: " << cur_tuple.edge.dst_id << " wgt: " << cur_tuple.edge.wgt
        // << " is last: " << cur_tuple.last << " end_edge_served: " << cur_tuple.end_edge_served << " all_edges_received: " << all_edges_received << endl;
        // this should be done only at the last packet...
        // FIXME: other than src_id, all are garbage values..
        if (_asic->_config->_pull_mode || cur_tuple.end_edge_served == all_edges_received)
        { // last works for pull mode
          if (_asic->_config->_pull_mode)
          { // FIX THIS...just identify this end somehow; and we should not need dst_id and wgt from the tuple...
            if (cur_tuple.last)
            { // correct only twice, do not need that check
              if (_asic->_config->_entry_abort)
              {
                _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_tuple.src_id);
              }
              // dfg length of an update should be larger; also implement this correctly...
              DTYPE error = 0; // it will come here twice??
              for (int e = _asic->_offset[cur_vid]; e < _asic->_offset[cur_vid + 1]; ++e)
              {
                error += _asic->cf_update(_asic->_neighbor[e].dst_id, cur_tuple.src_id, _asic->_neighbor[e].wgt);
              }
              int in_degree = (_asic->_offset[cur_vid + 1] - _asic->_offset[cur_vid]);
              // error /= in_degree;
              assert(!isnan(error) && "Error is NAN, not allowed");
              _asic->_stats->_stat_tot_finished_edges += (in_degree - 1);
              spawn = _asic->_config->is_async() && (error > epsilon);
              // cout << "Executed task for vid: " << cur_vid << " error: " << error << " is spawned: " << spawn << "\n";
            }
            else
            {
              should_push = false;
            }
          }
          else
          {
            DTYPE error = _asic->cf_update(cur_tuple.src_id, cur_tuple.edge.dst_id, cur_tuple.edge.wgt);
            spawn = _asic->_config->is_async() && error > epsilon;
            // cout << "Src_id: " << cur_tuple.src_id << " dst_id: " << cur_tuple.edge.dst_id << " error: " << error << endl;
          }
        }
        if (_asic->_config->_pull_mode == 1 && !cur_tuple.last)
        {
          should_push = false;
        }
      }

      // push in the pending queue
      /*int delay_df_len = (_cur_cycle_ptr+DFG_LENGTH)%STORE_LEN;
      if(!_asic->_config->no_swarm()) {
        delay_df_len = (_cur_cycle_ptr+FEAT_LEN1/VEC_LEN)%STORE_LEN;
      }*/

      // REAL EXECUTION OF THIS PIPELINE STAGE: ISSUE THE ATOMIC UPDATE
      // REQUESTS
      // + latency of cgra + number of vector instances
      // TODO: for variable-sized packets, ceil(cur_tuple.packet_len/vec_len)
      // int ready_cycle = _cur_free_cycle + DFG_LENGTH + FEAT_LEN/VEC_LEN;
      // cout << "vid: " << cur_vid << " update width in CF: " << cur_tuple.update_width << endl;
      // cout << "ready cycle: " << ready_cycle << " cur free cycle: " << _cur_free_cycle << " update width: " << bus_width << endl;

      float prio = 0;
      // #if CF==1 && GCN==0 && FIFO==0 && SGU==1
      //     // TODO: calculate L1 norm of the associated scratch vector (is it even available here or we should pass it from earlier)
      //       for(int f=0; f<FEAT_LEN; ++f) {
      //           prio += abs(old_feat_vec[f]-_asic->_scratch_vec[f][cur_vid]);
      //       }
      //       prio = (_asic->_offset[cur_vid+1]-_asic->_offset[cur_vid])/(float)prio;
      // #endif
      if (should_push)
      {
        insert_vector_task(cur_tuple, spawn, dfg_id);
      }

      if (go_with_prefetch)
      {
        int times = FEAT_LEN / 16;

        if (is_prefetch)
        {
          pop_from_prefetch_process();
          if (!_asic->_reconfig_flushing_phase && _prefetch_process[0].size() < (_agg_fifo_length - times - 1) && !_asic->_task_ctrl->_pending_updates.empty())
          {
            for (int i = 0; i < times; ++i)
            {
              pref_tuple new_tuple = _asic->_task_ctrl->_pending_updates.front();
              int priority = _asic->_task_ctrl->get_cache_hit_aware_priority(_core_id, 0, -1, new_tuple.edge.dst_id, _asic->_remote_coalescer[_core_id]->_batch_width, -1);
              insert_prefetch_process(0, priority, new_tuple);
            }
            _asic->_task_ctrl->_pending_updates.pop();
          }
        }
        else
        {
          (*target_queue).pop_front();
          if (!_asic->_reconfig_flushing_phase && (*target_queue).size() < (_agg_fifo_length - times - 1) && !_asic->_task_ctrl->_pending_updates.empty())
          {
            for (int i = 0; i < times; ++i)
            {
              (*target_queue).push_back(_asic->_task_ctrl->_pending_updates.front());
            }
            _asic->_task_ctrl->_pending_updates.pop();
          }
        }

        if (_asic->_config->_cache_hit_aware_sched)
        {
          // cout << "Input l2hit: " << _asic->_mem_ctrl->_l2hits << " ";
          // Who reduced it? I cannot find it.
          ++_asic->_mem_ctrl->_l2hits;
          ;
          ++_asic->_mem_ctrl->_l2accesses;
          ;
          // cout << "Completed request for dst: " << cur_tuple.edge.dst_id << " and l2 hits: " << _asic->_mem_ctrl->_l2hits << endl;
        }
      }
    }
    else
    {
      assert(go_with_prefetch);
      // push the current tuple into the waiting queue
      _hol_conflict_queue.push(cur_tuple);
      (*target_queue).pop_front();
      if (_asic->_config->_cache_hit_aware_sched)
      {
        // cout << "Input l2hit: " << _asic->_mem_ctrl->_l2hits << " ";
        ++_asic->_mem_ctrl->_l2hits;
        ++_asic->_mem_ctrl->_l2accesses;
        ;
        // cout << "Completed request for dst: " << cur_tuple.edge.dst_id << " and l2 hits: " << _asic->_mem_ctrl->_l2hits << endl;
      }
    }
  }
  else
  {
    _asic->_stats->_stat_mem_stall_thr[_core_id]++;
  }
}

void asic_core::forward_multiply_opn(int lane_id)
{
  auto it = _coarse_cgra_event_queue.begin();
  if (it == _coarse_cgra_event_queue.end())
  {
    return;
  }
  int compl_cycle = it->ready_cycle;
  if (_asic->_cur_cycle < compl_cycle)
  {
    return;
  }
  _asic->_stats->_stat_vec_mat++;
  // cout << "Completed the multiply task for vid: " << (it->vid) << " and the current vec-mat: " << _asic->_stats->_stat_vec_mat  << "\n";
  _coarse_cgra_event_queue.pop_front();
}

// TODO: if second buffer is true, use correct_vertex_data_second_buffer instead. Convey this information
// in cgra_event_queue
void asic_core::forward_atomic_updates(int dfg_id, int lane_id)
{
  // Check if front cycle done matches the cur cycle
  auto it = _cgra_event_queue[dfg_id].begin();
  if (it == _cgra_event_queue[dfg_id].end())
  {
    return;
  }

  int compl_cycle = it->ready_cycle;
  if (_asic->_cur_cycle < compl_cycle)
  {
    return;
  }
  _asic->_stats->_stat_tot_finished_edges += it->thr;

  // cout << "Core: " << _core_id << " cycle: " << _asic->_cur_cycle << " dfg id: " << dfg_id << endl;

  // cout << "Came in to forward atomic updates\n";
  int vid = it->vid; // _pending_stores[lane_id][_cur_cycle_ptr].first;
  // cout << "Came in to forward updates for vid: " << vid << " at cycle: " << _asic->_cur_cycle << " with vertex data: " << _asic->_correct_vertex_data[vid]
  // << " and core: " << _core_id << " with stat finished edges: " << _asic->_stats->_stat_tot_finished_edges << endl;
  assert(vid < _asic->_graph_vertices);

  // for pull mode, when ID switched, or _edges_served==incoming_degree, create coarse_grained_tasks.
  if (_asic->_config->_agg_mult_type != global)
  {
    if (_asic->_config->_pull_mode && !_asic->_task_ctrl->is_local_coarse_grain_task_queue_full(_core_id))
    {
      int inc_degree = _asic->_offset[vid + 1] - _asic->_offset[vid];
      inc_degree *= num_packets;
      assert(_current_edges < inc_degree && "should come in sorted order");
      ++_current_edges;
      if (_current_edges == inc_degree)
      {
        _current_edges = 0;
        _asic->_task_ctrl->insert_local_coarse_grained_task(_core_id, vid, 0, false);
      }
      _asic->_stats->_stat_tot_finished_edges++;
      _cgra_event_queue[dfg_id].pop_front();
      return;
    }
  }

  // TODO: it should not pop if the resulting queue is full
  bool going_to_spawn = (_asic->_correct_vertex_data[vid] == 1);
  if (it->second_buffer)
  {
    if (_asic->_correct_vertex_data_double_buffer[vid] < 0)
    {
      cout << "Received update from second buffer with vid: " << vid << " and value: " << _asic->_correct_vertex_data_double_buffer[vid] << endl;
    }
    // assert(_asic->_correct_vertex_data_double_buffer[vid]>=0);
    going_to_spawn = (_asic->_correct_vertex_data_double_buffer[vid] == 1);
  }
  else
  {
    if (_asic->_correct_vertex_data[vid] < 0)
    {
      cout << "Received update from first buffer with vid: " << vid << " and value: " << _asic->_correct_vertex_data[vid] << endl;
    }
    // assert(_asic->_correct_vertex_data[vid]>=0);
  }
  int dst_core = _asic->_task_ctrl->find_min_coarse_task_core();
  if (going_to_spawn)
  {
    // if(_asic->_correct_vertex_data[vid]==1) {
    // if(!_asic->_task_ctrl->is_local_coarse_grain_task_queue_full(_core_id)) { // cannot push to the task queue
    if (!_asic->_task_ctrl->is_local_coarse_grain_task_queue_full(dst_core))
    {
      _cgra_event_queue[dfg_id].pop_front();
    }
    else
    {
      // at 8th core, this is full
      // cout << "Stalled due to tasks in the coarse-grained task queue at core: " << _core_id << " at cycle: " << _asic->_cur_cycle << "\n";
      return;
    }
  }
  else
  {
    _cgra_event_queue[dfg_id].pop_front();
  }

  _asic->_stats->_stat_correct_edges[_core_id]++;
  _asic->_stats->_stat_pushed_to_ps--;

  _asic->_stats->_stat_tot_finished_edges++;
  _edges_done_this_slice++;

  if (_asic->_config->_agg_mult_type != global)
  {
    // slightly higher number of edges are executed??? with two less vertices...
    // 2 maybe for no outgoing vertex
    // cout << "vid: " << vid << " left: " <<  _asic->_correct_vertex_data[vid] << endl;
    /*if(_asic->_correct_vertex_data[vid]<0) {
      cout << "vid: " << vid << " left: " <<  _asic->_correct_vertex_data[vid] << endl;
    }*/
    // assert(_asic->_correct_vertex_data[vid]>=0);
    if (!it->second_buffer)
    {
      ++_asic->_stats->_stat_tot_finished_first_buffer_edge_tasks;
      _asic->_correct_vertex_data[vid]--; // -ve for matrix-multiplication
      if (_asic->_correct_vertex_data[vid] == 0)
      {
        // _asic->_task_ctrl->insert_coarse_grained_task(vid);
        // cout << "Created matrix multiplication task from the first buffer\n";
        _asic->_task_ctrl->insert_local_coarse_grained_task(_core_id, vid, 0, false);
      }
    }
    else
    {
      ++_asic->_stats->_stat_tot_finished_second_buffer_edge_tasks;
      _asic->_correct_vertex_data_double_buffer[vid]--; // -ve for matrix-multiplication
      if (_asic->_correct_vertex_data_double_buffer[vid] == 0)
      {
        // cout << "Created double buffer tasks for matrix multiply\n";
        _asic->_task_ctrl->insert_local_coarse_grained_task(_core_id, vid, 0, true);
      }
    }

    /*if(_asic->_correct_vertex_data[vid]==-1) {
    _asic->_stats->_stat_vec_mat++;
    _asic->_stats->_stat_tot_finished_edges--;
  }*/
    /*
  assert(_asic->_correct_vertex_data[vid]>=0);
  if(_asic->_correct_vertex_data[vid]==0) { // Let's have a different cgra_event queue
    // cout << "Created a new multiply task for vid: " << vid << "\n";
    _asic->_stats->_stat_pushed_to_ps++;
    // push to cgra_event_queue
    // 1*256 x 256x256 = 1*256
    int comp_time = FEAT_LEN*FEAT_LEN/VEC_LEN; // 16*16/64
    // int ready_cycle = _cur_free_cycle + DFG_LENGTH + comp_time;
    int ready_cycle = _coarse_cur_free_cycle + DFG_LENGTH + comp_time;
    pref_tuple cur_tuple;
    gcn_update new_up(vid, false, cur_tuple, ready_cycle, 0);
    _coarse_cgra_event_queue.push_back(new_up);
    _coarse_cur_free_cycle = ready_cycle - DFG_LENGTH;
    // _cgra_event_queue.push_back(new_up);
    // _cur_free_cycle = ready_cycle - DFG_LENGTH;
  }
  */

    /** I would like to insert this as a new task in another task queue that
     * will schedule this accordingly
     * When executed, its dataflow will be different: dispatch: send memory
     * request for weight matrix and scratch read, process: dot product as they
     * come in, reduce: write back the output. (so my new instances would still
     * be a functions of these). For example, my dispatch for different
     * execution models is different. But multiple task types should be allowed
     * to exist. (task::sgu, task::mult): second is a coarse-grained task, so
     * pushed to memory? OR initialize multiple instances of asic_cores, and
     * call both of them one by one as in like multiple dfgs but they share the
     * same memory controller.
     *
     */
  }

  if (_asic->_config->_algo == cf && it->spawn == true)
  {
    // assert(_asic->_config->_algo==cf && "only cf can spawn tasks");
    // assert(GCN==0);
    _asic->_stats->_stat_online_tasks++;
    // cout << "spawning new task\n";
    task_entry new_task(vid, _asic->_offset[vid]);
    int lane_id = 0;
    DTYPE prio = it->prio;
    _asic->_task_ctrl->insert_new_task(0, lane_id, _asic->_scratch_ctrl->get_local_scratch_id(new_task.vid), prio, new_task);
    _asic->_virtual_finished_counter++;
  }

#if GCN_SWARM == 1 || CF_SWARM == 1
  pref_tuple compl_tuple = _pending_mirror[lane_id][_cur_cycle_ptr];
  assert(lane_id == compl_tuple.lane_id);
  int src_core = _asic->_scratch_ctrl->get_local_scratch_id(compl_tuple.src_id);
  _asic->_asic_cores[src_core]->_waiting_count[lane_id] -= (float)(VEC_LEN / (float)FEAT_LEN1);
  assert(_waiting_count[lane_id] >= 0);
  if (_asic->_asic_cores[src_core]->_waiting_count[lane_id] == 0)
  {
    _asic->_asic_cores[src_core]->_in_order_flag[lane_id] = true;
    _asic->_stats->_stat_tot_tasks_dequed++;
    _asic->_avg_task_time += (_asic->_cur_cycle - _prev_start_time[lane_id]);
  }
#endif

  // cout << "vid: " << vid << " finished\n";
  // if(_pending_stores[lane_id][_cur_cycle_ptr].second==true) {

  // empty pending buffer every cycle
  // _pending_stores[lane_id][_cur_cycle_ptr]=make_pair(-1,false);
}

/*

// --------------------- pull version of CF implementation-------
bool cf::cycle() {
  // Step1: pull the task from queue (number = throughput/32)
  // Step2: read from cache/scratch (can it be single core for basic tests)
  // Step3: Delay until new request is available
  // Step4: check for whole vector to be changed or not? and then spawn new task
  // (actually clarifying this would be good)
}*/

void asic_core::arob_to_pref()
{
  int init_start_point = _cur_arob_pop_ptr;
  for (int i = init_start_point; i < AROB_SIZE; ++i)
  {
    if (!_atomic_rob[i].valid)
      return;
    int lane_id = 0; // _atomic_rob[i].cur_tuple.lane_id;
    // cout << "Found an entry in atomic rob at core: " << _core_id << " and lane: " << lane_id << endl;
    insert_prefetch_process(lane_id, 0, _atomic_rob[i].cur_tuple);
    _cur_arob_pop_ptr++;
    _atomic_rob[i].valid = false;
  }
  _cur_arob_pop_ptr = 0; // if it came here, then it is cycling around
  for (int i = 0; i < init_start_point; ++i)
  {
    if (!_atomic_rob[i].valid)
      return;
    int lane_id = 0; // _atomic_rob[i].cur_tuple.lane_id;
    insert_prefetch_process(lane_id, 0, _atomic_rob[i].cur_tuple);

    _cur_arob_pop_ptr = i + 1;
    _atomic_rob[i].valid = false;
  }
}

void asic_core::issue_requests_from_pending_buffer(int lane_id)
{
  // if request already pending in memory, do nothing
  if (_swarm_pending_request[lane_id])
    return;
  if (_pending_swarm[lane_id].empty())
    return;
  pending_mem_req a = _pending_swarm[lane_id].front();
  _asic->_mem_ctrl->send_mem_request(a.isWrite, a.line_addr, a.cur_tuple, VERTEX_SID);

  _swarm_pending_request[lane_id] = true;
  _pending_swarm[lane_id].pop();
}

void asic_core::fill_task_queue()
{
  while (_asic->_task_ctrl->_remaining_task_entries[_core_id][0] > HIGH_PRIO_RESERVE && !_fifo_task_queue.empty())
  {
    task_entry cur_task = _fifo_task_queue.front();
    _fifo_task_queue.pop();
    _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_task.vid);
    if (_asic->_config->_work_dist != work_steal)
    {
      _asic->_task_ctrl->insert_global_task(0, cur_task);
    }
    else
    {
      // _asic->_task_ctrl->insert_local_task(_agg_push_index, 0, _core_id, cur_task.dist, cur_task);
      _asic->_task_ctrl->insert_local_task(_agg_push_index, 0, _core_id, _asic->_scratch[cur_task.vid], cur_task, true);
      _agg_push_index = (_agg_push_index + 1) % NUM_TQ_PER_CORE;
    }
  }
}

// currently it is only valid with graphmat CF
void asic_core::empty_crossbar_input()
{
  if (!_crossbar_input.empty())
  {
    pref_tuple cur_tuple = _crossbar_input.front();
    int dst_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.edge.dst_id);

    // _asic->_asic_cores[dst_core]->_atomic_rob[cur_tuple.arob_entry].valid=true;
    // _asic->_asic_cores[dst_core]->_atomic_rob[cur_tuple.arob_entry].cur_tuple=cur_tuple;
    _crossbar_input.pop();
  }
}

/*
 * _visited[src_id]=0;
// we would load dest vertices when child would be here
// for(int i=_offset[src_id]; i<_offset[src_id+1]; ++i) _visited[i]=0;
push_task(src_id, -1);

At core i:
if(task_queue_has_entries) {
vid = pop_task();
if(!_visited[vid]) {
	if(_count_current_core==_graph_vertices) {
		evict_and_move_a_vertex(); // need space for vertex or its neighbors too?
	} else {
_visited[vid]=current_core;
}
}
for(int i=_offset[vid]; i<_offset[vid+1]; ++i) push_task(_neighbor[i].dst_id, prio=”inc_freq[dsr_id]”);
// update the frequency of the current vertex wrt its parent that must be in this core if not evicted
Inc_freq[vid]++;
Out_freq[vid] += (_offset[vid+1]-_offset[vid]); // not sure if we should update w/o issuing
} else { // this is work-stealing
  // check all neighboring cores
  for(int c=0; c<_core_cnt; ++c) {
    if(tasks[_mesh_neighbor[current_core][c] > TASK_THRESH) {
        pull_and_push(0.1 * TASK_THRESH tasks);
     }
  }
}
*
 */

// TODO: better to consider task_time = data it accesses
// 1 task per cycle doesn't work in variable sized tasks...
// and hence LB schemes...
void asic_core::automatic_partition_profile()
{
  assert(_asic->_config->_algo != cf && "automatic partition studied only for cf");
  int max_tasks_per_cycle = 2;
  int tasks_issued = 0;
  int edges_served = 0;
  int max_edges_per_cycle = 16;

  // TODO: to move it to take close-to-practical cycles
  // Step1: offset loop should only go until edges served is less than thr
  // Step2: update task's start offset, stealer should not steal this pending task...
  // Step3: pop task only if its entry is done...

  while (tasks_issued < max_tasks_per_cycle && edges_served < max_edges_per_cycle && !_task_queue[0][0].empty())
  {
    auto it = _task_queue[0][0].begin();
    task_entry cur_task = (it->second).front(); //_task_queue[t].front();

    int vid = cur_task.vid;
    // Meta-data: 1. allot it space 2. update frequency
    if (_asic->_map_vertex_to_core[vid] == -1)
    { // Step1: never mapped
      _asic->_map_vertex_to_core[vid] = _core_id;
      // this is equal to _graph_vertices means each vertex is allotted a core once
      // cout << "Cycle: " << _asic->_cur_cycle << " Mapped vid: " << vid << " to core: " << _core_id << endl;
      if (_asic->_stats->_stat_dist_tasks_dequed[_core_id] < _asic->_scratch_ctrl->_slice_size)
      {
        _asic->_stats->_stat_dist_tasks_dequed[_core_id]++;
      }
      else
      { // TODO: need to evict some other data (ie. change its mapping)
        // cout << "cur core: " << _core_id << " tasks here: " << _asic->_stats->_stat_dist_tasks_dequed[_core_id] << " slice: " << _asic->_slice_size << endl;
        _asic->_stats->_stat_ping_pong_vertices++;
        int evict_vid = choose_eviction_cand();
        // TODO: ideally should check nearest cores first and with highest affinity
        int evict_core = get_evict_core(evict_vid);

        _asic->_map_vertex_to_core[evict_vid] = evict_core;
        _asic->_stats->_stat_dist_tasks_dequed[evict_core]++;
        // TODO: change the affinity of all its dependent edges
        update_affinity_on_evict(evict_vid, _core_id, evict_core);

        // cout << "Cycle: " << _asic->_cur_cycle << " Evict full, mapped vid: " << evict_vid
        // << " to core: " << evict_core << endl;
        _prev_core_migrated = evict_core;
      }
    }
    else
    {
      // Stealer needs to move data as well? (though infrequent)
      // Why so? some other source would have pushed there, I somehow need to know if should evict from that core or not
      // assert(_asic->_map_vertex_to_core[vid]==_core_id && "Task needs to pushed along with data");
      int cur_mapped_core = _asic->_map_vertex_to_core[vid];
      if (cur_mapped_core != _core_id)
      {
        // Now, see if we would like to move if its frequency has just increased
        // cout << "cur_core_inc_freq: " << _asic->_inc_freq[_core_id][vid] << " and mapped core: " << _asic->_inc_freq[cur_mapped_core][vid] << endl;
        if (_asic->_inc_freq[_core_id][vid] > _asic->_inc_freq[cur_mapped_core][vid])
        {
          _asic->_map_vertex_to_core[vid] = _core_id;
          _asic->_stats->_stat_dist_tasks_dequed[cur_mapped_core]--;
          // cout << "Cycle: " << _asic->_cur_cycle << " Change, Mapped vid: " << vid << " to core: " << _core_id << endl;
          if (_asic->_stats->_stat_dist_tasks_dequed[_core_id] < _asic->_scratch_ctrl->_slice_size)
          {
            _asic->_stats->_stat_dist_tasks_dequed[_core_id]++;
          }
          else
          { // TODO: need to evict some other data
            int evict_vid = choose_eviction_cand();
            int evict_core = get_evict_core(evict_vid);

            _asic->_map_vertex_to_core[evict_vid] = evict_core;
            _asic->_stats->_stat_dist_tasks_dequed[evict_core]++;
            update_affinity_on_evict(evict_vid, _core_id, evict_core);

            // cout << "Cycle: " << _asic->_cur_cycle << " Evict low freq, mapped vid: " << evict_vid
            // << " to core: " << evict_core << endl;
            _asic->_stats->_stat_low_freq_vertices++;
            _prev_core_migrated = evict_core;

            // TODO: ideally should check nearest cores first
            // round-robin in the nearest cores...
          }
        }
        else
        {
          // cout << "Earlier mapping was better or similar, so we decide not to move!\n";
        }
      }
    }
    int e = _asic->_offset[vid];
    for (e = cur_task.start_offset; e < _asic->_offset[vid + 1] && edges_served < max_edges_per_cycle; ++e)
    {
      edges_served++;
      _asic->_stats->_stat_tot_finished_edges++;
      int dst_id = _asic->_neighbor[e].dst_id;
      DTYPE new_dist = _asic->process(_asic->_neighbor[e].wgt, _asic->_scratch[vid]);
      new_dist = _asic->reduce(_asic->_scratch[dst_id], new_dist);
      bool spawn = _asic->should_spawn_task(_asic->_scratch[dst_id], new_dist);
      _asic->_scratch[dst_id] = new_dist;
      _asic->_inc_freq[_core_id][vid]++;
      _asic->_edge_freq[e]++;
      if (spawn)
      {
        task_entry new_task(dst_id, _asic->_offset[dst_id]);
        int priority = new_dist; // _asic->_inc_freq[_core_id][dst_id];
        _asic->_task_ctrl->insert_new_task(0, 0, _core_id, priority, new_task);
      }
    }
    (it->second).front().start_offset = e;
    if (e == _asic->_offset[vid + 1])
    {
      tasks_issued++;
#if ENTRY_ABORT == 1
      if (!_asic->_config->_pull_mode || _asic->_config->is_sync())
      {
        _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(cur_task.vid);
      }
#endif
      (it->second).pop_front();
      if ((it->second).empty())
      {
        _task_queue[0][0].erase(it->first);
      }
      _asic->_task_ctrl->_remaining_task_entries[_core_id][0]++;
    }
  }
  _asic->_stats->_stat_fine_gran_lb_factor[_core_id] = edges_served;
  _asic->_stats->_stat_tot_tasks_dequed += tasks_issued;
  // TODO: work-stealing should be done on the lowest frequency task (whose edge has never appeared)
  // TODO: also stats from here
  // inc_freq[core_cnt][_graph_vertices];
  // if(0) { // task queue is empty, then work steal?
  if (edges_served == 0)
  { // task queue is empty, then work steal?
    // if(tasks_issued==0) { // task queue is empty, then work steal?
    // most-loaded core (hopefully it won't be 0)
    int steal_core = -1, cur_most_loaded = 0; // rand()%core_cnt;
    for (int c = 0; c < core_cnt; ++c)
    {
      if (c == _core_id)
        continue;
      int num_ready_tasks = TASK_QUEUE_SIZE - _asic->_task_ctrl->_remaining_task_entries[c][0];
      if (num_ready_tasks > cur_most_loaded)
      {
        cur_most_loaded = num_ready_tasks;
        steal_core = c;
      }
    }

    if (steal_core == -1 || _core_id == steal_core)
      return;
    // cout << "Trying to steal from core: " << _core_id << " the victim core: " << steal_core <<
    //  " num ready tasks at victim core: " << cur_most_loaded << endl;
    // cout << "Number of ready tasks: " << num_ready_tasks << endl;
    if (cur_most_loaded >= STEAL_VICTIM_LIM)
    {
      // steal 10 tasks (not working for more than 1: FIXME)
      for (int t = 0; t < STEAL_THIEF_COUNT; ++t)
      {
        // FIXME: Can I skip the first one if it is not ready? (or pending task can also be moved for now I guess?)
        auto it = _asic->_asic_cores[steal_core]->_task_queue[0][0].begin();
        task_entry stolen_task = (it->second).front(); //_task_queue[t].front();
                                                       // FIXME: I am not sure how that affects the other case?
                                                       // stolen_task.start_offset = _asic->_offset[stolen_task.vid];
#if ENTRY_ABORT == 1
        _asic->_task_ctrl->_present_in_local_queue[_core_id].reset(stolen_task.vid);
#endif
        (it->second).pop_front();
        if ((it->second).empty())
        {
          _asic->_asic_cores[steal_core]->_task_queue[0][0].erase(it->first);
        }
        _asic->_work_steal_depth++;
        // stolen_task.stolen=true;
        _asic->_inc_freq[_core_id][stolen_task.vid]--;
        _asic->_inc_freq[steal_core][stolen_task.vid]++;
        // will the priority change?
        // cout << "Stealing task from core: " << steal_core << " to thief: " << _core_id << endl;
        _asic->_task_ctrl->_remaining_task_entries[steal_core][0]++;
        _asic->_task_ctrl->insert_new_task(0, 0, _core_id, stolen_task.dist, stolen_task);
      }
    }
  }
}

// just chose the minimum frequency, if it is the current task? okay!
// TODO: if same freq, chose random instead of always chosing from start...
// hopefully, same vertices ping-pong, at least they do not both bother others...
int asic_core::choose_eviction_cand()
{
  uint64_t min_freq = _asic->_max_iter;
  int min_freq_vid = -1;
  for (int v = 0; v < _asic->_graph_vertices; ++v)
  {
    if (_asic->_map_vertex_to_core[v] == _core_id)
    {
      if (_asic->_inc_freq[_core_id][v] < min_freq)
      {
        min_freq = _asic->_inc_freq[_core_id][v];
        min_freq_vid = v;
      }
    }
  }
  // Should I reduce the inc_freq of all its outgoing vertex as well?
  // cout << "Chosen frequency: " << min_freq << " and vertex: " << min_freq_vid << endl;
  assert(min_freq_vid != -1 && "at least one vertex would be lower otherwise increase inf");
  return min_freq_vid;
}

/*
 * affinity of all its children should be reduced (proportional to how many times that edge was executed).
 * The children’s affinity to the new core should be incremented by the same amount.
 */
// freq_edge[E] -- this is dynamic freq
void asic_core::update_affinity_on_evict(int evict_vid, int old_core, int new_core)
{
  for (int e = _asic->_offset[evict_vid]; e < _asic->_offset[evict_vid + 1]; ++e)
  {
    int dep = _asic->_neighbor[e].dst_id;
    int dep_core = _asic->_map_vertex_to_core[dep];
    // Reduce it by the edge affinity amount
    // Increment it with the remote edge affinity account
    if (dep_core == old_core || dep_core == new_core)
    {
      _asic->_inc_freq[old_core][dep] -= _asic->_edge_freq[e];
      _asic->_inc_freq[new_core][dep] += _asic->_edge_freq[e];
    }
  }
}

// we usually evict it to the highest frequency core
// but what to do if there are a bunch of high-freq?
// if you find new, find if it is closer?
int asic_core::get_evict_core(int evict_vid)
{
  int evict_core = -1;
  int cur_max_freq = -1;
  int cur_min_hops = 2 * num_rows + 1;
  for (int c = 0; c < core_cnt; ++c)
  {
    int test_core = (c + _prev_core_migrated + 1) % core_cnt;
    if (_asic->_stats->_stat_dist_tasks_dequed[test_core] < _asic->_scratch_ctrl->_slice_size)
    {
      if (_asic->_inc_freq[test_core][evict_vid] == cur_max_freq)
      {
        int hop_count = _asic->_network->calc_hops(_core_id, test_core);
        if (hop_count < cur_min_hops)
        {
          evict_core = test_core;
          cur_min_hops = hop_count;
        }
      }
      if (_asic->_inc_freq[test_core][evict_vid] > cur_max_freq)
      {
        cur_max_freq = _asic->_inc_freq[test_core][evict_vid];
        evict_core = test_core;
        cur_min_hops = _asic->_network->calc_hops(_core_id, test_core);
      }
    }
  }
  assert(evict_core != -1 && "Slice ratio should be able to fit all vertices");
  return evict_core;
}

// FIXME: task queue is no accessible by plain distance
// FIXME: doesn't work for pagerank
void asic_core::delete_one_timestamp(int dst_id, int tid, int dst_timestamp)
{
#if SWARM == 1 || ESPRESSO == 1
  // set the current entry to invalid in dependence queue
  auto itd = _dependence_queue.equal_range(dst_id);
  for (auto itdn = itd.first; itdn != itd.second; ++itdn)
  {
    if (itdn->second.first.second == tid)
    {
      itdn->second.first.first = true;
      break;
    }
  }
  auto it2 = _task_queue[0][0].find(dst_timestamp);
  if (it2 == _task_queue[0][0].end())
  {
    _asic->_stats->_stat_not_found_in_task_queue++;
    return;
  }

  _asic->_stats->_stat_executing++;

  bool found = false;
  for (auto it = _task_queue[0][0][dst_timestamp].begin(); it != _task_queue[0][0][dst_timestamp].end();)
  {
    task_entry cur_task = *it;
    if (cur_task.vid == dst_id && cur_task.tid == tid)
    { // to assure we are deleting the right one
      found = true;
      // I don't think I need to delete pointer in the src queue?
      // cout << "dsr id is: " << dst_id << endl;
      it = _task_queue[0][0][dst_timestamp].erase(it); // erase from list
      if (_task_queue[0][0][dst_timestamp].empty())
      {
        // cout << "Erased timestamp: " << dst_timestamp << endl;
        _asic->_stats->_stat_dependent_task_erased++;
        _task_queue[0][0].erase(dst_timestamp); // erase from map
      }
      break;
    }
    else
    {
      ++it;
    }
  }
#endif
}

// FIXME: task queue is no accessible by plain distance
void asic_core::recursively_erase_dependent_tasks(int dst_id, int tid, int dst_timestamp, int rem_depth)
{
#if 0

  // need to find corresponding dst_id; whose invalid flag is not set
  // otherwise return -- should be maximum 1
  vector<forward_info> dep_ptr;
  bool found=false;
  auto itd = _dependence_queue.equal_range(dst_id);
  for(auto itdn=itd.first; itdn!=itd.second; ++itdn) {
    if(itdn->second.first.first==false) {
      if(((tid!=-1) && (itdn->second.first.second==tid)) || (tid==-1)) {
        dep_ptr=itdn->second.second;
        found=true;
        break;
      }
    }
  }
  if(!found) {
    _asic->_stats->_stat_not_found_in_local_dep_queue++;
    return;
  }

  delete_one_timestamp(dst_id, tid, dst_timestamp);
  // cout << "Found in dependence queue and dst_id: " << dst_id << endl;
  /*if(rem_depth!=DEP_CHECK_DEPTH) { // first entry should definitely not be in task queue -- also tid is unknown
    delete_one_timestamp(dst_id, tid, dst_timestamp);
  }*/


  for(int i=0; i<(int)dep_ptr.size(); ++i) {
    // for cycle in graph -- FIXME: check if it does anything or is it ever
    // a case also
    // if(dep_ptr[i].dst_id==dst_id) continue;
    // cout << "Send for vid: " << _dependence_queue[dst_id][i].dst_id << " dst_id: " << dst_id << endl;
    if(rem_depth>1) { // just for recurrence for depth 1
      _asic->_asic_cores[dep_ptr[i].core_id]->recursively_erase_dependent_tasks(dep_ptr[i].dst_id, dep_ptr[i].task_id, dep_ptr[i].dst_timestamp, rem_depth-1);
    } else {
      _asic->_stats->_stat_abort_packets_sent++;
      _asic->push_dummy_packet(_core_id, dep_ptr[i].core_id, false);
      _asic->_asic_cores[dep_ptr[i].core_id]->delete_one_timestamp(dep_ptr[i].dst_id, dep_ptr[i].task_id, dep_ptr[i].dst_timestamp);
    }
  }

  dep_ptr.clear();
#endif
}
