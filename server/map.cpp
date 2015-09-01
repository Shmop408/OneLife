#include "map.h"


#include "minorGems/util/random/JenkinsRandomSource.h"

#include "minorGems/util/stringUtils.h"
#include "minorGems/util/SimpleVector.h"
#include "minorGems/util/SettingsManager.h"

#include "minorGems/formats/encodingUtils.h"

#include "kissdb.h"


#include "kissdb.h"

#include <stdarg.h>


#include "../gameSource/transitionBank.h"
#include "../gameSource/objectBank.h"




static int chunkDimension = 32;


static int startingObjectID = 1;



static KISSDB db;
static char dbOpen = false;


static int randSeed = 10;
static JenkinsRandomSource randSource;



#define DECAY_SLOT 1
#define NUM_CONT_SLOT 2
#define FIRST_CONT_SLOT 3


// 15 minutes
static int maxSecondsForActiveDecayTracking = 900;


typedef struct LiveDecayRecord {
        int x, y;
        unsigned int etaTimeSeconds;
    } LiveDecayRecord;



#include "minorGems/util/MinPriorityQueue.h"

static MinPriorityQueue<LiveDecayRecord> liveDecayQueue;




static float getXYRandom( int inX, int inY ) {
    
    unsigned int fullSeed = inX ^ (inY * 57) ^ ( randSeed * 131 );
    
    randSource.reseed( fullSeed );
    
    return randSource.getRandomFloat();
    }


int getChunkDimension() {
    return chunkDimension;
    }







// gets procedurally-generated base map at a given spot
// player modifications are overlayed on top of this
static int getBaseMap( int inX, int inY ) {
    
    float randValue = getXYRandom( inX, inY );
    
    if( randValue < 0.1 ) {
        return startingObjectID;
        }
    return 0;
    }





// two ints to an 8-byte key
void intTripleToKey( int inX, int inY, int inSlot, unsigned char *outKey ) {
    for( int i=0; i<4; i++ ) {
        int offset = i * 8;
        outKey[i] = ( inX >> offset ) & 0xFF;
        outKey[i+4] = ( inY >> offset ) & 0xFF;
        outKey[i+8] = ( inSlot >> offset ) & 0xFF;
        }    
    }

// one int to an 4-byte value
void intToValue( int inV, unsigned char *outValue ) {
    for( int i=0; i<4; i++ ) {
        outValue[i] = ( inV >> (i * 8) ) & 0xFF;
        }    
    }


int valueToInt( unsigned char *inValue ) {
    return 
        inValue[3] << 24 | inValue[2] << 16 | 
        inValue[1] << 8 | inValue[0];
    }





void initMap() {


    int error = KISSDB_open( &db, 
                             "map.db", 
                             KISSDB_OPEN_MODE_RWCREAT,
                             80000,
                             12, // three 32-bit ints, xys
                                 // s is the slot number 
                                 // s=0 for base object
                                 // s=1 for count of contained objects
                                 // s=2 first contained object
                                 // s=3 second contained object
                                 // s=... remaining contained objects
                             4 // one int, object ID at x,y in slot s
                               // OR contained count if s=1
                             );
    
    if( error ) {
        printf( "Error %d opening KissDB\n", error );
        return;
        }
    
    dbOpen = true;
    }



void freeAndNullString( char **inStringPointer ) {
    if( *inStringPointer != NULL ) {
        delete [] *inStringPointer;
        *inStringPointer = NULL;
        }
    }



void freeMap() {
    if( dbOpen ) {
        KISSDB_close( &db );
        }
    }



// returns -1 if not found
static int dbGet( int inX, int inY, int inSlot ) {
    unsigned char key[12];
    unsigned char value[4];

    // look for changes to default in database
    intTripleToKey( inX, inY, inSlot, key );
    
    int result = KISSDB_get( &db, key, value );
    
    if( result == 0 ) {
        // found
        return valueToInt( value );
        }
    else {
        return -1;
        }
    }



static void dbPut( int inX, int inY, int inSlot, int inValue ) {
    unsigned char key[12];
    unsigned char value[4];
    

    intTripleToKey( inX, inY, inSlot, key );
    intToValue( inValue, value );
            
    
    KISSDB_put( &db, key, value );
    }



