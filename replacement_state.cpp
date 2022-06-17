#include "replacement_state.h"
#include <csignal>
#include "limits.h"

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

/*
** This file implements the cache replacement state. Users can enhance the code
** below to develop their cache replacement ideas.
**
*/


////////////////////////////////////////////////////////////////////////////////
// The replacement state constructor:                                         //
// Inputs: number of sets, associativity, and replacement policy to use       //
// Outputs: None                                                              //
//                                                                            //
// DO NOT CHANGE THE CONSTRUCTOR PROTOTYPE                                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
CACHE_REPLACEMENT_STATE::CACHE_REPLACEMENT_STATE( UINT32 _sets, UINT32 _assoc, UINT32 _pol, bool evict_miss )
{

    numsets    = _sets;
    assoc      = _assoc;
    replPolicy = _pol;

    num_shifts = 0;

    pLRU_offset = (Next_Power_2(assoc))/2;
    pLRU_mask_or = 0u;
    pLRU_mask_and = ~(0u);

    mytimer    = 0;

    CSA_L2_EVICT_ON_MISS_FIX = evict_miss;

    switch (assoc) {
        case 6:
            pLRU_mask_and = ~(64);
            break;

        case 10:
            pLRU_mask_and = ~(768);
            //                        pLRU_mask_or = 4;
            break;

        case 12:
            pLRU_mask_and = ~(256);
            //                        pLRU_mask_or = 4;
            break;

        case 14:
            pLRU_mask_and = ~(4096);
            break;

        default:
            break;
    }

    InitReplacementState();
}

CACHE_REPLACEMENT_STATE::~CACHE_REPLACEMENT_STATE()
{
    DeleteReplacementState();
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function initializes the replacement policy hardware by creating      //
// storage for the replacement state on a per-line/per-cache basis.           //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void CACHE_REPLACEMENT_STATE::InitReplacementState() {
    // std::cout << "Came here with replacement policy to: " << replPolicy << "\n";
    if (replPolicy == REPL_MRU || replPolicy == REPL_QUAD_AGE_LRU || replPolicy == REPL_LRU || replPolicy == REPL_RRIP) {

        // Create the state for sets, then create the state for the ways
        repl = new LINE_REPLACEMENT_STATE* [numsets];
        assert(repl); // ensure that we were able to create replacement state
        plru = NULL;

        // Create the state for the sets
        for (UINT32 setIndex = 0; setIndex < numsets; ++setIndex) {
            repl[setIndex] = new LINE_REPLACEMENT_STATE[assoc];
            for (UINT32 way = 0; way < assoc; ++way) {
                repl[setIndex][way].pending_way.reset();
                repl[setIndex][way].and_mask.set();
                repl[setIndex][way].or_mask.reset();

                // set all bits in as well
                repl[setIndex][way].rr_int.set();

                if (replPolicy == REPL_MRU) {
                    repl[setIndex][way].QLRUage = -1;
                    repl[setIndex][way].LRUstackposition = 0;
                } else if (replPolicy == REPL_QUAD_AGE_LRU) {
                    repl[setIndex][way].QLRUage = 0;
                    repl[setIndex][way].LRUstackposition = 0;
                } else {
                    // initialize stack position (for true LRU)
                    repl[setIndex][way].QLRUage = -1;
                    repl[setIndex][way].LRUstackposition = way;
                }
            }
        }
    }
    // Sid Fix Me: Confirm this
    if ((replPolicy == REPL_PSEUDO_LRU_KNH) ||
        (replPolicy == REPL_PSEUDO_LRU) ||
        (replPolicy == REPL_PSEUDO_LRU_HIT_SPECIAL) ||
        (replPolicy == REPL_PSEUDO_LRU_ALL)) {
        repl = NULL;
        plru = new LINE_REPLACEMENT_STATE[numsets];
        // Create the state for the sets
        for (UINT32 setIndex = 0; setIndex<numsets; ++setIndex) {
            plru[setIndex].pending_way.reset();
            plru[setIndex].and_mask.set();
            plru[setIndex].or_mask.reset();
            plru[setIndex].QLRUage = -1;
            plru[setIndex].LRUstackposition = 0;

            if (replPolicy == REPL_PSEUDO_LRU_ALL) {
                if (assoc == 6 || assoc == 8 || assoc == 10 || assoc == 12) {
                    plru[setIndex].LRUstackposition |= pLRU_mask_or;
                    plru[setIndex].LRUstackposition &= pLRU_mask_and;
                }
            }
        }
    }

    if (replPolicy == REPL_PSEUDO_LRU_KNH) {
        switch (assoc) {
            case 2:
                num_shifts = 4;
                break;
            case 4:
                num_shifts = 3;
                break;
            case 8:
                num_shifts = 2;
                break;
            case 16:
                num_shifts = 1;
                break;
            case 32:
                num_shifts = 0;
                break;
            default:
                assert(0 && "Invalid lru way");
                break;
        }
    }
}

