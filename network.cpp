#include "asic_core.hh"
#include "stats.hh"
#include "network.hh"
#include "common.hh"
#include "asic.hh"

network::network(asic *host) : _asic(host)
{

  for (int i = 0; i < core_cnt; ++i)
  {
    _routing_table[i] = (int *)malloc(core_cnt * sizeof(int));
    _link_map[i] = (int *)malloc(core_cnt * sizeof(int));
  }
  _map_dest_buf = (int *)malloc(num_rows * sizeof(int));
  _is_done = (bool *)malloc(MAX_LABEL * sizeof(bool));
  _bus_available = (bool *)malloc(_num_links * 2 * sizeof(bool));

  fill_routing_table();
  fill_link_map();
  for (int i = 0; i < 2 * _num_links; ++i)
  {
    _bus_available[i] = true;
  }
  _map_dest_buf[0] = 2;
  _map_dest_buf[1] = 3;
  // _map_dest_buf.insert(make_pair(0,2));
  // _map_dest_buf.insert(make_pair(1,3));
  for (int i = 0; i < num_rows; ++i)
  {
    _core_net_map[i] = (int *)malloc(num_rows * sizeof(int));
    for (int j = 0; j < num_rows; ++j)
    {
      _core_net_map[i][j] = j + i * 4;
    }
  }

  for (int c = 0; c < core_cnt; ++c)
  {
    assert(_net_in_buffer[c].empty());
    assert(_net_out_buffer[c].empty());
  }
}

// core_cnt*core_cnt
void network::fill_link_map()
{
  int cnt = -1;
  // horizontal links
  for (int i = 0; i < num_rows * num_rows; ++i)
  {
    if ((i + 1) % num_rows == 0)
      continue;
    _link_map[i][i + 1] = ++cnt;
    _link_map[i + 1][i] = cnt;
    // cout << "i: " << i << " j: " << (i+1) << " link_map: " << cnt << endl;
  }

  // vertical links
  for (int i = 0; i < num_rows * (num_rows - 1); ++i)
  {
    _link_map[i][i + num_rows] = ++cnt;
    _link_map[i + num_rows][i] = cnt;
    // cout << "i: " << i << " j: " << (i+num_rows) << " link_map: " << cnt << endl;
  }
  /*for(int i=0; i<core_cnt; ++i) {
    for(int j=0; j<core_cnt; ++j) {
      cout << "i: " << i << " j: " << j << " link_map: " << _link_map[i][j] << endl;
    }
  }*/
}

// X-Y routing
void network::fill_routing_table()
{
  int X, Y, cur_X, cur_Y;

  for (int i = 0; i < core_cnt; ++i)
  {
    for (int j = 0; j < core_cnt; ++j)
    {
      Y = j / num_rows;
      X = j % num_rows;
      cur_Y = i / num_rows;
      cur_X = i % num_rows;

      int x_stride = X > cur_X ? 1 : -1;
      int y_stride = Y > cur_Y ? 1 : -1;
      // route first in the X-direction
      if ((cur_X + 1) % num_rows != 0)
      { // not the boundary cases
        if (X != cur_X)
        {
          _routing_table[i][j] = (cur_X + x_stride) + cur_Y * num_rows;
        }
        else if (Y != cur_Y)
        {
          _routing_table[i][j] = cur_X + (cur_Y + y_stride) * num_rows;
        }
        else
        {
          _routing_table[i][j] = -1; // same location, can check this also
        }
      }
      else
      {
        if (Y != cur_Y)
        {
          _routing_table[i][j] = cur_X + (cur_Y + y_stride) * num_rows;
        }
        else if (X != cur_X)
        {
          _routing_table[i][j] = (cur_X + x_stride) + cur_Y * num_rows;
        }
        else
        {
          _routing_table[i][j] = -1; // same location, can check this also
        }
      }
    }
  }
  /*for(int i=0; i<core_cnt; ++i) {
    for(int j=0; j<core_cnt; ++j) {
      cout << "i: " << i << " j: " << j << " _routing_table: " << _routing_table[i][j] << endl;
    }
  }*/
}

int network::calc_hops(int src, int dest)
{
  int x_disp = 0, y_disp = 0;
  // get Y of both and take mod
  int y1 = src / num_rows;
  int y2 = dest / num_rows;
  y_disp = y1 - y2;
  y_disp = y_disp > 0 ? y_disp : -y_disp;
  // get X of both and take mod
  int x1 = src % num_rows;
  int x2 = dest % num_rows;
  x_disp = x1 - x2;
  x_disp = x_disp > 0 ? x_disp : -x_disp;

  // sum
  assert(x_disp >= 0);
  assert(y_disp >= 0);
  return x_disp + y_disp;
}

// throughput is the matter
// randomized routing: chose according to priority random link whichever is free
// random(goodX, goodY), random(badX, badY)
/*int network::next_destination(int cur_dest, int final_dest) {

}*/

// net_tuple should be vectorized read
// void network::push_to_prefetch_process(int dest_core, pref_tuple cur_tuple) {
void network::push_to_prefetch_process(int dest_core, net_packet cur_tuple, int k)
{
  pref_tuple new_tuple = convert_net_to_pref(cur_tuple, k);
  if (cur_tuple.packet_type == vectorized)
  {
    // TODO: condition to prevent overflow in process, prevent HOL blocking and
    // allow load balance by distributing to other cores...
#if 1
    if (_asic->_asic_cores[dest_core]->can_push_in_prefetch(0))
    {
      _asic->_asic_cores[dest_core]->insert_prefetch_process(0, 0, new_tuple);
    }
    else
    {

      for (int x = (dest_core + 1) % core_cnt, i = 0; i < core_cnt; x = (x + 1) % core_cnt, ++i)
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
        if (cond && _asic->_asic_cores[x]->can_push_in_process(0))
        {
          dest_core = x;
          break;
        }
      }
      _asic->_asic_cores[dest_core]->insert_prefetch_process(0, 0, new_tuple);
    }
#else
    _asic->_asic_cores[dest_core]->_prefetch_process[0].push_back(new_tuple);
#endif
  }
  else
  {
    assert(k != -1 && "for multicast, k has to be an index in mcast dst");
    assert(cur_tuple.packet_type == vectorizedRead && "only other entry for gcn is vector read");
    dest_core = cur_tuple.multicast_dst_core_id[k];
    int dfg_id = cur_tuple.multicast_dfg_id[k];
    net_packet temp = convert_pref_to_net(dest_core, dfg_id, new_tuple);
    temp.cb_entry = temp.dst_id;
    _asic->_scratch_ctrl->_net_data.push(temp);
  }
}