int checkDecayObject( int inX, int inY, int inID ) {
    TransRecord *t = getTrans( -1, inID );

    if( t == NULL ) {
        // no auto-decay for this object
        return inID;
        }
    
    
    // else decay exists for this object
    
    int newID = inID;

    // is eta stored in map?
    unsigned int mapETA = getEtaDecay( inX, inY );
    
    if( mapETA != 0 ) {
        
        if( (int)mapETA < time( NULL ) ) {
            
            // object in map has decayed (eta expired)

            // apply the transition
            newID = t->newTarget;
            

            int oldSlots = getNumContainerSlots( inID );

            int newSlots = getNumContainerSlots( newID );
            
            if( newSlots < oldSlots ) {
                shrinkContainer( inX, inY, newSlots );
                }
            
            setMapObject( inX, inY, newID );
            
            TransRecord *newDecayT = getTrans( -1, newID );

            if( newDecayT != NULL ) {
                mapETA = time(NULL) + newDecayT->autoDecaySeconds;
                }
            else {
                // no further decay
                mapETA = 0;
                }

            setEtaDecay( inX, inY, mapETA );
            }

        }
    else {
        // update map with decay for the applicable transition
        mapETA = time( NULL ) + t->autoDecaySeconds;
        
        setEtaDecay( inX, inY, mapETA );
        }
    

    if( mapETA != 0 ) {
        int timeLeft = mapETA - time( NULL );
        
        if( timeLeft < maxSecondsForActiveDecayTracking ) {
            // track it live
            
            // duplicates okay
            // we'll deal with them when they ripen
            // (we check the true ETA stored in map before acting
            //   on one stored in this queue)
            LiveDecayRecord r = { inX, inY, mapETA };
            
            liveDecayQueue.insert( r, mapETA );
            }
        }
    
    
    return inID;
    }



int getMapObject( int inX, int inY ) {    
    
    
    int result = dbGet( inX, inY, 0 );
    
    if( result == -1 ) {
        // nothing in map
        result = getBaseMap( inX, inY );
        }

    // apply any decay that should have happened by now
    return checkDecayObject( inX, inY, result );
    }



// returns properly formatted chunk message for chunk centered
// around x,y
unsigned char *getChunkMessage( int inCenterX, int inCenterY,
                                int *outMessageLength ) {
    
    int chunkCells = chunkDimension * chunkDimension;
    
    int *chunk = new int[chunkCells];
    
    int *containedStackSizes = new int[ chunkCells ];
    int **containedStacks = new int*[ chunkCells ];

    // 0,0 is center of map
    
    int halfChunk = chunkDimension /2;
    

    int startY = inCenterY - halfChunk;
    int startX = inCenterX - halfChunk;
    
    int endY = startY + chunkDimension;
    int endX = startX + chunkDimension;

    
    
    for( int y=startY; y<endY; y++ ) {
        int chunkY = y - startY;
        

        for( int x=startX; x<endX; x++ ) {
            int chunkX = x - startX;
            
            int cI = chunkY * chunkDimension + chunkX;
            
            chunk[cI] = getMapObject( x, y );

            int numContained;
            int *contained = getContained( x, y, &numContained );

            if( contained != NULL ) {
                containedStackSizes[cI] = numContained;
                containedStacks[cI] = contained;
                }
            else {
                containedStackSizes[cI] = 0;
                containedStacks[cI] = NULL;
                }
            }
        
        }



    SimpleVector<unsigned char> chunkDataBuffer;

    for( int i=0; i<chunkCells; i++ ) {
        
        if( i > 0 ) {
            chunkDataBuffer.appendArray( (unsigned char*)" ", 1 );
            }
        

        char *cell = autoSprintf( "%d", chunk[i] );
        
        chunkDataBuffer.appendArray( (unsigned char*)cell, strlen(cell) );
        delete [] cell;

        if( containedStacks[i] != NULL ) {
            for( int c=0; c<containedStackSizes[i]; c++ ) {
                char *containedString = autoSprintf( ",%d", 
                                                     containedStacks[i][c] );
        
                chunkDataBuffer.appendArray( (unsigned char*)containedString, 
                                             strlen( containedString ) );
                delete [] containedString;
                }
            delete [] containedStacks[i];
            }
        }
    delete [] chunk;
    
    delete [] containedStackSizes;
    delete [] containedStacks;
    

    unsigned char *chunkData = chunkDataBuffer.getElementArray();
    
    int compressedSize;
    unsigned char *compressedChunkData =
        zipCompress( chunkData, chunkDataBuffer.size(),
                     &compressedSize );



    char *header = autoSprintf( "MC\n%d %d %d\n%d %d\n#", chunkDimension,
                                startX, startY, chunkDataBuffer.size(),
                                compressedSize );
    
    SimpleVector<unsigned char> buffer;
    buffer.appendArray( (unsigned char*)header, strlen( header ) );
    delete [] header;

    
    buffer.appendArray( compressedChunkData, compressedSize );
    
    delete [] chunkData;
    delete [] compressedChunkData;
    

    
    *outMessageLength = buffer.size();
    return buffer.getElementArray();
    }













