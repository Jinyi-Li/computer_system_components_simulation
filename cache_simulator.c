/*
 * cache_simulator.c - Simulate cache behaviors on Load and Store requests,
 * and count the numbers of hits, misses, evictions, dirty bytes in cache and 
 * evicted.
 *
 * Author: Jinyi Li
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include "cachelab.h"

#define MAX_UNSIGNED 0xFFFFFFFFFFFFFFFF

/* represent one line in cache. */
typedef struct{
    int validBit;
    int dirtyBit;
    int tag;
    long lruTimestamp;
}CacheLine;

/* represent one cache structure. */
typedef struct{
    /* s for 2^s sets */
    int s;
    /* e for E lines */
    int e;
    /* b for 2^b bytes per block */
    int b;
    CacheLine **sets;
}Cache;

/* global counters */
long hits = 0;
long misses = 0;
long evictions = 0;
long dirtyBlocksInCache = 0;
long dirtyBlocksEvicted = 0;
long operationCounter = 0;


/************ Helper methods ***********/

/* Parse string args to integers with special condition for E option. */
int parseInt(int isE, char *str){
    errno = 0;
    int res = (int) strtol(str, (char **)NULL, 10);
    if(errno == 0 && ((isE == 0 && res >= 0) || res > 0)) {
        return res;
    }
    // fail if s<0 or b<0 or E<=0
    return -1;
}

/* Parse address to get set index of the request. */
long getSetIndex(unsigned long address, int s, int b){
    // mask of s bits    
    unsigned long mask = (MAX_UNSIGNED) >> (64 - s);
    // take the [b+1, b+s] bits counting from the right
    return (address >> b) & mask;
}

/* Parse address to get tag of the request. */
long getTag(unsigned long address, int s, int b){
    // mask of t(= m-s-b) bits 
    unsigned long mask = (MAX_UNSIGNED) >> (64 - (64 - s - b));
    // take the [1, m - s - b] bits counting from the left
    return (address >> (s + b)) & mask;
}

/****************************************/


/******* Cache simulation methods *******/

/* Initialize cache with S sets and E lines in each set. */
Cache *initCache(int s, int e, int b){
    Cache *myCache = malloc(sizeof(Cache));
    if(!myCache){
        return NULL;
    }
    myCache->s = s;    
    myCache->e = e;
    myCache->b = b;
    // number of sets S = 2^s
    long numSets = (1 << s);  
    myCache->sets = malloc(sizeof(CacheLine*) * numSets);
    if(!myCache->sets){
        return NULL;
    }
    for(long i = 0; i < numSets; i++){
        // allocate mem of E lines to each set
        myCache->sets[i] = malloc(sizeof(CacheLine) * e);
        if(!myCache->sets[i]) {
            return NULL;
        }
        for(int j = 0; j < e; j++){
            // initially valid bit in each line is unset
            myCache->sets[i][j].validBit = 0;
            myCache->sets[i][j].dirtyBit = 0;
            myCache->sets[i][j].tag = -1;
            myCache->sets[i][j].lruTimestamp = 0;
        }
    }
    return myCache;
}

/* Free dynamically allocated memory in myCache. */
int freeCache(Cache *myCache){
    if(!myCache){
        return 1;
    }
    int numSets = (1 << myCache->s);
    for(long i = 0; i < numSets; i++){
        if(myCache->sets[i]){
            free(myCache->sets[i]);
        }        
    }
    free(myCache->sets);
    free(myCache);
    return 0;
}