void CACHE_REPLACEMENT_STATE::DeleteReplacementState()
{
    for (UINT32 setIndex=0; setIndex<numsets; ++setIndex) {
        delete [] repl[setIndex];
    }
    delete [] repl;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is called by the cache on every cache miss. The input        //
// arguments are the thread id, set index, pointers to ways in current set    //
// and the associativity.  We are also providing the PC, physical address,    //
// and accesstype should you wish to use them at victim selection time.       //
// The return value is the physical way index for the line being replaced.    //
// Return -1 if you wish to bypass LLC.                                       //
//                                                                            //
// vicSet is the current set. You can access the contents of the set by       //
// indexing using the wayID which ranges from 0 to assoc-1 e.g. vicSet[0]     //
// is the first way and vicSet[4] is the 4th physical way of the cache.       //
// Elements of LINE_STATE are defined in simple_cache_defs.h                  //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
INT32 CACHE_REPLACEMENT_STATE::GetVictimInSet( UINT32 tid, UINT32 setIndex, const LINE_STATE *vicSet, UINT32 assoc,
                                               Addr_t PC, Addr_t paddr, UINT32 accessType )
{

    // std::cout << "Came in with repl policy: " << replPolicy << "\n";
    assert(setIndex < numsets);

    // For Direct Map Cache
    if (assoc == 1) {
        return 0;
    }
    // If no invalid lines, then replace based on replacement policy
    if (replPolicy == REPL_LRU) {
        // std::cout << "LRU\n";
        return Get_LRU_Victim(setIndex);
    } else if (replPolicy == REPL_RANDOM) {
        // std::cout << "RANDOM\n";
        return Get_Random_Victim(setIndex);
    } else if (replPolicy == REPL_CONTESTANT) {
        // Contestants:  ADD YOUR VICTIM SELECTION FUNCTION HERE
        return Get_LRU_Victim(setIndex);
    } else if (replPolicy == REPL_MRU) {
        // std::cout << "MRU\n";
        return Get_MRU_Victim(setIndex);
        // return Get_MRU_Victim(setIndex, vicSet);
    } else if (replPolicy == REPL_PSEUDO_LRU_KNH) {
        return Get_PseudoLRU_Victim_KNH(setIndex);
    } else if ((replPolicy == REPL_PSEUDO_LRU) || (replPolicy == REPL_PSEUDO_LRU_HIT_SPECIAL)) {
        assert(assoc == 8 && "Invalid Assoc for this Pseudo LRU knob.");
        return Get_PseudoLRU_Victim(setIndex);
    } else if (replPolicy == REPL_PSEUDO_LRU_ALL) {
        assert(((assoc<16) || (assoc & (assoc-1)) == 0) && "Associativity not supported by Replacement policy");
        assert(((assoc>16) || (assoc%2) == 0) && "Associativity not supported by Replacement policy");
        return Get_PseudoLRU_All_Victim(setIndex);
    } else if (replPolicy == REPL_QUAD_AGE_LRU) {
        return Get_QuadLRU_Victim(setIndex, vicSet);
    } else if (replPolicy == REPL_RRIP) {
        // std::cout << "Went to get rrip victim set\n";
        // assert(assoc == 8 && "Invalid Assoc for this Pseudo LRU knob.");
        return Get_RRIP_Victim(setIndex);
    }

    // We should never get here
    assert(0 && "Unknown Replacement policy");

    return -1; // Returning -1 bypasses the LLC
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is called by the cache after every cache hit/miss            //
// The arguments are: the set index, the physical way of the cache,           //
// the pointer to the physical line (should contestants need access           //
// to information of the line filled or hit upon), the thread id              //
// of the request, the PC of the request, the accesstype, and finall          //
// whether the line was a cachehit or not (cacheHit=true implies hit)         //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void CACHE_REPLACEMENT_STATE::UpdateReplacementState( 
    UINT32 setIndex, INT32 updateWayID, const LINE_STATE *currLine, 
    UINT32 tid, Addr_t PC, UINT32 accessType, bool cacheHit )
{
    assert(setIndex < numsets);
    assert(updateWayID < assoc);

    // For Direct Map Cache
    if (assoc == 1) {
        return;
    }
    // What replacement policy?
    if (replPolicy == REPL_LRU) {
        UpdateLRU(setIndex, updateWayID);
        return;
    } else if (replPolicy == REPL_RANDOM) {
        // Random replacement requires no replacement state update
        return;
    } else if (replPolicy == REPL_CONTESTANT) {
        return;
        // Contestants:  ADD YOUR UPDATE REPLACEMENT STATE FUNCTION HERE
        // Feel free to use any of the input parameters to make
        // updates to your replacement policy
    } else if (replPolicy == REPL_MRU) {
        UpdateMRU(setIndex, updateWayID);
        return;
    } else if (replPolicy == REPL_PSEUDO_LRU_KNH) {
        UpdatePseudoLRU_KNH(setIndex, updateWayID);
        return;
    } else if (replPolicy == REPL_PSEUDO_LRU) {
        UpdatePseudoLRU(setIndex, updateWayID, !cacheHit);
        return;
    } else if (replPolicy == REPL_PSEUDO_LRU_HIT_SPECIAL) {
        if (cacheHit) {
            UpdatePseudoLRU_Hit_Special(setIndex, updateWayID);
        } else {
            UpdatePseudoLRU(setIndex, updateWayID, !cacheHit);
        }
        return;
    } else if (replPolicy == REPL_PSEUDO_LRU_ALL) {
        UpdatePseudoLRU_All(setIndex, updateWayID);
        return;
    } else if (replPolicy == REPL_QUAD_AGE_LRU) {
        UpdateQuadLRU(setIndex, updateWayID, currLine);
        return;
    } else if (replPolicy == REPL_RRIP) {
        // std::cout << "RRIP gone to update state\n";
        UpdateRRIP(setIndex, updateWayID);
        return;
    }

    assert(0 && "Unknown Replacement policy");
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//////// HELPER FUNCTIONS FOR REPLACEMENT UPDATE AND VICTIM SELECTION //////////
//                                                                            //
////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function finds the LRU victim in the cache set by returning the       //
// cache block at the bottom of the LRU stack. Top of LRU stack is '0'        //
// while bottom of LRU stack is 'assoc-1'                                     //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
INT32 CACHE_REPLACEMENT_STATE::Get_LRU_Victim(UINT32 setIndex) {
    assert(setIndex < numsets);
    // Get pointer to replacement state of current set
    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];

    INT32 lruWay = 0;

    // Search for victim whose stack position is assoc-1
    for (UINT32 way = 0; way < assoc; way++) {
        if (replSet[way].LRUstackposition == (assoc-1)) {
            lruWay = way;
            break;
        }
    }

    // std::cout << "Victim LRU set: " << setIndex << " way: " << lruWay << std::endl;
    return lruWay;
}

// bit position to look for depends on the value of previous bit (Why so?)
INT32 CACHE_REPLACEMENT_STATE::Get_PseudoLRU_Victim_KNH(UINT32 setIndex) {
    assert(setIndex < numsets);
    UINT32 entry = plru[setIndex].LRUstackposition;
    UINT32 bit_num = 0;
    bool bit0 = entry & (1 << bit_num);
    bit_num = 1 + 2 * bit_num + bit0;
    bool bit1 = entry & (1 << bit_num);
    bit_num = 1 + 2 * bit_num + bit1;
    bool bit2 = entry & (1 << bit_num);
    bit_num = 1 + 2 * bit_num + bit2;
    bool bit3 = entry & (1 << bit_num);
    bit_num = 1 + 2 * bit_num + bit3;
    bool bit4 = entry & (1 << bit_num);

    UINT32 lru_way = (bit0 ? 16 : 0)
                     + (bit1 ? 8 : 0)
                     + (bit2 ? 4 : 0)
                     + (bit3 ? 2 : 0)
                     + (bit4 ? 1 : 0);

    return lru_way >> num_shifts;
}

// looks like the conventional tree-based LRU (PLRU)
INT32 CACHE_REPLACEMENT_STATE::Get_PseudoLRU_Victim(UINT32 setIndex) {
    assert(setIndex < numsets);
    UINT32 entry = ((plru[setIndex].LRUstackposition |  plru[setIndex].or_mask.to_ulong()) & plru[setIndex].and_mask.to_ulong());

    if (!CSA_L2_EVICT_ON_MISS_FIX) {
        assert(plru[setIndex].or_mask.none());
        assert(plru[setIndex].and_mask.all());
    }

    if (plru[setIndex].pending_way.all()) {
        return -1;
    }

    UINT32 lru_way = 0;
    bool bit0 = entry & 1;
    bool bit1 = entry & 2;
    bool bit2 = entry & 4;
    bool bit3 = entry & 8;
    bool bit4 = entry & 16;
    bool bit5 = entry & 32;
    bool bit6 = entry & 64;

    if (bit2 == 0 && bit1 == 0 && bit0 == 0) {
        lru_way = 0;
    } else if (bit2 == 1 && bit1 == 0 && bit0 == 0) {
        lru_way = 1;
    } else if (bit3 == 0 && bit1 == 1 && bit0 == 0) {
        lru_way = 2;
    } else if (bit3 == 1 && bit1 == 1 && bit0 == 0) {
        lru_way = 3;
    } else if (bit5 == 0 && bit4 == 0 && bit0 == 1) {
        lru_way = 4;
    } else if (bit5 == 1 && bit4 == 0 && bit0 == 1) {
        lru_way = 5;
    } else if (bit6 == 0 && bit4 == 1 && bit0 == 1) {
        lru_way = 6;
    } else if (bit6 == 1 && bit4 == 1 && bit0 == 1) {
        lru_way = 7;
    } else {
        assert(0);
    }

    if (CSA_L2_EVICT_ON_MISS_FIX) {
        SetPseudoLRUPedingWay(setIndex, lru_way);
    }
    return lru_way;
}

void CACHE_REPLACEMENT_STATE::ClearPseudoLRUPedingWay(UINT32 setIndex, UINT32 updateWayID) {
    assert(setIndex < numsets);
    LINE_REPLACEMENT_STATE *updSet = &plru[setIndex];
    //assert(updSet->pending_way.test(updateWayID), "No pending fill for set/way specified");
    if (!updSet->pending_way.test(updateWayID)) {
        std::cerr<<"No pending fill for set/way specified"<<std::endl;
        std::raise(SIGINT);
    }

    updSet->pending_way.reset(updateWayID);
    ClearpLRUMasks(setIndex, updateWayID);
}

void CACHE_REPLACEMENT_STATE::SetPseudoLRUPedingWay(UINT32 setIndex, UINT32 updateWayID) {
    assert(setIndex < numsets);
    LINE_REPLACEMENT_STATE* updSet = &plru[setIndex];
    assert(!updSet->pending_way.test(updateWayID) && "Previous pending fill for set/way specified");

    updSet->pending_way.set(updateWayID);
    UpdatepLRUMasks(setIndex, updateWayID);
}

void CACHE_REPLACEMENT_STATE::UpdatePseudoLRU_KNH(UINT32 setIndex, INT32 updateWayID) {
    assert(setIndex < numsets);
    UINT32 entry = plru[setIndex].LRUstackposition;
    UINT32 shifted_way = updateWayID << num_shifts;

    int bit_to_scan = 4;
    UINT32 bit_to_set = 0;
    UINT32 one_bit = 0;
    bool bit_is_set = 0;
    do {
        one_bit = 1 << bit_to_set;
        bit_is_set = shifted_way & (1 << bit_to_scan);
        entry = (entry & ~one_bit) + (bit_is_set ? 0 : one_bit);
        bit_to_set = 1 + 2 * bit_to_set + bit_is_set;
        bit_to_scan--;
    } while (bit_to_scan >= 0);

    plru[setIndex].LRUstackposition = entry;
}

// New LRU State[6:0]
//      | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
// Way0 | - | - | - | - | 1 | 1 | 1 | Way0 => | 7
// Way1 | - | - | - | - | 0 | 1 | 1 | Way1 => | 3  => & (~4)
// Way2 | - | - | - | 1 | - | 0 | 1 | Way2 => | 9  => & (~2)
// Way3 | - | - | - | 0 | - | 0 | 1 | Way3 => | 1  => & (~10)
// Way4 | - | 1 | 1 | - | - | - | 0 | Way4 => | 48 => & (~1)
// Way5 | - | 0 | 1 | - | - | - | 0 | Way5 => | 16 => & (~33)
// Way6 | 1 | - | 0 | - | - | - | 0 | Way6 => | 64 => & (~17)
// Way7 | 0 | - | 0 | - | - | - | 0 | Way7 => | 0  => & (~81)
void CACHE_REPLACEMENT_STATE::UpdatePseudoLRU(UINT32 setIndex, INT32 updateWayID, bool fill) {
    assert(setIndex < numsets);
    UINT32 entry = plru[setIndex].LRUstackposition;

    if (!CSA_L2_EVICT_ON_MISS_FIX) {
        assert(plru[setIndex].or_mask.none());
        assert(plru[setIndex].and_mask.all());
    }

    switch (updateWayID) {
        case 0:
            entry = entry | 7;
            break;
        case 1:
            entry = entry | 3;
            entry = entry & (~4);
            break;
        case 2:
            entry = entry | 9;
            entry = entry & (~2);
            break;
        case 3:
            entry = entry | 1;
            entry = entry & (~10);
            break;
        case 4:
            entry = entry | 48;
            entry = entry & (~1);
            break;
        case 5:
            entry = entry | 16;
            entry = entry & (~33);
            break;
        case 6:
            entry = entry | 64;
            entry = entry & (~17);
            break;
        case 7:
            entry = entry | 0;
            entry = entry & (~81);
            break;
        default:    assert(0 && "Invalid lru way");
            break;
    }
    plru[setIndex].LRUstackposition = entry;

    if (CSA_L2_EVICT_ON_MISS_FIX && fill) {
        ClearPseudoLRUPedingWay(setIndex, updateWayID);
    }
}

// On Hits      New LRU State[6:0]
//      | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
// Way0 | - | - | - | - | - | - | 1 | Way0 => | 1
// Way1 | - | - | - | - | - | 1 | - | Way1 => | 2
// Way2 | - | - | - | 1 | - | - | - | Way2 => | 8
// Way3 | - | - | - | - | - | 0 | - | Way3 => & (~2)
// Way4 | - | - | 1 | - | - | - | - | Way4 => | 16
// Way5 | - | 0 | - | - | - | - | - | Way5 => & (~32)
// Way6 | 1 | - | - | - | - | - | - | Way6 => | 64
// Way7 | - | - | - | - | - | - | 0 | Way7 => & (~1)
void CACHE_REPLACEMENT_STATE::UpdatePseudoLRU_Hit_Special(
        UINT32 setIndex, INT32 updateWayID) {
    assert(setIndex < numsets);
    UINT32 entry = plru[setIndex].LRUstackposition;
    switch (updateWayID) {
        case 0:
            entry = entry | 1;
            break;
        case 1:
            entry = entry | 2;
            break;
        case 2:
            entry = entry | 8;
            break;
        case 3:
            entry = entry & (~2);
            break;
        case 4:
            entry = entry | 16;
            break;
        case 5:
            entry = entry & (~32);
            break;
        case 6:
            entry = entry | (64);
            break;
        case 7:
            entry = entry & (~1);
            break;
        default:
            assert(0 && "Invalid lru way");
            break;
    }
    plru[setIndex].LRUstackposition = entry;
}

INT32 CACHE_REPLACEMENT_STATE::Get_PseudoLRU_All_Victim(UINT32 setIndex) {
    assert(setIndex < numsets);
    UINT32 entry = plru[setIndex].LRUstackposition;
    UINT32 lru_way = 0;

    lru_way = Get_PseudoLRU_Victim_way(entry, lru_way, pLRU_offset);

    //    std::cerr<<"Entry:"<<entry<<" Way:"<<lru_way<<std::endl;

    return lru_way;
}

void CACHE_REPLACEMENT_STATE::UpdatePseudoLRU_All(UINT32 setIndex, INT32 updateWayID) {
    assert(setIndex < numsets);
    UINT32 entry = plru[setIndex].LRUstackposition;

    entry = Update_PseudoLRU_way(entry, updateWayID, 0u, pLRU_offset, 1u);

    entry |= pLRU_mask_or;
    entry &= pLRU_mask_and;

    plru[setIndex].LRUstackposition = entry;

}

UINT32 CACHE_REPLACEMENT_STATE::Get_PseudoLRU_Victim_way(UINT32 entry, UINT32 vic_way, UINT32 offset) {
    if (offset == 1) {
        if (entry%2 == 0) {
            return vic_way;
        } else {
            return vic_way+1;
        }
    }

    if (entry%2 == 0) {
        return Get_PseudoLRU_Victim_way(entry>>1, vic_way, offset/2);
    } else {
        return Get_PseudoLRU_Victim_way(entry>>offset, vic_way+(offset), offset/2);
    }
}

UINT32 CACHE_REPLACEMENT_STATE::Next_Power_2(UINT32 val) {
    UINT32 i = val & (val-1);
    UINT32 ret_val = val;

    while (i != 0) {
        ret_val = i*2;
        i = i & (i-1);
    }

    //    std::cerr<<" In Value:"<<val<<" POW2:"<<ret_val<<std::endl;

    return ret_val;
}

UINT32 CACHE_REPLACEMENT_STATE::Update_PseudoLRU_way(UINT32 entry, UINT32 wayID, UINT32 base_way, UINT32 offset, UINT32 mask) {
    if (offset == 1) {
        if (wayID<(base_way+offset)) {
            return entry|mask;
        } else {
            return entry&(~mask);
        }
    }

    if (wayID<(base_way+offset)) {
        return Update_PseudoLRU_way(entry|mask, wayID, base_way, offset/2, mask<<1);
    } else {
        return Update_PseudoLRU_way(entry&(~mask), wayID, base_way+offset, offset/2, mask<<offset);
    }
}

INT32 CACHE_REPLACEMENT_STATE::Get_MRU_Victim(UINT32 setIndex) {
    // Get pointer to replacement state of current set
    assert(setIndex < numsets);
    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];

    for (UINT32 ii = 0; ii < assoc; ++ii) {
        if (replSet[ii].LRUstackposition == 0) {
            // std::cout << "Victim MRU set: " << setIndex << " way: " << ii << std::endl;
            return ii;
        }
    }

    assert(0 && "Should not happen for OBL2!!");
}