int network::get_core_id(int int_link_id)
{
  int num_horizontal = num_rows * (num_rows - 1);
  return int_link_id % num_horizontal;
}

int network::get_link_dest_id(int int_link_id)
{
  bool horizontal_link = false;
  int num_horizontal = num_rows * (num_rows - 1);
  if (int_link_id < num_horizontal)
    horizontal_link = true;

  if (horizontal_link)
  {
    return get_core_id(int_link_id) + 1;
  }
  else
  {
    return get_core_id(int_link_id) + num_rows;
  }
}

void network::update_pops_acc_to_bandwidth(net_packet cur_tuple, int &num_pops)
{
  if (!is_remote_read(cur_tuple))
  { // simple req
    if (cur_tuple.packet_type == vectorized || cur_tuple.packet_type == vectorizedRead)
    {
      int reqd_size = cur_tuple.multicast_dst_core_id.size();
      int bw = min(reqd_size, _bus_vec_width);
      num_pops += bw;
      _stat_tot_bw_utilization += bw;
      // num_pops+=_bus_vec_width;
      // _stat_tot_bw_utilization+=bus_width; // assuming it takes whole
    }
    else
    {
      num_pops++;
      _stat_tot_bw_utilization += 4;
    }
  }
  else
  {                                // mem req
    num_pops++;                    // =4;
    _stat_tot_bw_utilization += 4; // *4*2;
  }
}

bool network::can_push_in_network(int core_id)
{
  return _net_in_buffer[core_id].size() < FIFO_DEP_LEN;
  // return (_active_net_packets<500000) && (_net_in_buffer[core_id].size() < FIFO_DEP_LEN);
}

bool network::can_push_in_internal_buffer(int x, int y)
{
  return _internal_buffers[x][y].size() < FIFO_DEP_LEN;
}

void network::decode_real_multicast(net_packet cur_tuple, int core_id)
{
  for (int k = 0; k < cur_tuple.multicast_dst_core_id.size(); ++k)
  {
    int dest_core = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.multicast_dst_wgt[k].first);
    // FIXME: it should not go to non-agg-cores
    // cout << "k: " << k << " dst_id: " << cur_tuple.multicast_dst[k] << " dest_core: " << cur_tuple.multicast_dst_core_id[k] << " calculated dest core: " << dest_core << endl;
    if (_asic->_config->_perfect_net == 0 && cur_tuple.packet_type != vectorizedRead)
    {
      assert((_asic->_config->_heter_cores == 0 || dest_core == cur_tuple.multicast_dst_core_id[k]) && "some issue, matching doesn't work");
    }
    dest_core = cur_tuple.multicast_dst_core_id[k];
    if (_asic->_config->is_vector())
    {
      pref_tuple return_to_arob = convert_net_to_pref(cur_tuple, k);
      if (_asic->_config->_graphmat_slicing)
      {
        int dest_slice = _asic->_scratch_ctrl->get_slice_num(return_to_arob.edge.dst_id);
        if (_asic->_current_slice != dest_slice && !_asic->_finishing_phase)
        {
          _asic->_delta_updates[_asic->_current_reuse].push_back(make_pair(return_to_arob.edge.dst_id, -1));
        }
        else
        {
          push_to_prefetch_process(dest_core, cur_tuple, k);
        }
      }
      else
      {
        // cout << "Pushing a new data for dst: " << return_to_arob.edge.dst_id << endl;
        // push_to_prefetch_process(dest_core, return_to_arob);
        push_to_prefetch_process(dest_core, cur_tuple, k);
      }
    }
    else
    { // for other case when we need to push to bank queues
      red_tuple return_to_bank_queue = convert_net_to_red(cur_tuple, k);

      // Is the output derived from multicast? (yes is there a one-to-one
      // mapping b/w multicast_dst and multicast_dest_core_id)
      int dest_core = _asic->_scratch_ctrl->get_local_scratch_id(return_to_bank_queue.dst_id);
      // assert(dest_core==cur_tuple.multicast_dst_core_id[k]);
      int bank_id = _asic->_scratch_ctrl->get_local_bank_id(cur_tuple.multicast_dst_wgt[k].first);
      _asic->execute_func(-1, BANK_TO_XBAR, return_to_bank_queue);
    }
  }
}

void network::decode_path_multicast(net_packet cur_tuple, int core_id)
{
  // just put selected entries and push a new packet
  net_packet remote_tuple = cur_tuple;

  remote_tuple.multicast_dst_core_id.clear();
  remote_tuple.multicast_dst_wgt.clear();
  remote_tuple.multicast_dfg_id.clear();
  int min_dist = num_rows * 2;
  for (int k = 0; k < cur_tuple.multicast_dst_core_id.size(); ++k)
  {
    // drop packets for the current core
    if (cur_tuple.multicast_dst_core_id[k] == core_id)
    {
      // pref_tuple return_to_arob = convert_net_to_pref(cur_tuple, k);
      // push_to_prefetch_process(core_id, return_to_arob);
      push_to_prefetch_process(core_id, cur_tuple, k);
    }
    else
    { // create entry into the remote tuple, while finding the min_dist
      int cur_dist = _routing_table[core_id][cur_tuple.multicast_dst_core_id[k]];
      remote_tuple.multicast_dst_core_id.push_back(cur_tuple.multicast_dst_core_id[k]);
      remote_tuple.multicast_dst_wgt.push_back(cur_tuple.multicast_dst_wgt[k]);
      remote_tuple.multicast_dfg_id.push_back(cur_tuple.multicast_dfg_id[k]);
      if (cur_dist < min_dist)
      {
        min_dist = cur_dist;
      }
    }
  }
  if (remote_tuple.multicast_dst_core_id.size() > 0)
  {
    encode_scalar_packet(remote_tuple, core_id);
  }
}

