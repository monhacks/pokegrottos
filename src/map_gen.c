#include "global.h"
#include "data_util.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "fieldmap.h"
#include "field_screen_effect.h"
#include "field_weather.h"
#include "main.h"
#include "malloc.h"
#include "map_gen.h"
#include "overworld.h"
#include "random.h"
#include "sound.h"
#include "script.h"
#include "strings.h"
#include "string_util.h"
#include "text.h"
#include "constants/songs.h"

// global floorplan
EWRAM_DATA struct Floorplan gFloorplan = {0};

#include "data/prefab_rules.h"

// Generation helper functions
static u32 CountNeighbors(struct Floorplan* floorplan, u8 i)
{
    return (floorplan->layout[i-10].type >= NORMAL_ROOM) + (floorplan->layout[i-1].type >= NORMAL_ROOM) \
    + (floorplan->layout[i+1].type >= NORMAL_ROOM) + (floorplan->layout[i+10].type >= NORMAL_ROOM);
}

static bool32 Visit(struct Floorplan* floorplan, u8 i)
{
    if (floorplan->numRooms >= floorplan->maxRooms)
        return FALSE;
    if (floorplan->layout[i].type >= NORMAL_ROOM)
        return FALSE;
    if (CountNeighbors(floorplan, i) > 1)
        return FALSE;
    if (i != STARTING_ROOM && (Random() % 2))
        return FALSE;

    Enqueue(&floorplan->queue, i);
    floorplan->layout[i].type = NORMAL_ROOM;
    floorplan->occupiedRooms[floorplan->numRooms] = i;
    floorplan->numRooms += 1;
    return TRUE;
}

static void ZeroFloorplan(struct Floorplan* floorplan)
{
    u32 i;
    floorplan->numRooms = 0;
    floorplan->maxRooms = 0;
    ZeroQueue(&floorplan->queue);
    ZeroStack(&floorplan->endrooms);
    floorplan->mapGroup = 35;
    for (i = 0; i < LAYOUT_SIZE; ++i)
    {
        floorplan->layout[i].type = 0;
        floorplan->layout[i].visited = FALSE;
        floorplan->layout[i].mapNum = 1;
    }
    for (i = 0; i < MAX_ROOMS; ++i)
    {
        floorplan->occupiedRooms[i] = 0;
    }
}

// TODO: Take into account depth.
static u8 GetMaxRooms(void)
{
    return 15;
}

// Populates an empty floorplan.
static void PopulateFloorplan(struct Floorplan* floorplan)
{
    // Set up floorplan.
    ZeroFloorplan(floorplan);
    Enqueue(&floorplan->queue, STARTING_ROOM);
    floorplan->numRooms = 1;
    floorplan->layout[STARTING_ROOM].type = NORMAL_ROOM;
    floorplan->layout[STARTING_ROOM].visited = TRUE;
    floorplan->maxRooms = GetMaxRooms();

    // Generate rooms.
    while (floorplan->queue.size > 0)
    {
        u32 i, x;
        bool32 createdRoom = FALSE;
        // DebugPrintQueue(&floorplan->queue);
        i = Dequeue(&floorplan->queue);
        x = i % 10;
        if (x > 1)
            createdRoom |= Visit(floorplan, i - 1);
        if (x < 9)
            createdRoom |= Visit(floorplan, i + 1);
        if (i > 20)
            createdRoom |= Visit(floorplan, i - 10);
        if (i < 70)
            createdRoom |= Visit(floorplan, i + 10);
        if (!createdRoom)
            Push(&floorplan->endrooms, i);
    }
}

static void ShuffleArray(u8* array, u32 size)
{
    u32 i, j, t;
    // Safety check.
    if (size == 0)
        return;

    // Code from https://stackoverflow.com/questions/6127503/shuffle-array-in-c.
    for (i = 0; i < size - 1; ++i) 
    {
        j = i + Random() / (UINT16_MAX / (size - i) + 1);
        t = array[j];
        array[j] = array[i];
        array[i] = t;
    }
}

// Assigns special room types.
static void AssignSpecialRooms(struct Floorplan* floorplan)
{
    // The farthest room is first on the stack and will always be the boss room.
    floorplan->layout[Pop(&floorplan->endrooms)].type = BOSS_ROOM;

    // Afterwards, we assign room types at random to the remaining rooms.
    while (floorplan->endrooms.top > 0)
        floorplan->layout[Pop(&floorplan->endrooms)].type = TREASURE_ROOM;
}