/*
INT32 CACHE_REPLACEMENT_STATE::Get_MRU_Victim( UINT32 setIndex, const LINE_STATE *vicset)
{
    // Get pointer to replacement state of current set
    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];

    INT32 mruWay = 0;
    INT32 minBits = INT_MAX;

    for (UINT32 way=0; way<assoc; way++)
    {
        if (replSet[way].LRUstackposition < minBits && IsValidVictim(vicset[way]))
        {
            mruWay = way;
            minBits = replSet[way].LRUstackposition;
        }
    }

    return mruWay;
}
*/


////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function finds a random victim in the cache set                       //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
INT32 CACHE_REPLACEMENT_STATE::Get_Random_Victim( UINT32 setIndex) // , const LINE_STATE *vicset)
{
    INT32 way = -1;
    way = (rand() % assoc);

    /*
    do {
        way = (rand() % assoc);
    }
    while (!IsValidVictim(vicset[way]));
    */

    return way;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function finds the Quad LRU victim in the cache set by returning the  //
// first cache block with LRU count as 0.                                     //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
UINT32 CACHE_REPLACEMENT_STATE::Get_QuadLRU_Victim(UINT32 setIndex ,  const LINE_STATE *vicSet) {
    assert(setIndex < numsets);
    // Get pointer to replacement state of current set
    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];

    INT32 lruWay = -1;

    // Search for victim whose stack position is assoc-1
    for (UINT32 way = 0; way < assoc; way++) {
        assert((vicSet[way].valid == true) && "Invalid way found in quad victim search");

        if (replSet[way].LRUstackposition == 0) {
            lruWay = way;
            break;
        }
    }

    // return lru way
    if (lruWay!=-1) {
        return lruWay;
    } else {
        quad_global_age(setIndex);
        return 0;
    }
}

