#include "simple_cache.h"
#include "csa_util.h"
#include "csa_constants.h"
#include <csignal>
#include <math.h>

namespace MEMTRACE {
    extern Knob2<bool> knob_csasim_verbose;
};

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


// Works by finding position of MSB set.
static inline INT32 FloorLog2(UINT32 n)
{
    INT32 p = 0;

    if (n == 0) return -1;

    if (n & 0xffff0000) { p += 16; n >>= 16; }
    if (n & 0x0000ff00) { p +=  8; n >>=  8; }
    if (n & 0x000000f0) { p +=  4; n >>=  4; }
    if (n & 0x0000000c) { p +=  2; n >>=  2; }
    if (n & 0x00000002) { p +=  1; }

    return p;
}

// Works by finding position of MSB set.
// @returns -1 if n == 0.
static inline INT32 CeilLog2(UINT32 n)
{
    return FloorLog2(n - 1) + 1;
}

string cache_access_names[] =
{
    "IFETCH   ",
    "LOAD     ",
    "STORE    ",
    "-NOP0-   ",
    "-NOP1-   ",
    "PREFETCH ",
    "WRITEBACK",
    "RFO      ",
    "UPGRADE  ",
    "SNOOP    ",
};

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The constructor for the cache with appropriate cache parameters as args    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
SIMPLE_CACHE::SIMPLE_CACHE( UINT32 _cacheSize, UINT32 _assoc, UINT32 _tpc, UINT32 _linesize, UINT32 _pol, UINT32 _numGlobalBanks, UINT32 _numBanks, bool _usePrivateBanks )
{
// Start off with empty cache and replacement state

    // Start off with empty cache and replacement state
    cache          = NULL;
    cacheReplState = NULL;

    // Initialize parameters to the cache
    numsets  = _cacheSize / (_linesize * _assoc);
    assoc    = _assoc;
    threads  = _tpc;
    linesize = _linesize;
    numBanks = _numBanks;
    numSetsPerBank = numsets/numBanks;
    numGlobalBanks = _numGlobalBanks;
    numSetsPerGlobalBank = numsets / numGlobalBanks;


    usePrivateBanks = _usePrivateBanks;
    setHashing = 0; //8; // 5; // set to default,  not sure what it is used for... _setHashing;
    numGlobalBanks = 1;

    // FIXME: what is this, some new coherence state?
    // CSA_L2_EVICT_ON_MISS_FIX = evict_miss;

    replPolicy = _pol;

    // FIXME: no need to separate initialize
    // Initialize the cache
    InitCache();

    // std::cout << "sending to init cahce repl state\n";

    // Initialize Replacement State
    InitCacheReplacementState();

    // Initialize the stats
    InitStats();
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The destructor for the cache                                               //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
SIMPLE_CACHE::~SIMPLE_CACHE()
{
    //if (MEMTRACE::knob_csasim_verbose) {
    //    fprintf(stderr, "csasim info: deleting the cache model\n");
    //}

    for(UINT32 setIndex=0; setIndex<numsets; setIndex++)
    {
        delete [] cache[ setIndex ];
    }
    delete [] cache;
    cache = NULL;

    for(UINT32 i=0; i<ACCESS_MAX; i++)
    {
        delete [] lookups[i];
        lookups[i] = NULL;

        delete [] misses[i];
        misses[i] = NULL;

        delete [] hits[i];
        hits[i] = NULL;
    }

    DeleteCacheReplacementState();
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The function initializes the cache hardware and structures                 //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void SIMPLE_CACHE::InitCache()
{

    // std::cout << "Initialize cache\n";
    // Initialize the Cache Access Functions
    lineShift  = FloorLog2( linesize );
    // indexShift = FloorLog2( numSetsPerGlobalBank );
    indexShift = FloorLog2( numsets);
    bankShift  = FloorLog2( numBanks );   
    indexMask  = (1 << indexShift) - 1;
    bankMask  = (1 << bankShift) - 1;
    globalBankShift = FloorLog2( numGlobalBanks);

    // Create the cache structure (first create the sets)
    cache = new LINE_STATE* [ numsets ];

    // ensure that we were able to create cache
    assert(cache);

    // If we were able to create the sets, now create the ways
    for(UINT32 setIndex=0; setIndex<numsets; setIndex++) 
    {
        cache[ setIndex ] = new LINE_STATE[ assoc ];

        // Initialize the cache ways
        for(UINT32 way=0; way<assoc; way++) 
        {
            cache[ setIndex ][ way ].tag   = 0xdeaddead;
            cache[ setIndex ][ way ].valid = false;

            // TODO: this looks like the advanced feature which i might not want to upgrade
            // Ok cool, don't include any coherence kind state

            cache[ setIndex ][ way ].exclusive = false;
            cache[ setIndex ][ way ].dirty = false;
            cache[ setIndex ][ way ].shared_upgrading = false;
	    //            cache[ setIndex ][ way ].sharing_dir   = 0;
        }
    }

    // Initialize cache access timer
    mytimer = 0;

    CSA_T2("CSA cache configuration:"
	   << " numsets: " << numsets
	   << " assoc: " << assoc
	   << " linesize: " << linesize
	   << " replPolicy: " << replPolicy
	   << " numBanks: " << numBanks
	   << " numGlobalBanks: " << numGlobalBanks
	   << " numSetsPerBank: " << numSetsPerBank
	   << " numSetsPerGlobalBank: " << numSetsPerGlobalBank
	   << " usePrivateBanks: " << usePrivateBanks
	   << " setHashing: " << setHashing
	   << " lineShift: " << lineShift
	   << " indexShift: " << indexShift
	   << " bankShift: " << bankShift
	   << " indexMask: " << indexMask
	   << " bankMask: " << bankMask
	   << " globalBankShift: " << globalBankShift
	   << " CacheName: " << CacheName);
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The function initializes the stats for the cache                           //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void SIMPLE_CACHE::InitStats()
{
    for(UINT32 i=0; i<ACCESS_MAX; i++) 
    {
        lookups[i] = new COUNTER[ threads ];
        misses[i]  = new COUNTER[ threads ];
        hits[i]    = new COUNTER[ threads ];

        for(UINT32 t=0; t<threads; t++) 
        {
            lookups[i][t] = 0;
            misses[i][t]  = 0;
            hits[i][t]    = 0;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The function prints the statistics for the cache                           //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
ostream & SIMPLE_CACHE::PrintStats(ostream &out)
{

    COUNTER totLookups = 0, totMisses = 0, totHits = 0;

    out<<"=========================================================="<<endl;
    out<<"============= Cache Statistics: "<<CacheName<<" =========="<<endl;
    out<<"=========================================================="<<endl;
    out<<endl;
    out<<endl;    
    out<<"Cache Configuration: "<<endl;
    out<<"\tCache Size:     "<<(numsets*assoc*linesize/1024)<<"K"<<endl;
    out<<"\tLine Size:      "<<linesize<<"B"<<endl;
    out<<"\tAssociativity:  "<<assoc<<endl;
    out<<"\tTot # Sets:     "<<numsets<<endl;
    out<<"\tTot # Threads:  "<<threads<<endl;
    

    out<<endl;
    out<<"Cache Statistics: "<<endl;
    out<<endl;
    
    for(UINT32 a=0; a<ACCESS_MAX; a++) 
    {

        totLookups = 0;
        totMisses = 0;
        totHits = 0;

        for(UINT32 t=0; t<threads; t++) 
        {
            totLookups += lookups[a][t];
            totMisses  += misses[a][t];
            totHits    += hits[a][t];
        }

        if( totLookups ) 
        {
            out<<"\t"<<cache_access_names[a]<<" Accesses:   "<<totLookups<<endl;
            out<<"\t"<<cache_access_names[a]<<" Misses:     "<<totMisses<<endl;
            out<<"\t"<<cache_access_names[a]<<" Hits:       "<<totHits<<endl;
            out<<"\t"<<cache_access_names[a]<<" Miss Rate:  "<<((double)totMisses/(double)totLookups)*100.0<<endl;

            out<<endl;
        }
    }

    out<<endl;
    out<<"Per Thread Demand Reference Statistics: "<<endl;

    for(UINT32 t=0; t<threads; t++) 
    {
        totLookups = ThreadDemandLookupStats(t);
        totMisses  = ThreadDemandMissStats(t);
        totHits    = ThreadDemandHitStats(t);

        if( totLookups )
        {
            out<<"\tThread: "<<t<<" Lookups: "<<totLookups<<" Misses: "<<totMisses
                <<" Miss Rate: "<<((double)totMisses/(double)totLookups)*100.0<<endl;
        }
    }
    out<<endl;

    cacheReplState->PrintStats( out );
     
    return out;
}

// This is not used now. It seems to be related to private banks. Might need to get rid of hard-coded 256 before using
UINT32 SIMPLE_CACHE::getBankIDRandomized( Addr_t addr )
{
  // This table derives from the LEAP hashBits function found in
  // leap/modules/leap/libraries/librl/base/hash-bits.bsv
  /*
  static UINT8 hashTable[256] = { 0, 7, 14,  9, 28, 27, 18, 21, 56,
				  63, 54, 49, 36, 35, 42, 45,112,
				  119,126,121,108,107, 98,101, 72,
				  79, 70, 65, 84, 83, 90, 93,224,
				  231,238,233,252,251,242,245,216,
				  223,214,209,196,195,202,205,144,
				  151,158,153,140,139,130,133,168,
				  175,166,161,180,179,186,189,199,
				  192,201,206,219,220,213,210,255,
				  248,241,246,227,228,237,234,183,
				  176,185,190,171,172,165,162,143,
				  136,129,134,147,148,157,154, 39,
				  32, 41, 46, 59, 60, 53, 50, 31,
				  24, 17, 22,  3,  4, 13, 10, 87,
				  80, 89, 94, 75, 76, 69, 66,111,
				  104, 97,102,115,116,125,122,137,
				  142,135,128,149,146,155,156,177,
				  182,191,184,173,170,163,164,249,
				  254,247,240,229,226,235,236,193,
				  198,207,200,221,218,211,212,105,
				  110,103, 96,117,114,123,124, 81,
				  86, 95, 88, 77, 74, 67, 68, 25,
				  30, 23, 16,  5,  2, 11, 12, 33,
				  38, 47, 40, 61, 58, 51, 52, 78,
				  73, 64, 71, 82, 85, 92, 91,118,
				  113,120,127,106,109,100, 99, 62,
				  57, 48, 55, 34, 37, 44, 43,  6,
				  1,  8, 15, 26, 29, 20, 19,174,
				  169,160,167,178,181,188,187,150,
				  145,152,159,138,141,132,131,222,
				  217,208,215,194,197,204,203,230,
				  225,232,239,250,253,244,243};

    static UINT8 hashTableA[256] = {0, 141,151, 26,163, 46, 52,185,203,
				   70, 92,209,104,229,255,114, 27,
				   150,140,  1,184, 53, 47,162,208,
				   93, 71,202,115,254,228,105, 54,
				   187,161, 44,149, 24,  2,143,253,
				   112,106,231, 94,211,201, 68, 45,
				   160,186, 55,142,  3, 25,148,230,
				   107,113,252, 69,200,210, 95,108,
				   225,251,118,207, 66, 88,213,167,
				   42, 48,189,  4,137,147, 30,119,
				   250,224,109,212, 89, 67,206,188,
				   49, 43,166, 31,146,136,  5, 90,
				   215,205, 64,249,116,110,227,145,
				   28,  6,139, 50,191,165, 40, 65,
				   204,214, 91,226,111,117,248,138,
				   7, 29,144, 41,164,190, 51,216,
				   85, 79,194,123,246,236, 97, 19,
				   158,132,  9,176, 61, 39,170,195,
				   78, 84,217, 96,237,247,122,  8,
				   133,159, 18,171, 38, 60,177,238,
				   99,121,244, 77,192,218, 87, 37,
				   168,178, 63,134, 11, 17,156,245,
				   120, 98,239, 86,219,193, 76, 62,
				   179,169, 36,157, 16, 10,135,180,
				   57, 35,174, 23,154,128, 13,127,
				   242,232,101,220, 81, 75,198,175,
				   34, 56,181, 12,129,155, 22,100,
				   233,243,126,199, 74, 80,221,130,
				   15, 21,152, 33,172,182, 59, 73,
				   196,222, 83,234,103,125,240,153,
				   20, 14,131, 58,183,173, 32, 82};
*/
    static UINT8 hashTableB[256] = {   0,49, 98, 83,196,245,166,151,185,
				       136,219,234,125, 76, 31, 46, 67,
				       114, 33, 16,135,182,229,212,250,
				       203,152,169, 62, 15, 92,109,134,
				       183,228,213, 66,115, 32, 17, 63,
				       14, 93,108,251,202,153,168,197,
				       244,167,150,  1, 48, 99, 82,124,
				       77, 30, 47,184,137,218,235, 61,
				       12, 95,110,249,200,155,170,132,
				       181,230,215, 64,113, 34, 19,126,
				       79, 28, 45,186,139,216,233,199,
				       246,165,148,  3, 50, 97, 80,187,
				       138,217,232,127, 78, 29, 44,  2,
				       51, 96, 81,198,247,164,149,248,
				       201,154,171, 60, 13, 94,111, 65,
				       112, 35, 18,133,180,231,214,122,
				       75, 24, 41,190,143,220,237,195,
				       242,161,144,  7, 54,101, 84, 57,
				       8, 91,106,253,204,159,174,128,
				       177,226,211, 68,117, 38, 23,252,
				       205,158,175, 56,  9, 90,107, 69,
				       116, 39, 22,129,176,227,210,191,
				       142,221,236,123, 74, 25, 40,  6,
				       55,100, 85,194,243,160,145, 71,
				       118, 37, 20,131,178,225,208,254,
				       207,156,173, 58, 11, 88,105,  4,
				       53,102, 87,192,241,162,147,189,
				       140,223,238,121, 72, 27, 42,193,
				       240,163,146,  5, 52,103, 86,120,
				       73, 26, 43,188,141,222,239,130,
				       179,224,209, 70,119, 36, 21, 59};

    // We select hash table B because it appears to have the best
    // result for strided consecutive access, but a more thorough
    // evaluation would be useful.
    return (hashTableB[GetSetIndex(addr)%256]%numBanks);
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The function slects a victim for the given set index. We enforce that      //
// we first fill all invalid entries. Once all invalid entries are filled     //
// the replacement policy is consulted to find the victim                     //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
INT32
SIMPLE_CACHE::GetVictimInSet_CheckValid(UINT32 tid, UINT32 setIndex, Addr_t PC, Addr_t paddr, UINT32 accessType, bool &isValid) {
    // Get pointer to replacement state of current set
    LINE_STATE *vicSet = cache[ setIndex ];

    // First find and fill invalid lines
    for (UINT32 way=0; way<assoc; way++) {
        if ((vicSet[way].valid == false) && (vicSet[way].evict_state != PENDING_EVICT)) {
            isValid = false;
            return way;
        }
    }

    // If no invalid lines, then replace based on replacement policy
    isValid = true;
    return cacheReplState->GetVictimInSet(tid, setIndex, vicSet, assoc, PC, paddr, accessType);
}

// setIndex in the parameter list is the index in the whole 2MB cache, not the index in each cache bank!!
INT32
SIMPLE_CACHE::GetVictimInSet(UINT32 tid, UINT32 setIndex, Addr_t PC, Addr_t paddr, UINT32 accessType) {
    // Get pointer to replacement state of current set
    LINE_STATE *vicSet = cache[ setIndex ];

    // First find and fill invalid lines
    for (UINT32 way=0; way<assoc; way++) {
        if ((vicSet[way].valid == false) && (vicSet[way].evict_state != PENDING_EVICT)) {
            return way;
        }
    }

    // If no invalid lines, then replace based on replacement policy
    return cacheReplState->GetVictimInSet(tid, setIndex, vicSet, assoc, PC, paddr, accessType);
}

// FIXME: input parameters for partial matches should be changed
////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// Counts the number of partial tag matches in a set                          //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
INT32
SIMPLE_CACHE::CountPartialMatches(Addr_t paddr, Addr_t mask) {
    INT32 setIndex = GetSetIndex(paddr);
    INT32 matches = 0;
    Addr_t tag = GetTag(paddr);
    // Get pointer to current set
    LINE_STATE *currSet = cache[ setIndex ];

    // Find Tag
    for (UINT32 way=0; way<assoc; way++) {
        if (currSet[way].valid && ((mask & currSet[way].tag) == (mask & tag)) && (currSet[way].evict_state!= EVICTED)) {
            matches++;
        }
    }

    return matches;
}
////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The function looks up the set for the tag and returns physical way index   //
// if the tag was a hit. Else returns -1 if it was a miss.                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
// setIndex in the parameter list is the set index in the whole 2MB cache, not per bank
INT32 SIMPLE_CACHE::LookupSet( UINT32 setIndex, Addr_t tag )
{
    // std::cout << "Initial search for tag in the assoc, hit/miss after this\n";
    // Get pointer to current set
    LINE_STATE *currSet = cache[ setIndex ];

    // Find Tag
    for(UINT32 way=0; way<assoc; way++) 
    {
        // FIXME: doesn't check if the state is evicted, looks like it ids for the latency?
        // check where evicted
        if( currSet[way].valid && (currSet[way].tag == tag) ) 
        {
            return way;
        }
    }

    // std::cout << "miss\n";

    // If not found, return -1
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// The function inspects the cache to see if the tag exists in the cache      //
// This function is used by the timing model for modeling latencies           //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
// This is not used now
/*bool SIMPLE_CACHE::CacheInspect( UINT32 tid, Addr_t PC, Addr_t paddr, UINT32 accessType, UINT64 bankID, UINT32 privateBankID )
{

  // UINT32 setIndex = GetSetIndex( paddr, privateBankID ) + numSetsPerGlobalBank * bankID;  // Get the set index
  UINT32 setIndex = GetSetIndex(paddr, privateBankID);  // Get the set index
  Addr_t tag      = GetTag( paddr );       // Determine Cache Tag

    INT32 wayID     = LookupSet( setIndex, tag );

    // if wayID = -1, miss, else it is a hit
    return (wayID != -1);
}*/

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is responsible for looking up and filling a line in the      //
// cache. If the result is a miss, the function consults the replacement      //
// policy for a victim candidate, inserts the missing line in the cache       //
// and consults the replacement policy for replacement state update. If       //
// the result was a cache hit, the replacement policy again is consulted      //
// to determine how to update the replacement state.                          //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
// This is not used now
bool SIMPLE_CACHE::LookupAndFillCache( UINT32 tid, Addr_t PC, Addr_t paddr, UINT32 accessType, bool fillOnMiss, UINT32 privateBankID )
{

    LINE_STATE *currLine = NULL;

    // for modeling LRU
    ++mytimer;     
    cacheReplState->IncrementTimer();

    // manage stats for cache
    lookups[ accessType ][ tid ]++;

    // Process request
    bool  hit       = true;
    UINT32 setIndex = GetSetIndex( paddr, privateBankID ); // + numSetsPerGlobalBank * bankID;  // Get the set index
    Addr_t tag      = GetTag( paddr );       // Determine Cache Tag

    // Lookup the cache set to determine whether line is already in cache or not
    INT32 wayID     = LookupSet( setIndex, tag );

    if( wayID == -1 ) 
    {
        hit = false;

        // Update Stats
        misses[ accessType ][ tid ]++;

        // if no fill on miss, return immediately
        if( !fillOnMiss ) return hit;

        // get victim line to replace (wayID = -1, then bypass)
        wayID     = GetVictimInSet( tid, setIndex, PC, paddr, accessType );

        if( wayID != -1 )
        {
            currLine  = &cache[ setIndex ][ wayID ];

            // Update the line state accordingly
            currLine->valid          = true;
            currLine->tag            = tag;
            currLine->exclusive      = IS_STORE( accessType ) || accessType == ACCESS_RFO;
            currLine->dirty          = IS_STORE( accessType );
	    //            currLine->sharing_dir    = (1<<tid);

            // Update Replacement State
            cacheReplState->UpdateReplacementState( setIndex, wayID, currLine, tid, PC, accessType, hit );
        }        
    }
    else 
    {
        // Update Stats
        hits[ accessType ][ tid ]++;

        // get pointer to cache line we hit
        currLine         = &cache[ setIndex ][ wayID ];

        // Update the line state accordingly
        currLine->exclusive     |= IS_STORE( accessType ) || accessType == ACCESS_RFO;
        currLine->dirty         |= IS_STORE( accessType );
	//        currLine->sharing_dir   |= (1<<tid);

        // Update Replacement State
        if( accessType != ACCESS_WRITEBACK ) 
        {
            cacheReplState->UpdateReplacementState( setIndex, wayID, currLine, tid, PC, accessType, hit );
        }
    }        

    return hit;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is responsible for invlidating a perticular address in the   //
// cache                                                                      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void
SIMPLE_CACHE::InvalidateAddr(Addr_t paddr, UINT32 privateBankID) {
    // Process request
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);  // Get the set index
    Addr_t tag = GetTag(paddr);       // Determine Cache Tag

    // Lookup the cache set to determine whether line is already in cache or not
    INT32 wayID = LookupSet(setIndex, tag);

    assert(wayID!=-1 && "Trying to invalidate a line which doesnot exist in the cache");

    // get pointer to cache line we hit
    cache[ setIndex ][ wayID ].valid = false;
}


////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is responsible for creating the cache replacement state      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void SIMPLE_CACHE::InitCacheReplacementState()
{
    // std::cout << "Trying to initialize repl state during cache const:\n";
    cacheReplState = new CACHE_REPLACEMENT_STATE( numsets, assoc, replPolicy );
}

void SIMPLE_CACHE::DeleteCacheReplacementState()
{
    delete cacheReplState;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is responsible for looking up cache and return line          //
// Returns NULL is miss, line if hit                                          //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
// LINE_STATE* SIMPLE_CACHE::LookupCache( Addr_t paddr, UINT32 accessType, UINT64 bankID, UINT32 privateBankID )
// {
LINE_STATE*
SIMPLE_CACHE::LookupCache(Addr_t paddr, UINT32 accessType, bool updateReplacement, UINT32 privateBankID) {
    LINE_STATE *currLine = NULL;

    // for modeling LRU
    ++mytimer;
    cacheReplState->IncrementTimer();
    // manage stats for cache
    UINT32 tid = 0;
    lookups[ accessType ][ tid ]++;

    // Process request
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);  // Get the set index
    Addr_t tag = GetTag(paddr);       // Determine Cache Tag
    INT32 wayID = LookupSet(setIndex, tag); // Determine whether line is already in cache or not

    // If hit
    if (wayID != -1) {
        // Update Stats
        hits[ accessType ][ tid ]++;

        // get pointer to cache line we hit
        currLine = &cache[ setIndex ][ wayID ];

        // Update Replacement State
        if ((accessType != ACCESS_WRITEBACK) && updateReplacement) {
            cacheReplState->UpdateReplacementState(setIndex, wayID, currLine, 0/*tid*/, 0/*PC*/, accessType, true);
        }
    } else {
        // collect stat for misses
        // Update Stats
        misses[ accessType ][ tid ]++;
    }

    return currLine;
}

void
SIMPLE_CACHE::UpdateVictimReplacementState (Addr_t paddr, INT32 wayID, UINT32 privateBankID) {
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);
    LINE_STATE *currLine = &cache[ setIndex ][ wayID ];
    assert((currLine->evict_state == READY) && "Line chosen for eviction has a pending evict/has been evicted");
    currLine->evict_state = PENDING_EVICT;

    if (!currLine->valid) {
        cacheReplState->SetPseudoLRUPedingWay(setIndex, wayID);
    }
}

void
SIMPLE_CACHE::EvictLine (Addr_t paddr, INT32 wayID, UINT32 privateBankID) {
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);
    LINE_STATE *currLine = &cache[ setIndex ][ wayID ];

    assert((currLine->evict_state == PENDING_EVICT) && "Trying to evict a line which was not marked for eviction");
    currLine->evict_state = EVICTED;
    currLine->valid =true;
}

void
SIMPLE_CACHE::ClearLineReplacementMask (Addr_t paddr, INT32 wayID, UINT32 privateBankID) {
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);
    LINE_STATE *currLine = &cache[ setIndex ][ wayID ];

    assert((currLine->evict_state == PENDING_EVICT) && "Trying to clear replacement state for a line which was not marked for eviction");
    if (currLine->evict_state != PENDING_EVICT) {
        std::raise(SIGINT);
    }

    cacheReplState->ClearPseudoLRUPedingWay(setIndex, wayID);
    currLine->evict_state = READY;
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is responsible for finding victim and where it is            //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
LINE_STATE*
SIMPLE_CACHE::FindVictim_CheckValid(Addr_t paddr, INT32 &wayID ,bool &isValid,  UINT32 privateBankID) {
    LINE_STATE *victimLine = NULL;

    // Process request
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);  // Get the set index
    Addr_t tag = GetTag(paddr);       // Determine Cache Tag
    wayID = LookupSet(setIndex, tag); // Determine whether line is already in cache or not

    if (wayID == -1) { // address is not in cache
        // get victim line to replace
        wayID = GetVictimInSet_CheckValid(0, setIndex, 0, paddr, 0, isValid);

        if (wayID != -1) { // Found victim
            victimLine = &cache[ setIndex ][ wayID ];
        } else {
            victimLine = NULL;
        }
    } else {
        assert(0 && "The address you are trying to find victim for is already in cache\n");
    }
    return victimLine;
}

LINE_STATE*
SIMPLE_CACHE::GetVictimLine(Addr_t paddr, INT32 wayID) {   // Return victim line to replace
    // Process request
    UINT32 setIndex = GetSetIndex(paddr, 0);  // Get the set index

    // Get pointer to replacement state of current set
    LINE_STATE *victimLine = cache[ setIndex ];

    return victimLine;
}

LINE_STATE*
SIMPLE_CACHE::FindVictim(Addr_t paddr, INT32 &wayID , UINT32 privateBankID) {
    LINE_STATE *victimLine = NULL;

    // Process request
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);  // Get the set index
    Addr_t tag = GetTag(paddr);       // Determine Cache Tag
    wayID = LookupSet(setIndex, tag); // Determine whether line is already in cache or not

    if (wayID == -1) { // address is not in cache
        // get victim line to replace
        wayID = GetVictimInSet(0, setIndex, 0, paddr, 0);

        if (wayID != -1) { // Found victim
            victimLine = &cache[ setIndex ][ wayID ];
        } else {
            victimLine = NULL;
        }
    } else {
        assert(0 && "The address you are trying to find victim for is already in cache\n");
    }
    return victimLine;
}

INT32
SIMPLE_CACHE::GetLRUBits(Addr_t paddr, UINT32 privateBankID )
{
    UINT32 setIndex = GetSetIndex( paddr, privateBankID );
    Addr_t tag      = GetTag( paddr );
    INT32 wayID     = LookupSet( setIndex, tag );

    return cacheReplState->GetLRUBits( setIndex, wayID);
}

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
// This function is responsible for filling cache at the wayID provided by    //
// FindVictim                                                                 //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
void
SIMPLE_CACHE::FillCache(Addr_t paddr, INT32 wayID, bool markDirty, bool updateReplacement , UINT32 privateBankID, UINT64 cycle) {
    // Process request
    UINT32 setIndex = GetSetIndex(paddr, privateBankID);  // Get the set index
    Addr_t tag = GetTag(paddr);       // Determine Cache Tag

    if (wayID < 0 || wayID >= (int)assoc) {
        assert(0 && "Something is wrong with way assoc\n");
    }

    LINE_STATE *victimLine = &cache[ setIndex ][ wayID ];

    // for modeling LRU
    ++mytimer;
    cacheReplState->IncrementTimer();

    // Update the line state accordingly
    victimLine->valid = true;
    victimLine->evict_state = READY;
    victimLine->tag = tag;
    victimLine->dirty = markDirty;

    // Update Replacement State
    if (updateReplacement){
        cacheReplState->UpdateReplacementState(setIndex, wayID, victimLine, 0, 0, 0, 0);
    }
}


// Return a good prime number for hashing
UINT64 good_primes_for_set_hashing(UINT64 number) {
  if (number <= pow(2, 6))       return 53;
  else if (number <= pow(2, 7))  return 97;
  else if (number <= pow(2, 8))  return 193;
  else if (number <= pow(2, 9))  return 389;
  else if (number <= pow(2, 10)) return 769;
  else if (number <= pow(2, 11)) return 1543;
  else if (number <= pow(2, 12)) return 3079;
  else if (number <= pow(2, 13)) return 6151;
  else if (number <= pow(2, 14)) return 12289;
  else if (number <= pow(2, 15)) return 24593;
  else if (number <= pow(2, 16)) return 49157;
  else if (number <= pow(2, 17)) return 98317;
  else if (number <= pow(2, 18)) return 196613;
  else if (number <= pow(2, 19)) return 393241;
  else if (number <= pow(2, 20)) return 786433;
  else if (number <= pow(2, 21)) return 1572869;
  else if (number <= pow(2, 22)) return 3145739;
  else if (number <= pow(2, 23)) return 6291469;
  else if (number <= pow(2, 24)) return 12582917;
  else if (number <= pow(2, 25)) return 25165843;
  else if (number <= pow(2, 26)) return 50331653;
  else if (number <= pow(2, 27)) return 100663319;
  else if (number <= pow(2, 28)) return 201326611;
  else if (number <= pow(2, 29)) return 402653189;
  else if (number <= pow(2, 30)) return 805306457;
  else                           return 1610612741;
}

typedef unsigned char UCHAR_BYTE;       // 8-bit unsigned entity.
typedef UCHAR_BYTE *  P_UCHAR_BYTE;     // Pointer to BYTE.
UINT64 crc_poly_for_set_hashing = 0xC96C5795D7870F42;

UINT64 crc_table_for_set_hashing[256];

void generate_crc_table_for_set_hashing()
{
  for (int i = 0; i < 256; ++i)
    {
      UINT64 crc = i;

      for (int j = 0; j < 8; ++j)
	{
	  // is current coefficient set?
	  if (crc & 1)
	    {
	      // yes, then assume it gets zero'd (by implied x^64 coefficient of dividend)
	      crc >>= 1;

	      // and add rest of the divisor
	      crc ^= crc_poly_for_set_hashing;
	    }
	  else
	    {
	      // no? then move to next coefficient
	      crc >>= 1;
	    }
	}

      crc_table_for_set_hashing[i] = crc;
    }
}

UINT64 calculate_crc_for_set_hashing(P_UCHAR_BYTE stream, int n)
{

  UINT64 crc = 0;

  for (int i = 0; i < n; ++i)
    {
      UCHAR_BYTE index = (UCHAR_BYTE)(stream[i] ^ crc);
      UINT64 lookup = crc_table_for_set_hashing[index];

      crc >>= 8;
      crc ^= lookup;
    }

  return crc;
}

UINT32 SIMPLE_CACHE::GetSetIndex( Addr_t addr, UINT32 privateBankID) 
{ 
  // Skew set indices
  globalBankShift = FloorLog2( numGlobalBanks);

  Addr_t set_hash_addr = addr >> (lineShift + globalBankShift);
  Addr_t set_hash_index = 0;

  if (setHashing == 0) {
    set_hash_index = set_hash_addr;
  }
  else if (setHashing == 1) {
    // Division method (Cormen): Choose a prime m that isn't close to a power of 2
    // h(k) = k mod m. Works badly for many types of patterns in the input data
    UINT64 prime = good_primes_for_set_hashing(set_hash_addr);
    set_hash_index = (set_hash_addr % prime);
  }
  else if (setHashing == 2) {
    // Knuth Variant on Division: h(k) = k(k+3) mod m
    // Supposedly works much better than the raw division method.
    UINT64 prime = good_primes_for_set_hashing(set_hash_addr);
    set_hash_index = ((set_hash_addr * (set_hash_addr + 3)) % prime);
  }
  else if (setHashing == 3) {
    // Generated by www.random.org
    UINT64 key = 0xfeeb3a0b9337b84a;
    set_hash_index = (set_hash_addr ^ key);
  }
  else if (setHashing == 4) {
    generate_crc_table_for_set_hashing();
	  
    UCHAR_BYTE input_stream[8];
	  
    input_stream[0] = (set_hash_addr >> 56) & 0xFF;
    input_stream[1] = (set_hash_addr >> 48) & 0xFF;
    input_stream[2] = (set_hash_addr >> 40) & 0xFF;
    input_stream[3] = (set_hash_addr >> 32) & 0xFF;
    input_stream[4] = (set_hash_addr >> 24) & 0xFF;
    input_stream[5] = (set_hash_addr >> 16) & 0xFF;
    input_stream[6] = (set_hash_addr >> 8) & 0xFF;
    input_stream[7] = set_hash_addr & 0xFF;
	  
    set_hash_index = calculate_crc_for_set_hashing(input_stream, 8);
  }
  else if (setHashing == 5) {
    set_hash_index = addr >> lineShift;
  }
  else if (setHashing == 6) {
    // Assume the physical address is 64 bits
    UINT64 phys_addr_bits = 64 - FloorLog2(linesize) - FloorLog2(numGlobalBanks);
    for (UINT64 shift = 0; shift < phys_addr_bits; shift += log2(numsets)) {
      set_hash_index ^= (set_hash_addr >> shift) & (numsets - 1);
    }
  }
  /* setHashing #7 
     Set Index Bit[0] = bits 7 XOR 22 XOR 23
     Set Index Bit[1] = bits 8 XOR 21 XOR 24
     Set Index Bit[2] = bits 9 XOR 20 XOR 25
     Set Index Bit[3] = bits 10 XOR 19 XOR 26
     Set Index Bit[4] = bits 11 XOR 18 XOR 27
     Set Index Bit[5] = bits 12 XOR 17 XOR 28
     Set Index Bit[6] = bits 13 XOR 16 XOR 29
     Set Index Bit[7] = bits 14 XOR 15 XOR 30
  */
  else if (setHashing == 7) {
    int bit1 = FloorLog2(linesize) + 1;
    int bit2 = bit1 + 2 * FloorLog2(numsets) - 1;
    int bit3 = bit1 + 2 * FloorLog2(numsets);
    UINT64 Set_ii = 0;
    for (int ii = 0; ii < FloorLog2(numsets); ii++) {
      //std::cout << "Set Index Bit[" << ii << "] = bits " << bit1 << " XOR " << bit2 << " XOR " << bit3 << "\n";
      Set_ii = ((addr >> bit1) & 1) ^ ((addr >> bit2) & 1) ^ ((addr >> bit3) & 1);
      set_hash_index += Set_ii << ii;
      bit1++;
      bit2--;
      bit3++;
    }
  }
  /* setHashing #8
     Set Index Bit[0] = bit 6
     Set Index Bit[1] = bits 8 XOR 21 XOR 24
     Set Index Bit[2] = bits 9 XOR 20 XOR 25
     Set Index Bit[3] = bits 10 XOR 19 XOR 26
     Set Index Bit[4] = bits 11 XOR 18 XOR 27
     Set Index Bit[5] = bits 12 XOR 17 XOR 28
     Set Index Bit[6] = bits 13 XOR 16 XOR 29
     Set Index Bit[7] = bits 14 XOR 15 XOR 30
  */
  else if (setHashing == 8) {
    int bit1 = FloorLog2(linesize) + 2;
    int bit2 = bit1 + 2 * FloorLog2(numsets) - 3;
    int bit3 = bit1 + 2 * FloorLog2(numsets);
    UINT64 Set_ii = 0;
    set_hash_index += ((addr >> 6) & 1);
    //std::cout << "Set Index Bit[0] = bit 6\n";
    for (int ii = 1; ii < FloorLog2(numsets); ii++) {
      //std::cout << "Set Index Bit[" << ii << "] = bits " << bit1 << " XOR " << bit2 << " XOR " << bit3 << "\n";
      Set_ii = ((addr >> bit1) & 1) ^ ((addr >> bit2) & 1) ^ ((addr >> bit3) & 1);
      set_hash_index += Set_ii << ii;
      bit1++;
      bit2--;
      bit3++;
    }
  }
  else if (setHashing == 9) { // SGU hashing type, FIXME: check if correct!!!
    int config_numsets=2;
    set_hash_index = addr >> lineShift;
    set_hash_index %= config_numsets;
  }
  else {
    ASIMERROR ("Not supported set hash type!\n");
  }
    
  if(usePrivateBanks)
    {
      return (((set_hash_index) & (indexMask ^ bankMask)) | privateBankID); 
    }
  else
    {
      //return ((addr >> lineShift) & indexMask); 
      CSA_T2("GetSetIndex: address " << hex << addr << dec
	     << " lineShift " << lineShift
	     << " globalBankShift " << globalBankShift
	     << " addr " << hex << addr
	     << " set_hash_addr " << set_hash_addr
	     << " set_hash_index " << set_hash_index << dec
	     << " return set index " << (set_hash_index & indexMask)
	     << " indexMask " << indexMask);
      return (set_hash_index & indexMask);
    }
}

UINT32 SIMPLE_CACHE::GetGlobalSetIndex( Addr_t addr, UINT64 bankID, UINT32 privateBankID) 
{
    return (GetSetIndex(addr, privateBankID) + numSetsPerGlobalBank * bankID); 
}

UINT32
SIMPLE_CACHE::getNumValidLines() {
    UINT32 numValidLines = 0;
    for (UINT32 setIndex = 0; setIndex < numsets; setIndex++) {
        for (UINT32 way = 0; way < assoc; way++) {
            if (cache[setIndex][way].valid == true)
                numValidLines++;
        }
    }
    return numValidLines;
}

UINT32
SIMPLE_CACHE::getNumDirtyLines() {
    UINT32 numDirtyLines = 0;
    for (UINT32 setIndex = 0; setIndex < numsets; setIndex++) {
        for (UINT32 way = 0; way < assoc; way++) {
            if (cache[setIndex][way].dirty == true)
                numDirtyLines++;
        }
    }
    return numDirtyLines;
}