void network::decode_scalar_packet(net_packet cur_tuple, int core_id)
{
#if SGU_GCN_REORDER == 1
  for (int k = 0; k < cur_tuple.multicast_dst_core_id.size(); ++k)
  {
    pref_tuple return_to_arob = convert_net_to_pref(cur_tuple, k);
    int dest_core = _asic->_scratch_ctrl->get_local_scratch_id(return_to_arob.edge.dst_id);
    // push_to_prefetch_process(dest_core, return_to_arob);
    push_to_prefetch_process(dest_core, cur_tuple, k);
  }
#else
  // pref_tuple return_to_arob = convert_net_to_pref(cur_tuple, -1);
  // push_to_prefetch_process(core_id, return_to_arob);
  push_to_prefetch_process(core_id, cur_tuple, -1);
  // cout << "pushed into current arob entry: " << cur_entry << endl;
#endif
}

void network::encode_real_multicast(net_packet cur_tuple, int core_id)
{
  // _stat_remote_updates+=cur_tuple.multicast_dst_core_id.size();

  // combine the states of all dst (3: same core, greater, less)
  vector<int> sorted_dest[3];
  vector<pair<iPair, int>> sorted_info[3];

  for (unsigned k = 0; k < cur_tuple.multicast_dst_core_id.size(); ++k)
  {
    int state = -1;
    int cur_dst = cur_tuple.multicast_dst_core_id[k];
    if (cur_dst == core_id)
      state = 0;
    else
    {
      int next_dest = _routing_table[core_id][cur_dst];
      assert(core_id != next_dest);
      if (next_dest > core_id)
        state = 1;
      else
        state = 2;
    }
    sorted_dest[state].push_back(cur_dst);
    if (cur_tuple.multicast_dfg_id.size() > 0)
    {
      sorted_info[state].push_back(make_pair(cur_tuple.multicast_dst_wgt[k], cur_tuple.multicast_dfg_id[k]));
    }
    else
    {
      sorted_info[state].push_back(make_pair(cur_tuple.multicast_dst_wgt[k], -1));
    }
  }

  // cout << "s=1: " << sorted_dest[1].size() << " s=2: " << sorted_dest[2].size() << endl;
  // run over all states
  for (int s = 0; s < 3; ++s)
  {
    if (sorted_dest[s].size() == 0)
      continue;
    // assign new label to each created packet
    cur_tuple.label = _cur_packet_label;
    _cur_packet_label = (_cur_packet_label + 1 < MAX_LABEL) ? (_cur_packet_label + 1) : 0;

    cur_tuple.multicast_dst_wgt.clear();
    cur_tuple.multicast_dfg_id.clear();
    cur_tuple.multicast_dst_core_id.clear();
    for (unsigned k = 0; k < sorted_dest[s].size(); ++k)
    {
      cur_tuple.multicast_dst_core_id.push_back(sorted_dest[s][k]);
      cur_tuple.multicast_dst_wgt.push_back(sorted_info[s][k].first);
      cur_tuple.multicast_dfg_id.push_back(sorted_info[s][k].second);
    }

    int next_dest = _routing_table[core_id][sorted_dest[s][0]];
    int link_id = _link_map[core_id][next_dest];

    if (s == 0)
    {
      // this is local
      _stat_local_updates += sorted_dest[s].size();
      _net_out_buffer[core_id].push(cur_tuple);
    }
    else if (s == 1)
    {
      // cout << "cur dest: " << sorted_dest[s][0] << " next core: " << core_id << endl;
      _stat_remote_updates += sorted_dest[s].size();
      assert(sorted_dest[s][0] != core_id);
      _internal_buffers[link_id][0].push(cur_tuple);
    }
    else
    {
      _stat_remote_updates += sorted_dest[s].size();
      assert(sorted_dest[s][0] != core_id);
      _internal_buffers[link_id][3].push(cur_tuple);
    }
  }
  for (int s = 0; s < 3; ++s)
  {
    sorted_dest[s].clear();
    sorted_info[s].clear();
  }
}

void network::encode_path_multicast(net_packet cur_tuple, int core_id)
{
  // find closest destination
  net_packet local_tuple = cur_tuple;
  net_packet remote_tuple = cur_tuple;
  local_tuple.multicast_dst_core_id.clear();
  local_tuple.multicast_dst_wgt.clear();
  local_tuple.multicast_dfg_id.clear();
  remote_tuple.multicast_dst_core_id.clear();
  remote_tuple.multicast_dst_wgt.clear();
  remote_tuple.multicast_dfg_id.clear();

  int min_dist = 2 * num_rows;
  int min_dist_core = -1;
  for (unsigned k = 0; k < cur_tuple.multicast_dst_core_id.size(); ++k)
  {
    int cur_dist = _routing_table[core_id][cur_tuple.multicast_dst_core_id[k]];
    if (cur_dist == -1)
    {
      local_tuple.multicast_dst_core_id.push_back(cur_tuple.multicast_dst_core_id[k]);
      local_tuple.multicast_dst_wgt.push_back(cur_tuple.multicast_dst_wgt[k]);
      local_tuple.multicast_dfg_id.push_back(cur_tuple.multicast_dfg_id[k]);
    }
    else
    {
      remote_tuple.multicast_dst_core_id.push_back(cur_tuple.multicast_dst_core_id[k]);
      remote_tuple.multicast_dst_wgt.push_back(cur_tuple.multicast_dst_wgt[k]);
      remote_tuple.multicast_dfg_id.push_back(cur_tuple.multicast_dfg_id[k]);
      if (cur_dist < min_dist)
      {
        min_dist = cur_dist;
        min_dist_core = cur_tuple.multicast_dst_core_id[k];
        // do i want to split into 2 here?
      }
    }
  }
  remote_tuple.dest_core_id = min_dist_core;
  if (local_tuple.multicast_dst_core_id.size() > 0)
  {
    _net_out_buffer[core_id].push(local_tuple);
  }

  _stat_remote_updates += remote_tuple.multicast_dst_core_id.size();
  encode_scalar_packet(remote_tuple, min_dist_core);
}