// Assigns each room a map ID.
static void AssignRoomMapIds(struct Floorplan* floorplan)
{
    u32 i;
    struct Room* room;
    const struct PrefabRules * const rules = &gPrefabRules[floorplan->mapGroup];
    const u8 *normalPool = rules->normalRoomIds;
    u8 poolSize = rules->numNormalRooms;
    u8 *shuffled = AllocZeroed(sizeof(u8) * poolSize);

    // Assign special room types if it hasn't been done yet.
    if (floorplan->endrooms.top > 0)
        AssignSpecialRooms(floorplan);

    // Shuffle the normal map pool.
    memcpy(shuffled, normalPool, sizeof(u8) * poolSize);
    ShuffleArray(shuffled, poolSize);

    // Loop through rooms and assign map IDs.
    for (i = 0; i < floorplan->numRooms; ++i)
    {
        room = &floorplan->layout[floorplan->occupiedRooms[i]];
        switch (room->type)
        {
            case NORMAL_ROOM:
                room->mapNum = shuffled[i % poolSize]; // in case numRooms > poolSize
                break;
            // special rooms
            default:
                room->mapNum = rules->specialRoomIds[room->type];
                break;
        }
    }
}


// This prints the floor layout backwards. (it is janky)
void DebugPrintFloorplan(struct Floorplan* floorplan)
{
    u32 x, y, row, exponent;
    for (y = 0; y < MAX_LAYOUT_HEIGHT; ++y)
    {
        row = 2000000000;
        exponent = 1;
        for(x = 0; x < MAX_LAYOUT_WIDTH; ++x)
        {
            row += floorplan->layout[ROOM_COORD(x, y)].type * exponent;
            exponent *= 10;
        }
        DebugPrintf("%d", row);
    }
}

void CreateDebugFloorplan(void)
{
    u32 attempts = 0;
    gSaveBlock1Ptr->currentRoom = STARTING_ROOM;
    do {
        PopulateFloorplan(&gFloorplan);
    } while (gFloorplan.numRooms < MIN_ROOMS && ++attempts < 10);
    AssignRoomMapIds(&gFloorplan);
    DebugPrintFloorplan(&gFloorplan);
}

bool32 DoesRoomExist(u8 i)
{
    return gFloorplan.layout[i].type >= NORMAL_ROOM;
}

bool32 IsRoomAdjacentToVisited(u8 i)
{
    if (gFloorplan.layout[i - 10].visited)
        return TRUE;
    if (gFloorplan.layout[i + 10].visited)
        return TRUE;
    if (gFloorplan.layout[i - 1].visited)
        return TRUE;
    if (gFloorplan.layout[i + 1].visited)
        return TRUE;
    return FALSE;
}

u32 GetRoomInDirection(u32 dir)
{
    u32 target = 0;
    switch (dir)
    {
        case DIR_NORTH:
            target = gSaveBlock1Ptr->currentRoom - 10;
            break;
        case DIR_SOUTH:
            target = gSaveBlock1Ptr->currentRoom + 10;
            break;
        case DIR_EAST:
            target = gSaveBlock1Ptr->currentRoom + 1;
            break;
        case DIR_WEST:
            target = gSaveBlock1Ptr->currentRoom - 1;
            break;
    }
    return target;
}

void SetWarpDestinationToRoom(u8 i)
{
    SetWarpDestination(gFloorplan.mapGroup, gFloorplan.layout[i].mapNum, gSpecialVar_0x8000, -1, -1);
}

void TryWarpToRoom(void)
{
    u32 target = GetRoomInDirection(gSpecialVar_0x8000);
    gSpecialVar_Result = FALSE;

    // Don't warp if invalid room.
    if (!DoesRoomExist(target))
        return;

    // Set appropriate variables and flags.
    gSpecialVar_Result = TRUE;
    gSaveBlock1Ptr->currentRoom = target;
    gFloorplan.layout[target].visited = TRUE;

    // Do warp.
    StoreInitialPlayerAvatarState();
    LockPlayerFieldControls();
    TryFadeOutOldMapMusic();
    WarpFadeOutScreen();
    PlayRainStoppingSoundEffect();
    PlaySE(SE_EXIT);
    SetWarpDestinationToRoom(target);
    WarpIntoMap();
    SetMainCallback2(CB2_LoadMap);
}

static void ClearFloorEventFlags(void)
{
    u32 i;
    for (i = PREFAB_EVENT_FLAGS_START; i < PREFAB_EVENT_FLAGS_END + 1; ++i)
        FlagClear(i);
}
