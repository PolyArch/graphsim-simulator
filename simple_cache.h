#ifndef SIMPLE_CACHE_H
#define SIMPLE_CACHE_H

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

#include <cassert>
#include "asim-defs.h"
#include "replacement_state.h"
#include "simple_cache_defs.h"

class SIMPLE_CACHE
{
  private:

    // parameters
    UINT32 numsets;
    UINT32 assoc;
    UINT32 threads;
    UINT32 linesize;
    UINT32 replPolicy;
    UINT32 numBanks;
    UINT32 numGlobalBanks;
    UINT32 numSetsPerBank;
    UINT32 numSetsPerGlobalBank;
    bool   usePrivateBanks;
    int    setHashing;
    
    LINE_STATE               **cache;
    CACHE_REPLACEMENT_STATE  *cacheReplState;

    // statistics
    COUNTER *lookups[ ACCESS_MAX ];
    COUNTER *misses[ ACCESS_MAX ];
    COUNTER *hits[ ACCESS_MAX ];

    // Lookup Parameters
    UINT32 lineShift;
    UINT32 indexShift;
    UINT32 bankShift;
    UINT32 indexMask;
    UINT32 bankMask;
    UINT32 globalBankShift;

    COUNTER mytimer; 
    string  CacheName;

  private:
    // Copy operations are private and not implemented to prevent copying
    SIMPLE_CACHE(const SIMPLE_CACHE&);
    SIMPLE_CACHE& operator=(const SIMPLE_CACHE&);


  public:

    SIMPLE_CACHE( UINT32 _cacheSize, UINT32 _assoc, UINT32 _tpc, UINT32 _linesize=64, UINT32 _pol=REPL_LRU, UINT32 _numGlobalBanks = 16, UINT32 _numBanks=4, bool _usePrivateBanks=false );
    // SIMPLE_CACHE( UINT32 _cacheSize, UINT32 _assoc, UINT32 _tpc, UINT32 _linesize=64, UINT32 _numBanks=4,
       //           UINT32 _pol=REPL_LRU, bool _usePrivateBanks=false, UINT32 _numGlobalBanks=16, int _setHashing=5,
         //         bool evict_miss=false);
    ~SIMPLE_CACHE();

    void   SetCacheName( string _name ) { CacheName = _name; } //
    void   SetCacheGlobalBankNum(UINT32 _num) { numGlobalBanks = _num; }

    // bool   CacheInspect( UINT32 tid , Addr_t PC, Addr_t paddr, UINT32 accessType, UINT32 privateBankID=0 );
    // bool   LookupAndFillCache( UINT32 tid, Addr_t PC, Addr_t paddr, UINT32 accessType, bool fillOnMiss=1, bool updateReplacement=true, UINT32 privateBankID=0, UINT64 cycle=0);
    bool   LookupAndFillCache( UINT32 tid, Addr_t PC, Addr_t paddr, UINT32 accessType, bool fillOnMiss=1, UINT32 privateBankID=0);
    INT32 CountPartialMatches(Addr_t paddr, Addr_t mask ); // Calculates partial tag matches
    LINE_STATE* LookupCache( Addr_t paddr, UINT32 accessType, bool updateReplacement=true, UINT32 privateBankID=0);  // hit/miss
    void   SetCacheSetHashing(int _num) { setHashing = _num; }
    LINE_STATE* FindVictim( Addr_t paddr, INT32 &wayID, UINT32 privateBankID=0);   // Find line to replace
    LINE_STATE* FindVictim_CheckValid( Addr_t paddr, INT32 &wayID, bool &isValid, UINT32 privateBankID=0);   // Find line to replace
    LINE_STATE* GetVictimLine( Addr_t paddr, INT32 wayID);   // Return victim line to replace

    INT32 GetLRUBits(Addr_t paddr, UINT32 privateBankID);

