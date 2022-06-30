#ifndef REPL_STATE_H
#define REPL_STATE_H

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This file is distributed as part of the Cache Replacement Championship     //
// workshop held in conjunction with ISCA'2010.                               //
//                                                                            //
//                                                                            //
// Everyone is granted permission to copy, modify, and/or re-distribute       //
// this software.                                                             //
//                                                                            //
// Please contact Aamer Jaleel <ajaleel@gmail.com> should you have any        //
// questions                                                                  //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

// ASIM core
#include "asim-defs.h"

#include <iostream>
#include <bitset>
#include <cstdlib>
#include <cassert>
#include "simple_cache_defs.h"

// Replacement Policies Supported
typedef enum
{
  REPL_LRU = 0,
  REPL_RANDOM = 1,
  REPL_CONTESTANT = 2,
  REPL_MRU = 3,
  REPL_PSEUDO_LRU_KNH = 4,
  REPL_PSEUDO_LRU = 5,
  REPL_PSEUDO_LRU_HIT_SPECIAL = 6,
  REPL_PSEUDO_LRU_ALL = 7,
  REPL_QUAD_AGE_LRU = 8,
  REPL_RRIP = 9
} ReplacemntPolicy;

// Replacement State Per Cache Line
typedef struct
{
  UINT32 LRUstackposition;
  INT32 QLRUage;
  std::bitset<8> pending_way;
  std::bitset<7> and_mask;
  std::bitset<7> or_mask;
  std::bitset<3> rr_int; // Assuming associativity of 8
} LINE_REPLACEMENT_STATE;

// The implementation for the cache replacement policy
class CACHE_REPLACEMENT_STATE
{
private:
  UINT32 numsets;
  UINT32 assoc;
  UINT32 replPolicy;
  UINT32 num_shifts;
  UINT32 pLRU_offset;
  UINT32 pLRU_mask_or;
  UINT32 pLRU_mask_and;

  LINE_REPLACEMENT_STATE **repl;
  LINE_REPLACEMENT_STATE *plru;

  COUNTER mytimer; // tracks # of references to the cache

public:
  // The constructor CAN NOT be changed
  CACHE_REPLACEMENT_STATE(UINT32 _sets, UINT32 _assoc, UINT32 _pol, bool evict_miss = false);
  ~CACHE_REPLACEMENT_STATE();

  INT32 GetLRUBits(UINT32 setIndex, INT32 updateWayID);

  INT32 GetVictimInSet(UINT32 tid, UINT32 setIndex, const LINE_STATE *vicSet, UINT32 assoc, Addr_t PC, Addr_t paddr, UINT32 accessType);
  void UpdateReplacementState(UINT32 setIndex, INT32 updateWayID);

  void SetReplacementPolicy(UINT32 _pol) { replPolicy = _pol; }
  void IncrementTimer() { mytimer++; }

  void UpdateReplacementState(UINT32 setIndex, INT32 updateWayID, const LINE_STATE *currLine,
                              UINT32 tid, Addr_t PC, UINT32 accessType, bool cacheHit);

  ostream &PrintStats(ostream &out);

  void ClearPseudoLRUPedingWay(UINT32 setIndex, UINT32 updateWayID);
  void SetPseudoLRUPedingWay(UINT32 setIndex, UINT32 updateWayID);

private:
  void InitReplacementState();
  void DeleteReplacementState();
  INT32 Get_Random_Victim(UINT32 setIndex); //, const LINE_STATE *vicset );

  INT32 Get_LRU_Victim(UINT32 setIndex); // , const LINE_STATE *vicset );
  INT32 Get_MRU_Victim(UINT32 setIndex); //, const LINE_STATE *vicset );

  INT32 Get_PseudoLRU_Victim_KNH(UINT32 setIndex);
  INT32 Get_PseudoLRU_Victim(UINT32 setIndex);
  INT32 Get_PseudoLRU_All_Victim(UINT32 setIndex);

  UINT32 Get_RRIP_Victim(UINT32 setIndex);
  void UpdateRRIP(UINT32 setIndex, INT32 updateWayID);

  void UpdateLRU(UINT32 setIndex, INT32 updateWayID);
  void UpdateMRU(UINT32 setIndex, INT32 updateWayID);

  void UpdatePseudoLRU_KNH(UINT32 setIndex, INT32 updateWayID);
  void UpdatePseudoLRU(UINT32 setIndex, INT32 updateWayID, bool fill);
  void UpdatePseudoLRU_Hit_Special(UINT32 setIndex, INT32 updateWayID);
  void UpdatePseudoLRU_All(UINT32 setIndex, INT32 updateWayID);

  bool IsValidVictim(const LINE_STATE victim);

  void quad_global_age(UINT32 setIndex);
  void UpdateQuadLRU(UINT32 setIndex, INT32 updateWayID, const LINE_STATE *currLine);
  UINT32 Get_QuadLRU_Victim(UINT32 setIndex, const LINE_STATE *vicSet);
  UINT32 Get_PseudoLRU_Victim_way(UINT32 entry, UINT32 vic_way, UINT32 offset);
  UINT32 Next_Power_2(UINT32 val);
  UINT32 Update_PseudoLRU_way(UINT32 entry, UINT32 wayID, UINT32 base_way, UINT32 offset, UINT32 mask);
  void UpdatepLRUMasks(UINT32 setIndex, INT32 updateWayID);
  void ClearpLRUMasks(UINT32 setIndex, INT32 updateWayID);
  bool ACCEL_L2_EVICT_ON_MISS_FIX;
};

#endif