void network::encode_scalar_packet(net_packet remote_tuple, int core_id)
{
  int min_dist_core = remote_tuple.dest_core_id;

  int next_dest = _routing_table[core_id][min_dist_core];
  assert(core_id != next_dest); // next destination should be different

  remote_tuple.label = _cur_packet_label;
  remote_tuple.push_cycle = 0; // _asic->_cur_cycle;
  _cur_packet_label = (_cur_packet_label + 1 < MAX_LABEL) ? (_cur_packet_label + 1) : 0;

  int link_id = _link_map[core_id][next_dest];
  if (next_dest > core_id)
  {
    _internal_buffers[link_id][0].push(remote_tuple);
  }
  else
  {
    _internal_buffers[link_id][3].push(remote_tuple);
  }
}

pref_tuple network::convert_net_to_pref(net_packet packet, int k)
{
  pref_tuple return_to_arob;
  return_to_arob.src_id = packet.src_id;
  return_to_arob.second_buffer = packet.second_buffer;
  if (k == -1)
  {
    return_to_arob.edge.dst_id = packet.dst_id;
  }
  else
  {
    return_to_arob.edge.dst_id = packet.multicast_dst_wgt[k].first;
    return_to_arob.edge.wgt = packet.multicast_dst_wgt[k].second;
  }
  return_to_arob.update = (packet.packet_type == vectorized);
  // return_to_arob.cb_entry = packet.multicast_dst[k];
  // not sure what it is used for...!!!
  return_to_arob.update_width = bus_width;            // packet.arob_entry; // cur_tuple.update_width;
  return_to_arob.end_edge_served = packet.push_cycle; // FIXME: not sure if it is used here
  // cout << "In network, copy the end edge served is: " << return_to_arob.end_edge_served << endl;
  if (_asic->_config->_cf == 1 && _asic->_config->_algo == gcn)
  {
    for (int f = 0; f < FEAT_LEN; ++f)
      return_to_arob.vertex_data[f] = packet.vertex_data[f];
  }
  // cout << "Finally pushing for src_id: " << return_to_arob.src_id << " dst_id: " << return_to_arob.edge.dst_id << endl;
  return return_to_arob;
}

net_packet network::convert_pref_to_net(int req_core, int dfg_id, pref_tuple cur_tuple)
{
  net_packet packet;
  packet.src_id = cur_tuple.src_id;
  packet.dst_id = cur_tuple.edge.dst_id;
  packet.packet_type = vectorizedRead;
  packet.req_core_id = req_core;
  packet.dfg_id = dfg_id;
  return packet;
}

red_tuple network::convert_net_to_red(net_packet packet, int k)
{
  red_tuple return_to_bank_queue;
  if (k == -1)
  {
    return_to_bank_queue.dst_id = packet.dst_id;
  }
  else
  {
    return_to_bank_queue.dst_id = packet.multicast_dst_wgt[k].first;
    // return_to_bank_queue.wgt = packet.multicast_dst_wgt[k].second;
  }
  return_to_bank_queue.new_dist = packet.new_dist;
  return_to_bank_queue.src_id = packet.src_id;
  return return_to_bank_queue;
}

void network::push_net_packet(int core_id, net_packet cur_tuple)
{
  assert(core_id < core_cnt && "cannot push to a buffer outside network");
  // assert(_net_in_buffer[core_id].size()<FIFO_DEP_LEN && "so that queues do not overflow");

  _net_in_buffer[core_id].push(cur_tuple);
  // cout << "Pushed a packet to core: " << core_id << " new entries: " << _net_in_buffer[core_id].size() << " at cycle: " << _asic->_cur_cycle << endl;
}

net_packet network::create_dummy_request_packet(int src_id, int dst_id, int req_core)
{
  int cb_entry = -1;
  bool second_buffer = false;

  net_packet packet;
  packet.packet_type = dummy;
  packet.req_core_id = req_core;
  packet.dst_id = dst_id;
  packet.dest_core_id = (dst_id / (32 * 16)) % core_cnt;

  packet.cb_entry = cb_entry;
  packet.src_id = src_id;
  packet.second_buffer = second_buffer;
  for (int i = 0; i < FEAT_LEN; ++i)
  {
    packet.vertex_data[i] = 0;
  }
  // packet.tid = tid;
  return packet;
}

// for reading sources
net_packet network::create_scalar_request_packet(int src_id, int dst_id, int req_core, int cb_entry, bool second_buffer = false)
{
  net_packet packet;
  packet.packet_type = scalarRequest;
  packet.src_id = src_id;
  packet.dst_id = dst_id;
  packet.second_buffer = second_buffer;
  packet.req_core_id = req_core;
  packet.cb_entry = cb_entry;
  packet.dst_id = dst_id;
  packet.dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id(dst_id, false);
  // packet.tid = tid;
  return packet;
}

// for updating destination?
net_packet network::create_noopt_scalar_update_packet(int src_id, int dst_id, DTYPE new_dist)
{
  net_packet packet;
  packet.packet_type = scalarUpdate;
  packet.src_id = src_id;
  packet.dst_id = dst_id;
  packet.req_core_id = -1;
  packet.cb_entry = -1;
  packet.new_dist = new_dist;
  packet.dst_id = dst_id;
  packet.dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id(dst_id);
  // packet.tid = tid;
  return packet;
}