void CACHE_REPLACEMENT_STATE::quad_global_age(UINT32 setIndex) {
    assert(setIndex < numsets);
    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];

    for (UINT32 way = 0; way < assoc; way++) {
        //TODO:Sabir implement complete algorithm
        replSet[way].LRUstackposition = 0;
    }
}

UINT32 CACHE_REPLACEMENT_STATE::Get_RRIP_Victim(UINT32 setIndex) {
    assert(setIndex < numsets);
    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];

    // this is a set of all lines

    int count = 0;
    while(1) {
        count ++;
        for (UINT32 way = 0; way < assoc; way++) {
            if(replSet[way].rr_int.all()) { // =7
                replSet[way].rr_int.flip(2); // flip lsb
                return way;
            }
        }
        for (UINT32 way = 0; way < assoc; way++) {
          replSet[way].rr_int = bitset<3>(replSet[way].rr_int.to_ulong()+1ULL);
        }
        if(count >=8) {
            std::cout << "problem here\n";
            exit(0);
        }
    };

    return -1;

}

void CACHE_REPLACEMENT_STATE::UpdateRRIP(UINT32 setIndex, INT32 updateWayID) {
    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];
    assert(updateWayID < assoc);
    replSet[updateWayID].rr_int.reset();
}

void CACHE_REPLACEMENT_STATE::UpdateQuadLRU(UINT32 setIndex, INT32 updateWayID, const LINE_STATE *currLine) {
    assert(setIndex < numsets);
    assert(updateWayID < assoc);

    repl[setIndex][updateWayID].LRUstackposition = 1;

    for (UINT32 way = 0; way < assoc; way++) {
        if ((repl[setIndex][way].LRUstackposition == 0) || (currLine->valid == false)) {
            return;
        }
    }

    quad_global_age(setIndex);
}