void setMapObject( int inX, int inY, int inID ) {
    dbPut( inX, inY, 0, inID );
    }




void setEtaDecay( int inX, int inY, unsigned int inAbsoluteTimeInSeconds ) {
    dbPut( inX, inY, DECAY_SLOT, (int)inAbsoluteTimeInSeconds );
    }




unsigned int getEtaDecay( int inX, int inY ) {
    int value = dbGet( inX, inY, DECAY_SLOT );
    
    if( value != -1 ) {
        return (unsigned int)value;
        }
    else {
        return 0;
        }
    
    }





void addContained( int inX, int inY, int inContainedID ) {
    int oldNum = getNumContained( inX, inY );
    
    int newNum = oldNum + 1;
    

    dbPut( inX, inY, FIRST_CONT_SLOT + newNum - 1, inContainedID );
    
    dbPut( inX, inY, NUM_CONT_SLOT, newNum );
    }


int getNumContained( int inX, int inY ) {
    int result = dbGet( inX, inY, NUM_CONT_SLOT );
    
    if( result != -1 ) {
        // found
        return result;
        }
    else {
        // default, empty container
        return 0;
        }
    }



int *getContained( int inX, int inY, int *outNumContained ) {
    int num = getNumContained( inX, inY );

    *outNumContained = num;
    
    if( num == 0 ) {
        return NULL;
        }
   
    int *contained = new int[ num ];

    for( int i=0; i<num; i++ ) {
        int result = dbGet( inX, inY, FIRST_CONT_SLOT + i );
        if( result != -1 ) {
            contained[i] = result;
            }
        else {
            contained[i] = 0;
            }
        }
    return contained;
    }

    

// removes from top of stack
int removeContained( int inX, int inY ) {
    int num = getNumContained( inX, inY );
    
    if( num == 0 ) {
        return 0;
        }
    
    int result = dbGet( inX, inY, FIRST_CONT_SLOT + num - 1 );
    
    // shrink number of slots
    num -= 1;
    dbPut( inX, inY, NUM_CONT_SLOT, num );
        

    if( result != -1 ) {    
        return result;
        }
    else {
        // nothing in that slot
        return 0;
        }
    }



void clearAllContained( int inX, int inY ) {
    dbPut( inX, inY, NUM_CONT_SLOT, 0 );
    }



void shrinkContainer( int inX, int inY, int inNumNewSlots ) {
    int oldNum = getNumContained( inX, inY );
    
    if( oldNum > inNumNewSlots ) {
        dbPut( inX, inY, NUM_CONT_SLOT, inNumNewSlots );
        }
    }




char *getMapChangeLineString( int inX, int inY  ) {
    

    SimpleVector<char> buffer;
    

    char *header = autoSprintf( "%d %d ", inX, inY );
    
    buffer.appendElementString( header );
    
    delete [] header;
    

    char *idString = autoSprintf( "%d", getMapObject( inX, inY ) );
    
    buffer.appendElementString( idString );
    
    delete [] idString;
    
    
    int numContained;
    int *contained = getContained(  inX, inY, &numContained );

    if( numContained > 0 ) {
        for( int i=0; i<numContained; i++ ) {
            
            char *idString = autoSprintf( ",%d", contained[i] );
    
            buffer.appendElementString( idString );
    
            delete [] idString;
            }
        
        delete [] contained;
        }
    
    
    buffer.appendElementString( "\n" );

    return buffer.getElementString();
    }




void stepMap( SimpleVector<char> *inMapChanges, 
              SimpleVector<ChangePosition> *inChangePosList ) {
    // FIXME:

    // check live decay queue for any that are expired

    // for each, remove expired ETA record from queue,
    //
    // THEN check map for true ETA (there can be stale duplicates
    // in the queue), and act only if true ETA expired

    // calling checkDecayObject will handle this

    // for any calls to checkDecayObject that change the ID there,
    // getMapChangeLineString and append it to inMapChanges
    // append position to inChangePosList
    }