net_packet network::create_scalar_update_packet(int tid, int src_id, vector<iPair> multicast_dst_wgt, DTYPE new_dist)
{
  net_packet packet;
  packet.packet_type = scalarUpdate;
  packet.src_id = -1;
  packet.dst_id = -1;
  packet.req_core_id = -1;
  packet.cb_entry = -1;
  packet.new_dist = new_dist;
  packet.dst_id = -1;
  packet.dest_core_id = -1;
  packet.multicast_dst_wgt = multicast_dst_wgt;
  // Should this be only not done??
  for (unsigned d = 0; d < multicast_dst_wgt.size(); ++d)
  {
    packet.multicast_dst_core_id.push_back(_asic->_scratch_ctrl->get_local_scratch_id(multicast_dst_wgt[d].first));
    packet.multicast_dfg_id.push_back(0);
  }
  packet.tid = tid;
  return packet;
}

// for updating vector destination?
net_packet network::create_noopt_vector_update_packet(int src_id, int dst_id, DTYPE vertex_data[FEAT_LEN])
{
  net_packet packet;
  packet.packet_type = vectorized;
  packet.src_id = src_id;
  packet.req_core_id = -1;
  packet.cb_entry = -1;
  packet.dst_id = dst_id;
  packet.dest_core_id = _asic->_scratch_ctrl->get_local_scratch_id(dst_id);
  for (int i = 0; i < FEAT_LEN; ++i)
  {
    packet.vertex_data[i] = vertex_data[i];
  }
  return packet;
}

// for updating vector destination?
net_packet network::create_vector_update_packet(int tid, int src_id, vector<iPair> multicast_dst_wgt, DTYPE vertex_data[FEAT_LEN])
{
  net_packet packet; // is this initialized already?
  packet.packet_type = vectorized;
  packet.src_id = src_id;
  packet.req_core_id = -1;
  packet.cb_entry = -1;
  packet.dst_id = -1;
  packet.dest_core_id = -1;
  for (int i = 0; i < FEAT_LEN; ++i)
  {
    packet.vertex_data[i] = vertex_data[i];
  }
  packet.multicast_dst_wgt = multicast_dst_wgt;
  for (unsigned d = 0; d < multicast_dst_wgt.size(); ++d)
  {
    int dst = -1;
    if (multicast_dst_wgt[d].first < V)
    {
      dst = _asic->_scratch_ctrl->get_local_scratch_id(multicast_dst_wgt[d].first);
    }
    packet.multicast_dst_core_id.push_back(dst);
    packet.multicast_dfg_id.push_back(0);
  }
  packet.tid = tid;
  return packet;
}

bool network::is_remote_read(net_packet cur_tuple)
{
  return (cur_tuple.packet_type == scalarRequest || cur_tuple.packet_type == scalarResponse);
}

void network::pull_packets_to_network(int core_id)
{

  int num_pops = 0;
  while (!_net_in_buffer[core_id].empty() && num_pops < process_thr)
  {
    // assert(_net_in_buffer[core_id].size()<FIFO_DEP_LEN*4 && "net in buffer size should not be garbage");
    num_pops++;
    _num_outstanding_packets++;
    _stat_tot_packets_transferred++;
    // net_packet cur_tuple = _net_in_buffer[core_id].front();
    net_packet cur_tuple(_net_in_buffer[core_id].front());
    _net_in_buffer[core_id].pop();
    ++_active_net_packets;

    int dest_core_id = cur_tuple.dest_core_id;
    bool real_multicast = _asic->_config->_real_multicast;
    bool path_multicast = _asic->_config->_path_multicast;

    // cout << "Cycle: " << _asic->_cur_cycle << " src_id: " << cur_tuple.src_id << " dest core: " << dest_core_id << " core: "<< core_id << endl;
    if (real_multicast)
    {
      assert((is_remote_read(cur_tuple) || cur_tuple.dest_core_id == -1 || cur_tuple.packet_type == dummy) && "we do not use this parameter here");
    }
    if (dest_core_id == core_id)
    { // no need to push to network
      _stat_local_updates++;
      if (cur_tuple.packet_type == scalarRequest)
      {
        _mc_buffers[core_id].push(cur_tuple);
      }
      else
      {
        _net_out_buffer[core_id].push(cur_tuple);
      }
    }
    else
    {
      if (dest_core_id != -1)
        _stat_packets_to_router[dest_core_id]++;
      // if(!is_remote_read(cur_tuple)  && _asic->_config->_net_traffic==real_multicast) {
      if (real_multicast && !is_remote_read(cur_tuple))
      {
        encode_real_multicast(cur_tuple, core_id);
      }
      else if (path_multicast && !is_remote_read(cur_tuple))
      {
        // } else if(!is_remote_read(cur_tuple) && _asic->_config->_net_traffic==path_multicast) {
        encode_path_multicast(cur_tuple, core_id);
      }
      else
      {
        // Maybe this is the only request...
        // cout << "Input 2 Request core id: " << cur_tuple.req_core_id << " and the cb buf entry: " << cur_tuple.cb_entry  << " in cycle: " << _asic->_cur_cycle << endl;
        if (cur_tuple.packet_type == scalarResponse)
        {
          // cout << "Scalar response packet at net_in with src_id: " << cur_tuple.src_id << endl;
        }
        _stat_remote_updates++;

        encode_scalar_packet(cur_tuple, core_id);
      }
    }
    // cout << "A packet submitted inside the packet\n";
  }
}