////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function implements the LRU update routine for the traditional        //
// LRU replacement policy. The arguments to the function are the physical     //
// way and set index.                                                         //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void CACHE_REPLACEMENT_STATE::UpdateLRU( UINT32 setIndex, INT32 updateWayID )
{
    // Determine current LRU stack position
    UINT32 currLRUstackposition = repl[ setIndex ][ updateWayID ].LRUstackposition;

    // Update the stack position of all lines before the current line
    // Update implies incremeting their stack positions by one
    for(UINT32 way=0; way<assoc; ++way)
    {
        if( repl[setIndex][way].LRUstackposition < currLRUstackposition )
        {
            repl[setIndex][way].LRUstackposition++;
        }
    }

    // Set the LRU stack position of new line to be zero
    repl[ setIndex ][ updateWayID ].LRUstackposition = 0;
}

INT32 CACHE_REPLACEMENT_STATE::GetLRUBits( UINT32 setIndex, INT32 updateWayID )
{
    return repl[ setIndex ][ updateWayID ].LRUstackposition;
}

void CACHE_REPLACEMENT_STATE::UpdateMRU(UINT32 setIndex, INT32 updateWayID)
{
    assert(setIndex < numsets);
    assert(updateWayID < assoc);

    LINE_REPLACEMENT_STATE *replSet = repl[setIndex];
    if (replSet[updateWayID].LRUstackposition == 0) {
        bool clear_all = true;
        for (UINT32 ii = 0; ii < assoc; ++ii) {
            if (ii != updateWayID && replSet[ii].LRUstackposition == 0) {
                clear_all = false;
                break;
            }
        }

        if (clear_all) {
            for (UINT32 ii = 0; ii < assoc; ++ii) {
                replSet[ii].LRUstackposition = 0;
            }
        }
        replSet[updateWayID].LRUstackposition = 1;
    }
}

