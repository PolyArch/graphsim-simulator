#ifndef _NETWORK_H
#define _NETWORK_H

#include "asic_core.hh"
#include "common.hh"

class asic_core;
class asic;
class network;
class stats;

class network
{
    friend class asic;

  public:
    network(asic *host);

    // these functions fill the static routing table for XY routing
    void fill_routing_table();
    void fill_link_map();
    void routeXY(int core_id);

    // this function pulls and pushes data from cores
    void external_routing(int core_id);
    void pull_packets_to_network(int core_id);
    void push_packets_from_network(int core_id);

    // this function implements the router functionality for data incoming and outgoing from network links
    void internal_routing();
    
    // utility functions to model the connections
    int get_core_id(int int_link_id);
    int get_link_dest_id(int int_link_id);
    int calc_hops(int src, int dest);
    // int get_core_id(int int_buf_id);
    // int next_destination(int cur_dest, int final_dest);

    // this function receive the memory request packet and create a response traffic to model memory traffic in the simulator
    void serve_memory_requests();
    bool cycle();
    bool buffers_not_empty(bool show);

    // cores use this function to push new packets into the network
    void push_net_packet(int core_id, net_packet cur_tuple);

    // these functions decode the network packet at the destination core

    // this function pushes the scalar response to the "process" datapath stage
    void decode_scalar_packet(net_packet remote_tuple, int core_id);
    // push the data and send the left over packet again over the network
    void decode_path_multicast(net_packet remote_tuple, int core_id);
    // push either to bank queues for update; or pending task queue is the update
    // is delayed; not sure why prefetch process???
    void decode_real_multicast(net_packet remote_tuple, int core_id);

    // these functions are implemented by the router to reformat a network packet

    // this function creates a scalar packet as a response to memory request
    void encode_scalar_packet(net_packet remote_tuple, int core_id);
    // this function removes the current destinations and pick the next direction according to the nearest destination first
    void encode_path_multicast(net_packet remote_tuple, int core_id);
    // this function sorts the destinations and creates multiple packets that will go in different directions
    void encode_real_multicast(net_packet remote_tuple, int core_id);

    // these functions reformat the packets, but transfer them after considering the link bandwidth constraints
    void transfer_scalar_packet(net_packet cur_tuple, int cur_core_id, int i, int j, int &num_pops);
    void transfer_multicast_packet(net_packet cur_tuple, int cur_core_id, int i, int j, int &num_pops);

    // implements the router buffers for latency hiding
    bool can_push_in_network(int core_id);
    bool can_push_in_internal_buffer(int x, int y);
    void update_pops_acc_to_bandwidth(net_packet cur_tuple, int &num_pops);

    // these are the utility functions to create different kinds of packets allowed in the network
    // Please see README to get details about these packets
    net_packet create_dummy_request_packet(int src_id, int dst_id, int req_core);
    net_packet create_scalar_request_packet(int src_id, int dst_id, int req_core, int cb_entry, bool second_buffer);
    net_packet create_scalar_update_packet(int tid, int src_id, vector<iPair> multicast_dst_wgt, DTYPE new_dist);
    net_packet create_noopt_scalar_update_packet(int src_id, int dst_id, DTYPE new_dist);
    net_packet create_vector_update_packet(int tid, int src_id, vector<iPair> multicast_dst_wgt, DTYPE vertex_data[FEAT_LEN]);
    net_packet create_noopt_vector_update_packet(int src_id, int dst_id, DTYPE vertex_data[FEAT_LEN]);

    // these are utility function to convert the format from the task creation to task execution packet
    red_tuple convert_net_to_red(net_packet packet, int k);
    pref_tuple convert_net_to_pref(net_packet packet, int k);
    net_packet convert_pref_to_net(int req_core, int dfg_id, pref_tuple cur_tuple);
    bool is_remote_read(net_packet cur_tuple);

    // after decode, network packets are pushed to the computation datapath
    void push_to_prefetch_process(int dest_core, net_packet cur_tuple, int k);

  public:
    // queue<net_packet> _net_in_buffer[core_cnt];
    // protected:
    queue<net_packet> _net_in_buffer[core_cnt];
    queue<net_packet> _net_out_buffer[core_cnt];
    int *_core_net_map[num_rows]; // [num_rows]; // 4 cores mapped to each subblock
    // assuming square mesh network
    /*indexing for the cores
     * 0..1..2..3
     * 4..5..6..7
     * 8..9..10..11
     * 12..13..14..15
     *
     * indexing of the links
     * 0..1..2
     * .
     * .
     * 12..13..14
     *
     * buffers on a link
     * 0..2
     * 3..1
     */
    queue<net_packet> _internal_buffers[(num_rows) * (num_rows - 1) * 2][4];
    // queue<net_packet> *_internal_buffers[(num_rows)*(num_rows-1)*2];
    // queue<net_packet> *_mc_buffers; // [NUM_MC];
    queue<net_packet> _mc_buffers[NUM_MC];
    // Normal network: 0..2 or 3..1
    // TODO: Let's keep this true always, later need to add stall for this
    // bool _bus_available[2*(num_rows-1)*(num_rows-1)];
    bool *_bus_available; // [2*(num_rows-1)*(num_rows)*2];
    // Decomposable network
    // bool _bus_available[(num_rows-1)*(num_rows-1)*bus_width/message_size];
    int *_routing_table[core_cnt]; // [core_cnt];

    // give link_id for src->dest if exists (symmetric matrix)
    int *_link_map[core_cnt]; // [core_cnt];
    // unordered_map<int, int> _map_dest_buf = {{0,2},{1,3}};
    int *_map_dest_buf;
    bool *_is_done; // [MAX_LABEL]; // hope maximum packets active at the same time
    int _cur_packet_label = 0;

    // num bi-directional links
    const int _num_links = (num_rows - 1) * (num_rows)*2;
#if IDEAL_NET == 1
    const int _bus_vec_width = INF;
#elif DECOMPOSABLE == 1
    const int _bus_vec_width = bus_width / message_size;
#else
    const int _bus_vec_width = 1;
#endif

    // #if DECOMPOSABLE == 1
    //     const int _bus_vec_width=bus_width/message_size;
    // #else
    //     const int _bus_vec_width=1;
    // #endif

#if WORST_NET == 1
    bool _can_push_to_out[(num_rows) * (num_rows - 1) * 2][4];
#endif

    // FIXME: this should be scalar packets, right?
    int _stat_packets_to_router[core_cnt];
    int _stat_tot_packets_transferred = 0;
    // FIXME: motivation for vector NoC would be realized if we have core throughput
    // > 1
    int _stat_tot_bw_utilization = 0;
    int _stat_local_updates = 0;
    int _stat_remote_updates = 0;
    int _num_outstanding_packets = 0;
    int _active_net_packets = 0;

    asic *_asic;
};

#endif