void network::push_packets_from_network(int core_id)
{

  int num_pops = 0;
  // while(!_net_out_buffer[core_id].empty() && num_pops<_bus_vec_width*2) {
  while (!_net_out_buffer[core_id].empty() && num_pops < process_thr)
  {
    // cout << "Popped a packet at cycle: " << _asic->_cur_cycle << endl;
    num_pops++;
    _num_outstanding_packets--;
    net_packet cur_tuple(_net_out_buffer[core_id].front());
    _net_out_buffer[core_id].pop();
    --_active_net_packets;
    // TODO: push in corresponding mc buffer
    // not a read request...
    if (cur_tuple.req_core_id != -1)
    {
      if (!cur_tuple.packet_type != astarUpdate)
      {
        // int mc_ind=_asic->_scratch_ctrl->get_mc_core_id(cur_tuple.dst_id);
        int mc_ind = _asic->_scratch_ctrl->get_local_scratch_id(cur_tuple.dst_id, false);
        _mc_buffers[mc_ind].push(cur_tuple);
        // if coarse request
        if (cur_tuple.packet_type == vectorized)
        {
          assert(!_asic->_coarse_reorder_buf[cur_tuple.req_core_id][cur_tuple.dfg_id]->_reorder_buf[cur_tuple.cb_entry].valid && "coarse CB entry correct just before pushing to mc buffers");
          assert(_asic->_coarse_reorder_buf[cur_tuple.req_core_id][cur_tuple.dfg_id]->_reorder_buf[cur_tuple.cb_entry].waiting_addr > 0 && "coarse CB entry correct just before pushing to mc buffers");
        }
      }
      else
      {
        int latency_over_network = _asic->_cur_cycle - cur_tuple.push_cycle;
        // TODO: check optimal number of hops -- I do not have req core id
        // if(latency_over_network>(core_cnt*2)) _asic->_stats->_stat_took_more_time++;
        // TODO: push it into the bank queues: replicated code (fix later)
        int bank_id = _asic->_scratch_ctrl->get_local_bank_id(cur_tuple.dst_id);
        int dest_core_id = cur_tuple.dest_core_id;
        assert(dest_core_id == core_id);
        // cout << "FINAL DESTINATION PUSH IN CORE: " << dest_core_id << endl;
        assert(!_asic->_config->is_vector() && "not a gcn/cf packet, what is it doing here\n");
        red_tuple update_packet = convert_net_to_red(cur_tuple, -1); // .new_dist, cur_tuple.dst_id);
        _asic->execute_func(-1, BANK_TO_XBAR, update_packet);
      }
    }
    else
    {
      // TODO: check using a flag -- case of a remote read
      if (cur_tuple.packet_type == scalarResponse)
      {
        cur_tuple.req_core_id = cur_tuple.dest_core_id; // meaning current core
        // cout << "Pushing to net data with src_id: " << cur_tuple.src_id << endl;
        _asic->_scratch_ctrl->_net_data.push(cur_tuple);
        continue;
      }
      if (cur_tuple.packet_type != dummy)
      { // there should be my output
        if (_asic->_config->_real_multicast == 0 && _asic->_config->_path_multicast == 0)
        {
          int bank_id = _asic->_scratch_ctrl->get_local_bank_id(cur_tuple.dst_id);
          int dest_core_id = cur_tuple.dest_core_id;
          assert(dest_core_id == core_id);
        }

        if (cur_tuple.packet_type == vectorized || cur_tuple.packet_type == vectorizedRead)
        {
          if (_asic->_config->_real_multicast == 1)
          {
            decode_real_multicast(cur_tuple, core_id);
          }
          else if (_asic->_config->_path_multicast == 1)
          {
            decode_path_multicast(cur_tuple, core_id);
          }
          else
          {
            decode_scalar_packet(cur_tuple, core_id);
          }
        }
        else
        {
          assert(!_asic->_config->is_vector() && "not a gcn/cf packet, what is it doing here\n");
          if (_asic->_config->_real_multicast == 0 && _asic->_config->_path_multicast == 0)
          {
            _asic->execute_func(-1, BANK_TO_XBAR, convert_net_to_red(cur_tuple, -1));
          }
        }

#if SWARM == 1 || TESSERACT == 1
        _asic->_asic_cores[core_id]->_active_process_thr++;
#endif
      }
      else
      {
        // cout << "PREFETCH REQUEST SERVED! with label: " << cur_tuple.label << "\n";
      }
    }
    // cout << "Popped a packet to core: " << core_id << " new entries: " << _net_out_buffer[core_id].size() << " at cycle: " << _asic->_cur_cycle << endl;
  }
}

// function of each router
// TODO: for remote read requests, push the data to net data...
void network::external_routing(int core_id)
{
  pull_packets_to_network(core_id);
  push_packets_from_network(core_id);
}

void network::transfer_scalar_packet(net_packet cur_tuple, int cur_core_id, int i, int j, int &num_pops)
{
  // cout << "DURING INTERNAL ROUTING, cb entry: " << cur_tuple.cb_entry << endl;
  int next_dest = -1;
  int dest_core_id = cur_tuple.dest_core_id;
  next_dest = _routing_table[cur_core_id][dest_core_id];
  assert(next_dest != -1);
  if (get_link_dest_id(i) == next_dest)
  { // moving over the link
    // assert(j<2);
    if (_bus_available[i])
    { // good to go (cur_link is available)

      // num_pops++;
      // _stat_tot_bw_utilization+=4;

      update_pops_acc_to_bandwidth(cur_tuple, num_pops);
      _internal_buffers[i][_map_dest_buf[j]].push(cur_tuple);
      _internal_buffers[i][j].pop();
    }
    else
    {
      return;
    }
  }
  else
  { // need to switch links in same router, find new i and j
    // is it overestimation if it do 2 things for the same packet at the same time?
    // 4 options: +X, -X, +Y, -Y
    // i+1,0..2 ; i-1, 1..3 ; i+num_rows-1,0..2 ; i-num_rows+1,1..3
    // TODO: add a condition everywhere that the destination buffer is
    // not done for the cycle
    int dest1 = -1, dest2 = -1;
    if (next_dest == cur_core_id + 1)
    {
      dest1 = i + 1;
      dest2 = 0;
    }
    else if (next_dest == cur_core_id - 1)
    {
      dest1 = i - 1;
      dest2 = 3;
    }
    else if (next_dest == cur_core_id + num_rows)
    {
      dest1 = i + num_rows;
      dest2 = 0;
    }
    else if (next_dest == cur_core_id - num_rows)
    {
      dest1 = i - num_rows;
      dest2 = 3;
    }
    else
    {
      cout << "ERROR: next destination cannot be more than 1 hop\n";
      cout << "cur_core_id: " << cur_core_id << " next_dest: " << next_dest << " final dest core id: " << dest_core_id << endl;
      exit(0);
    }
#if WORST_NET == 1
    if (!_can_push_to_out[dest1][dest2]) // break; // no?
      meta.push(cur_tuple);
    else
      _internal_buffers[dest1][dest2].push(cur_tuple);
    dest_accessed.push(make_pair(dest1, dest2));
    // cout << "Pushed: " <<  dest1 << " " << dest2 << endl;
#else
    _internal_buffers[dest1][dest2].push(cur_tuple);
#endif
    _internal_buffers[i][j].pop();
  }
}