bool CACHE_REPLACEMENT_STATE::IsValidVictim(const LINE_STATE victim)
{
   if(victim.shared_upgrading)
      return false;
   else
      return true;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The function prints the statistics for the cache                           //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
ostream & CACHE_REPLACEMENT_STATE::PrintStats(ostream &out)
{
//
//     out<<"=========================================================="<<endl;
//     out<<"=========== Replacement Policy Statistics ================"<<endl;
//     out<<"=========================================================="<<endl;

    // CONTESTANTS:  Insert your statistics printing here

    return out;
}

void CACHE_REPLACEMENT_STATE::UpdatepLRUMasks (UINT32 setIndex, INT32 updateWayID) {
    assert(setIndex < numsets);
    LINE_REPLACEMENT_STATE *updSet = &plru[setIndex];

    if (updateWayID<4) {
        if (updSet->pending_way[0] && updSet->pending_way[1] && updSet->pending_way[2] && updSet->pending_way[3]) {
            updSet->or_mask.set(0);
            updSet->or_mask.reset(1);
            updSet->or_mask.reset(2);
            updSet->or_mask.reset(3);
            updSet->and_mask.set(0);
            updSet->and_mask.set(1);
            updSet->and_mask.set(2);
            updSet->and_mask.set(3);
        } else {
            if (updateWayID<2) {
                if (updSet->pending_way[0] && updSet->pending_way[1]) {
                    updSet->or_mask.set(1);
                    updSet->or_mask.reset(2);
                    updSet->and_mask.set(1);
                    updSet->and_mask.set(2);
                } else {
                    if (updSet->pending_way[0]) {
                        updSet->or_mask.set(2);
                        updSet->and_mask.set(2);
                    } else {
                        assert(updSet->pending_way[1]);
                        updSet->or_mask.reset(2);
                        updSet->and_mask.reset(2);
                    }
                }
            } else {
                if (updSet->pending_way[2] && updSet->pending_way[3]) {
                    updSet->or_mask.reset(1);
                    updSet->or_mask.reset(3);
                    updSet->and_mask.reset(1);
                    updSet->and_mask.set(3);
                } else {
                    if (updSet->pending_way[2]) {
                        updSet->or_mask.set(3);
                        updSet->and_mask.set(3);
                    } else {
                        assert(updSet->pending_way[3]);
                        updSet->or_mask.reset(3);
                        updSet->and_mask.reset(3);
                    }
                }
            }
        }
    } else {
        assert((updateWayID<8));
        if (updSet->pending_way[4] && updSet->pending_way[5] && updSet->pending_way[6] && updSet->pending_way[7]) {
            updSet->or_mask.reset(0);
            updSet->or_mask.reset(4);
            updSet->or_mask.reset(5);
            updSet->or_mask.reset(6);
            updSet->and_mask.reset(0);
            updSet->and_mask.set(4);
            updSet->and_mask.set(5);
            updSet->and_mask.set(6);
        } else {
            if (updateWayID<6) {
                if (updSet->pending_way[4] && updSet->pending_way[5]) {
                    updSet->or_mask.set(4);
                    updSet->or_mask.reset(5);
                    updSet->and_mask.set(4);
                    updSet->and_mask.set(5);
                } else {
                    if (updSet->pending_way[4]) {
                        updSet->or_mask.set(5);
                        updSet->and_mask.set(5);
                    } else {
                        updSet->or_mask.reset(5);
                        updSet->and_mask.reset(5);
                        assert(updSet->pending_way[5]);
                    }
                }
            } else {
                if (updSet->pending_way[6] && updSet->pending_way[7]) {
                    updSet->or_mask.reset(4);
                    updSet->or_mask.reset(6);
                    updSet->and_mask.reset(4);
                    updSet->and_mask.set(6);
                } else {
                    if (updSet->pending_way[6]) {
                        updSet->or_mask.set(6);
                        updSet->and_mask.set(6);
                    } else {
                        assert(updSet->pending_way[7]);
                        updSet->or_mask.reset(6);
                        updSet->and_mask.reset(6);
                    }
                }
            }
        }
    }
}

void CACHE_REPLACEMENT_STATE::ClearpLRUMasks (UINT32 setIndex, INT32 updateWayID) {
    assert(setIndex < numsets);
    LINE_REPLACEMENT_STATE *updSet = &plru[setIndex];

    if (updateWayID<4) {
        if (!updSet->pending_way[4] || !updSet->pending_way[5] || !updSet->pending_way[6] || !updSet->pending_way[7]) {
            updSet->or_mask.reset(0);
            updSet->and_mask.set(0);
        } else {
            updSet->or_mask.reset(0);
            updSet->and_mask.reset(0);
        }
        if (updateWayID<2) {
            if (!updSet->pending_way[2] || !updSet->pending_way[3]) {
                updSet->or_mask.reset(1);
                updSet->and_mask.set(1);
            } else {
                updSet->or_mask.reset(1);
                updSet->and_mask.reset(1);
            }
            if (updateWayID == 0) {
                assert(!updSet->pending_way[0]);
                if (!updSet->pending_way[1]) {
                    updSet->or_mask.reset(2);
                    updSet->and_mask.set(2);
                } else {
                    updSet->or_mask.reset(2);
                    updSet->and_mask.reset(2);
                }
            } else {
                assert(!updSet->pending_way[1]);
                if (!updSet->pending_way[0]) {
                    updSet->or_mask.reset(2);
                    updSet->and_mask.set(2);
                } else {
                    updSet->or_mask.set(2);
                    updSet->and_mask.set(2);
                }
            }
        } else {
            if (!updSet->pending_way[0] || !updSet->pending_way[1]) {
                updSet->or_mask.reset(1);
                updSet->and_mask.set(1);
            } else {
                updSet->or_mask.set(1);
                updSet->and_mask.set(1);
            }
            if (updateWayID == 2) {
                assert(!updSet->pending_way[2]);
                if (!updSet->pending_way[3]) {
                    updSet->or_mask.reset(3);
                    updSet->and_mask.set(3);
                } else {
                    updSet->or_mask.reset(3);
                    updSet->and_mask.reset(3);
                }
            } else {
                assert(!updSet->pending_way[3]);
                if (!updSet->pending_way[2]) {
                    updSet->or_mask.reset(3);
                    updSet->and_mask.set(3);
                } else {
                    updSet->or_mask.set(3);
                    updSet->and_mask.set(3);
                }
            }
        }
    } else {
        assert((updateWayID<8));
        if (!updSet->pending_way[0] || !updSet->pending_way[1] || !updSet->pending_way[2] || !updSet->pending_way[3]) {
            updSet->or_mask.reset(0);
            updSet->and_mask.set(0);
        } else {
            updSet->or_mask.set(0);
            updSet->and_mask.set(0);
        }
        if (updateWayID<6) {
            if (!updSet->pending_way[6] || !updSet->pending_way[7]) {
                updSet->or_mask.reset(4);
                updSet->and_mask.set(4);
            } else {
                updSet->or_mask.reset(4);
                updSet->and_mask.reset(4);
            }
            if (updateWayID == 4) {
                assert(!updSet->pending_way[4]);
                if (!updSet->pending_way[5]) {
                    updSet->or_mask.reset(5);
                    updSet->and_mask.set(5);
                } else {
                    updSet->or_mask.reset(5);
                    updSet->and_mask.reset(5);
                }
            } else {
                assert(!updSet->pending_way[5]);
                if (!updSet->pending_way[4]) {
                    updSet->or_mask.reset(5);
                    updSet->and_mask.set(5);
                } else {
                    updSet->or_mask.set(5);
                    updSet->and_mask.set(5);
                }
            }
        } else {
            if (!updSet->pending_way[4] || !updSet->pending_way[5]) {
                updSet->or_mask.reset(4);
                updSet->and_mask.set(4);
            } else {
                updSet->or_mask.set(4);
                updSet->and_mask.set(4);
            }
            if (updateWayID == 6) {
                assert(!updSet->pending_way[6]);
                if (!updSet->pending_way[7]) {
                    updSet->or_mask.reset(6);
                    updSet->and_mask.set(6);
                } else {
                    updSet->or_mask.reset(6);
                    updSet->and_mask.reset(6);
                }
            } else {
                assert(!updSet->pending_way[7]);
                if (!updSet->pending_way[6]) {
                    updSet->or_mask.reset(6);
                    updSet->and_mask.set(6);
                } else {
                    updSet->or_mask.set(6);
                    updSet->and_mask.set(6);
                }
            }
        }
    }
}