/* Update cache content based on request. */
int updateCache(Cache *myCache, char accessType, long setIndex, long tag){
    char store = 'S';
    char load = 'L';
    operationCounter++;
    CacheLine *lines = myCache->sets[setIndex];
    long leastVisitedLine = operationCounter - 1;
    int lruLine = -1;
    // iterate each line to find target data
    for(long i = 0; i < myCache->e; i++){
        // cache hit
        if(lines[i].validBit == 1 && lines[i].tag == tag){
            hits++;
            lines[i].lruTimestamp = operationCounter;
            if(accessType == store && lines[i].dirtyBit == 0){
                lines[i].dirtyBit = 1;
                dirtyBlocksInCache++;
            }
            return 0;
        }
        // cache miss; copy data from lower mem to this usable line
        if(lines[i].tag == -1){
            misses++;
            lines[i].validBit = 1;
            lines[i].tag = tag;
            lines[i].lruTimestamp = operationCounter;
            // dirty cuz it writes new data to the block which contains old data loaded from memory
            if(accessType == store){
                lines[i].dirtyBit = 1;
                dirtyBlocksInCache++;
            }
            return 0;
        }
    }
    // cache miss and eviction
    misses++;
    // locate the least recent used line
    for(long i = 0; i < myCache->e; i++){
        if(lines[i].lruTimestamp < leastVisitedLine){
            leastVisitedLine = lines[i].lruTimestamp;
            lruLine = i;
        }
    }
    // if no LRU line, choose first one
    if(leastVisitedLine == operationCounter - 1){
        lruLine = 0;
    }
    evictions++;
    // evict the LRU line
    lines[lruLine].validBit = 1;
    lines[lruLine].tag = tag;
    lines[lruLine].lruTimestamp = operationCounter;
    if(accessType == load && lines[lruLine].dirtyBit == 1){
        dirtyBlocksEvicted++;
        lines[lruLine].dirtyBit = 0;
        dirtyBlocksInCache--;
    }else if(accessType == store && lines[lruLine].dirtyBit == 0){
        lines[lruLine].dirtyBit = 1;
        dirtyBlocksInCache++;
    }else if(accessType == store && lines[lruLine].dirtyBit == 1){
        dirtyBlocksEvicted++;
    }
    return 0;
}

/* Handle each request in trace file by simulating cache activities. */
int handleRequests(Cache *myCache, char accessType, unsigned long address, int size){
    long setIndex = getSetIndex(address, myCache->s, myCache->b);
    long tag = getTag(address, myCache->s, myCache->b);
    switch(accessType){
        case 'L':
        case 'S':
            // load and store are same cuz we only compute hits/misses/evictions.
            updateCache(myCache, accessType, setIndex, tag);            
            break;
        default:
            fprintf(stderr, "Not supported request type. %c\n", accessType);
            return 1;
    }
    return 0;
}

/* Parse lines in trace file to single requests and handle them. */
int handleMemoryTrace(Cache *myCache, char *filename){
    FILE *stream = fopen(filename, "r");
    // null pointer if fopen fails
    if(!stream){
        return -1;
    }    
    char accessType;
    unsigned long address;
    int size;
    while(fscanf(stream, " %c %lx, %d", &accessType, &address, &size) > 0){
        // handle memory request by line
        handleRequests(myCache, accessType, address, size);
    }
    int res = fclose(stream);
    return res;
}

/* Simulate cache behavior with given memory trace file. */
int simulateCacheOps(int s, int e, int b, char *tvalue){
    int res = 0;
    Cache *myCache = NULL;
    // initialize cache with sets and lines
    myCache = initCache(s, e, b);
    if(!myCache){
        fprintf(stderr, "Failed to initialize cache.\n");
    }
    res = handleMemoryTrace(myCache, tvalue);
    if(res != 0){
        return res;
    }
    // number of bytes per line B = 2^b
    int numBytesPerLine = 1 << b;
    printSummary(hits, misses, evictions, dirtyBlocksInCache * numBytesPerLine, 
        dirtyBlocksEvicted * numBytesPerLine);
    res = freeCache(myCache);
    return res;
}

/****************************************/

int main(int argc, char * const argv[])
{    
    char *svalue, *evalue, *bvalue, *tvalue;
    int s, e, b;
    svalue = evalue = bvalue = tvalue = NULL;
    char *pattern = "s:E:b:t:";
    int c;
    while( (c = getopt(argc, argv, pattern)) != -1 ){
        switch(c){
            case 's':
                svalue = optarg;
                break;
            case 'E':
                evalue = optarg;
                break;
            case 'b':
                bvalue = optarg;
                break;
            case 't':
                tvalue = optarg;
                break;
            // invalid option input
            case '?':
                fprintf(stderr, "Invalid option -%c.\n", optopt);
                return 1;
            // under other conditions, just abort
            default:
                abort();
        }
    }    
    // check completeness of input: exclude null arg
    if(!(svalue && evalue && bvalue && tvalue)){
        fprintf(stderr, "Missing required options or arguments.\n");
        return 1;
    }
    // clear number of last error for converting string to int
    s = parseInt(0, svalue);
    e = parseInt(1, evalue);
    b = parseInt(0, bvalue);
    if(s < 0 || e < 0 || b < 0){
        fprintf(stderr, "Invalid argument. \n");
        return 1;
    }    
    int res = simulateCacheOps(s, e, b, tvalue);
    if(res != 0){
        fprintf(stderr, "Simulation failed.\n");
        return 1;
    }
    return 0;
}