void network::transfer_multicast_packet(net_packet cur_tuple, int cur_core_id, int i, int j, int &num_pops)
{
  int next_dest = -1;
  vector<int> sorted_dest[6]; // 5 possible states
  vector<pair<iPair, int>> sorted_info[6];
  int dest1 = -1, dest2 = -1;

  // Step1: Split destinations according to its directions
  for (int d = 0; d < cur_tuple.multicast_dst_core_id.size(); ++d)
  {
    int cur_dst_core = cur_tuple.multicast_dst_core_id[d];
    next_dest = _routing_table[cur_core_id][cur_dst_core];
    int state = -1;
    // cout << "Current dst: " << cur_dst_core << " next dest: " << next_dest << "cur core id: " << cur_core_id << endl;
    if (cur_dst_core == cur_core_id)
      state = 0;
    else if (next_dest == get_link_dest_id(i))
      state = 1;
    else if (next_dest == cur_core_id + 1)
      state = 2;
    else if (next_dest == cur_core_id - 1)
      state = 3;
    else if (next_dest == cur_core_id + num_rows)
      state = 4;
    else if (next_dest == cur_core_id - num_rows)
      state = 5;
    else
      cout << "Invalid option in internal core\n";

    sorted_dest[state].push_back(cur_dst_core);
    sorted_info[state].push_back(make_pair(cur_tuple.multicast_dst_wgt[d], cur_tuple.multicast_dfg_id[d])); // cur_tuple.multicast_arob_entries[d]));
  }

  // Step2: for each state, it should do exactly the given code
  for (int s = 0; s < 6; ++s)
  {
    if (sorted_dest[s].size() == 0)
      continue;
    // FIXME: if return, then these should be retained
    cur_tuple.multicast_dst_wgt.clear();
    cur_tuple.multicast_dst_core_id.clear();
    cur_tuple.multicast_dfg_id.clear();
    // cur_tuple.multicast_arob_entries.clear();
    for (unsigned k = 0; k < sorted_dest[s].size(); ++k)
    {
      cur_tuple.multicast_dst_core_id.push_back(sorted_dest[s][k]);
      cur_tuple.multicast_dst_wgt.push_back(sorted_info[s][k].first);
      cur_tuple.multicast_dfg_id.push_back(sorted_info[s][k].second);
    }
    dest1 = -1;
    dest2 = -1;
    if (s == 0)
    { // copy to the same core
      _net_out_buffer[sorted_dest[s][0]].push(cur_tuple);
    }
    else if (s == 1)
    {
      if (_bus_available[s])
      { // good to go (cur_link is available)
        // num_pops++;
        // _stat_tot_bw_utilization+=4;

        update_pops_acc_to_bandwidth(cur_tuple, num_pops);
        _internal_buffers[i][_map_dest_buf[j]].push(cur_tuple);
      }
      else
      {
        cout << "IT RETURNED\n";
        return; // slightly different...
      }
    }
    else if (s == 2)
    {
      dest1 = i + 1;
      dest2 = 0;
    }
    else if (s == 3)
    {
      dest1 = i - 1;
      dest2 = 3;
    }
    else if (s == 4)
    {
      dest1 = i + num_rows;
      dest2 = 0;
    }
    else
    {
      dest1 = i - num_rows;
      dest2 = 3;
    }

    if (s > 1)
    {
      // cout << " dest1: " << dest1 << " dest2: " << dest2 << " size: " << _internal_buffers[dest1][dest2].size() << endl;
      _internal_buffers[dest1][dest2].push(cur_tuple);
    }
  }
  _internal_buffers[i][j].pop();
  for (int k = 0; k < 5; ++k)
  {
    sorted_dest[k].clear();
    sorted_info[k].clear();
  }
}

// for all internal buffers: route to some other internal router
// consider bus arbitration here
// or if cur core is the destination, push to the destination buffer
// for all internal buffers: route to next internal buffer or out buffer
void network::internal_routing()
{
  // cout << "ENTERING: " << _routing_table[0][2] << endl;
#if WORST_NET == 1
  queue<net_packet> meta; // to store stopped tuples
#endif
  queue<iPair> dest_accessed;
  for (int i = 0; i < _num_links; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      if (_internal_buffers[i][j].empty())
        continue;
      int num_pops = 0;
      while (!_internal_buffers[i][j].empty() && num_pops < _bus_vec_width)
      {
        /*if(_internal_buffers[i][j].empty()) {
          cout << "empty at location: " << i << " " << j << endl;
        }*/
        net_packet cur_tuple(_internal_buffers[i][j].front());

        // condition here that the packet should not have been moved
        // earlier in the same cycle else continue/break
        if (_is_done[cur_tuple.label])
          break;
        _is_done[cur_tuple.label] = true;

        int dest_core_id = cur_tuple.dest_core_id;
        // FIXME: some flaw here
        int cur_core_id = -1;
        if (j == 0 || j == 3)
        {
          cur_core_id = get_core_id(i);
        }
        else
        {
          int num_horizontal = num_rows * (num_rows - 1);
          if (i < num_horizontal)
          { // if horizontal link
            cur_core_id = get_core_id(i) + 1;
          }
          else
          { // if vertical link
            cur_core_id = get_core_id(i) + num_rows;
          }
        }

        if (dest_core_id == cur_core_id)
        { // reached destination
          _net_out_buffer[dest_core_id].push(cur_tuple);
          _internal_buffers[i][j].pop();
        }
        else
        {
          // this is not a remote read packet (so req_core should be -1 and src
          // tile should not be -1)
          if (!is_remote_read(cur_tuple) && _asic->_config->_net_traffic == real_multicast)
          {
            transfer_multicast_packet(cur_tuple, cur_core_id, i, j, num_pops);
          }
          else
          {
            transfer_scalar_packet(cur_tuple, cur_core_id, i, j, num_pops);
          }
        }
      }

#if WORST_NET == 1
      while (!dest_accessed.empty())
      {
        iPair x = dest_accessed.front();
        _can_push_to_out[x.first][x.second] = false; // should be for all destinations accessed in that round
        dest_accessed.pop();
      }
      while (!meta.empty())
      {
        _internal_buffers[i][j].push(meta.front());
        meta.pop();
      }
#endif
    }
  }
}