    void FillCache( Addr_t paddr, INT32 wayID, bool markDirty, bool updateReplacement=true, UINT32 privateBankID=0, UINT64 cycle=0);   // Find line at way provided by FindVictim
    void UpdateVictimReplacementState (Addr_t paddr, INT32 wayID, UINT32 privateBankID=0);   //Update replacement state for victim way given by FindVictim_CheckValid
    void ClearLineReplacementMask (Addr_t paddr, INT32 wayID, UINT32 privateBankID=0);
    void EvictLine (Addr_t paddr, INT32 wayID, UINT32 privateBankID=0);

/*
    bool   CacheInspect( UINT32 tid, Addr_t PC, Addr_t paddr, UINT32 accessType, UINT64 bankID, UINT32 privateBankID=0 );
    bool   LookupAndFillCache( UINT32 tid, Addr_t PC, Addr_t paddr, UINT32 accessType, UINT64 bankID=0, bool fillOnMiss=1, UINT32 privateBankID=0 );
    INT32 CountPartialMatches(Addr_t paddr, UINT64 bankID, Addr_t mask ); // Calculates partial tag matches 
    LINE_STATE* LookupCache( Addr_t paddr, UINT32 accessType, UINT64 bankID = 0, UINT32 privateBankID=0 );  // hit/miss 
    void   SetCacheSetHashing(int _num) { setHashing = _num; }
    LINE_STATE* FindVictim( Addr_t paddr, INT32 &wayID, UINT64 bankID = 0, UINT32 privateBankID=0 );   // Find line to replace



    void FillCache( Addr_t paddr, INT32 wayID, bool markDirty, UINT64 bankID, UINT32 privateBankID=0 );   // Find line at way provided by FindVictim
    */
    UINT32 getBankID( Addr_t addr ) { return (GetSetIndex(addr)/numSetsPerBank);};
    UINT32 getBankIDInterleaved( Addr_t addr ){return (GetSetIndex(addr)%numBanks);};
    UINT32 getBankIDRandomized( Addr_t addr );

    UINT32 getNumBanks(){return numBanks;};

    Addr_t GetLineAddr( Addr_t addr ) 
    { 
        return ((addr >> lineShift) << lineShift);
    }

    Addr_t GetTag( Addr_t addr)
    {
        return ((addr >> lineShift) << lineShift); // Tag is the line address
    }

    UINT32 GetSetIndex( Addr_t addr, UINT32 privateBankID=0 );
    UINT32 GetGlobalSetIndex( Addr_t addr, UINT64 bankID, UINT32 privateBankID=0 );
    ostream &   PrintStats(ostream &out);

    UINT32 getNumValidLines();
    UINT32 getNumDirtyLines();

    void InvalidateAddr( Addr_t paddr, UINT32 privateBankID=0);
    // FIXME: check what is this initializing
    void Initialize();
  private:

    INT32  LookupSet( UINT32 setIndex, Addr_t tag);
    void   InitCache();
    void   InitCacheReplacementState();
    void   DeleteCacheReplacementState();

    void   InitStats();

    // FIXME: check why is this removed?
    // INT32  LookupSet( UINT32 setIndex, Addr_t tag );
    INT32  GetVictimInSet( UINT32 tid, UINT32 setIndex, Addr_t PC, Addr_t paddr, UINT32 accessType );
    INT32  GetVictimInSet_CheckValid( UINT32 tid, UINT32 setIndex, Addr_t PC, Addr_t paddr, UINT32 accessType, bool &isValid);

    bool CSA_L2_EVICT_ON_MISS_FIX;
  public:


    void setReplPolicy(int repl_policy) {
        replPolicy = repl_policy;
        InitCacheReplacementState();
    }

    // Statistics related functions
    COUNTER ThreadDemandLookupStats( UINT32 tid )
    {
        COUNTER stat = 0;
        for(UINT32 a=0; a<=ACCESS_STORE; a++) stat  += lookups[a][tid];
        return stat;
    }

    COUNTER ThreadDemandMissStats( UINT32 tid )
    {
        COUNTER stat = 0;
        for(UINT32 a=0; a<=ACCESS_STORE; a++) stat  += misses[a][tid];
        return stat;
    }
    
    COUNTER ThreadDemandHitStats( UINT32 tid )
    {
        COUNTER stat = 0;
        for(UINT32 a=0; a<=ACCESS_STORE; a++) stat  += hits[a][tid];
        return stat;
    }

};

#endif