// every cycle, it should serve mem_bw/64 cache lines
// create another packet with the updated data
void network::serve_memory_requests()
{
  int max_pops = 1; // mem_bw/(4*64); // 1 cache line per channel
  int cur_pops = 0;
  for (int i = 0; i < NUM_MC; ++i)
  {
    cur_pops = 0;
    while (!_mc_buffers[i].empty() && cur_pops < process_thr)
    {
      net_packet cur_tuple = _mc_buffers[i].front();
      int new_dest = cur_tuple.req_core_id;
      int buf_ind = cur_tuple.dest_core_id; // What was the old dest?
      // cout << "Came to serve memory controller buffers\n";

      // FIXME: for limited size? need to put backpressure?
      if (_asic->_config->_hats == 1)
      {
        assert(cur_tuple.packet_type != vectorizedRead && "shouldn't have come here");
        assert(_asic->_config->_domain != tree && "kNN does not involve tree requests");

        if (1)
        { // _asic->_mult_cores[cur_tuple.dest_core_id]->_batch_free_list.size()>0) {

          if (_asic->_config->_algo == gcn)
          {
            _asic->_remote_coalescer[8]->insert_leaf_task(cur_tuple.dst_id, new_dest, cur_tuple.cb_entry, cur_tuple.dfg_id); // these are just requests
          }
          else
          {
            _asic->_remote_coalescer[cur_tuple.dest_core_id]->insert_leaf_task(cur_tuple.dst_id, new_dest, cur_tuple.cb_entry, cur_tuple.dfg_id); // these are just requests
          }

          if (_asic->_config->_hats != 1)
          {
            assert(_asic->_coarse_reorder_buf[cur_tuple.req_core_id][cur_tuple.dfg_id]->_reorder_buf[cur_tuple.cb_entry].waiting_addr > 0 && "source core has already filled the space");
            assert(!_asic->_coarse_reorder_buf[cur_tuple.req_core_id][cur_tuple.dfg_id]->_reorder_buf[cur_tuple.cb_entry].valid && "source core has already filled the space");
          }
        }
        else
        {
          _mc_buffers[i].pop();
          break;
        }
      }
      else
      {
        cur_tuple.dst_id = cur_tuple.dst_id;
        cur_tuple.src_id = cur_tuple.src_id;
        // cout << "Read read source: " << i << " for src_id: " << cur_tuple.src_id << " and dst_id: " << cur_tuple.dst_id << endl;
        cur_tuple.dest_core_id = new_dest;
        cur_tuple.req_core_id = -1; // not a memory request now
        cur_tuple.packet_type = scalarResponse;
        cur_tuple.last = cur_tuple.last;

        // cout << "Buffer index: " << buf_ind << endl;
        _net_in_buffer[buf_ind].push(cur_tuple);
        // if(cur_tuple.packet_type!=fineScratch) {
        if (_asic->_config->_algo == gcn)
        {
          _mc_buffers[i].pop();
          break; // when this is vector
        }
      }

      _mc_buffers[i].pop();
      cur_pops++;
    }
  }
}

// TODO: might use it for simplicity
void network::routeXY(int core_id)
{
}

bool network::buffers_not_empty(bool show)
{
  for (int i = 0; i < NUM_MC; ++i)
  {
    if (!_mc_buffers[i].empty())
    {
      if (show)
        cout << "memory controllers not empty\n";
      return true;
    }
  }
  for (int c = 0; c < core_cnt; ++c)
  {
    if (!_net_in_buffer[c].empty())
    {
      if (show)
        cout << "network in buffers not empty\n";
      return true;
    }
    if (!_net_out_buffer[c].empty())
    {
      if (show)
        cout << "network out buffers not empty\n";
      return true;
    }
  }
  for (int i = 0; i < _num_links; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      if (!_internal_buffers[i][j].empty())
      {
        if (show)
          cout << "internal buffers not empty\n";
        return true;
      }
    }
  }
  return false;
}

// returns true if still active
bool network::cycle()
{

  /*for(int c=0; c<core_cnt; ++c) {
    assert(_net_in_buffer[c].size()<FIFO_DEP_LEN*4);
    assert(_net_out_buffer[c].size()<FIFO_DEP_LEN*4);
  }*/
  // This is not good!
  for (int i = 0; i < MAX_LABEL; ++i)
  {
    _is_done[i] = false;
  }
#if WORST_NET == 1
  for (int i = 0; i < _num_links; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      _can_push_to_out[i][j] = true;
    }
  }
#endif
  for (int c = 0; c < core_cnt; ++c)
  {
    external_routing(c);
  }
  internal_routing();
  serve_memory_requests();
  bool is_active = buffers_not_empty(false);
  for (int c = 0; c < core_cnt; ++c)
  {
    for (int i = 0; i < num_banks; ++i)
    {
      if (!_asic->_asic_cores[c]->_local_bank_queues[i].empty())
      {
        is_active = true;
        break;
      }
    }
    if (is_active)
      break;
    if (_asic->_config->_net == crossbar)
      break;
  }
  return is_active;
}
