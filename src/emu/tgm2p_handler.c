#include "tgm2p_handler.h"

#include "tgmtracker.h"
#include "tgm_memorymap.h"

#include <stdlib.h>
#include <time.h>

#include "emu.h"
#include "debug/express.h"

#if !defined(_WIN32) && (defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__)))
#include <sys/stat.h>

int createDir(const char* path)
{
    struct stat st = {0};
    if (stat(path, &st) == -1)
    {
        return mkdir(path, 0700);
    }
    return 0;
}

void tgm2p_create_mmap()
{
    tgm_mm_create(sizeof(struct tap_state));
}

void tgm2p_destroy_mmap()
{
    tgm_mm_destroy();
}

#elif defined(_WIN64) || defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static struct tap_state* sharedAddr = NULL;

int createDir(const char* path)
{
    const WCHAR* wcpath;
    int nChars = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    wcpath = (WCHAR*)malloc(sizeof(WCHAR) * nChars);
    MultiByteToWideChar(CP_ACP, 0, path, -1, (LPWSTR)wcpath, nChars);

    if (GetFileAttributes(wcpath) == INVALID_FILE_ATTRIBUTES)
    {
        if (CreateDirectory(wcpath, NULL) == 0)
        {
            WCHAR buf[256];
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 256, NULL);
            printf("%ls\n", buf);
        }
    }

    free((void*)wcpath);

    return 0;
}

void tgm2p_create_mmap()
{
    // TODO
}

void tgm2p_destroy_mmap()
{
    // TODO
}

#endif

const int GRADE_COUNT = 32;
const char* GRADE_DISPLAY[GRADE_COUNT] =
{
    "9", "8", "7", "6", "5", "4-", "4+", "3-", "3+", "2-", "2", "2+", "1-",
    "1", "1+", "S1-", "S1", "S1+", "S2", "S3", "S4-", "S4", "S4+", "S5-",
    "S5+", "S6-", "S6+", "S7-", "S7+", "S8-", "S8+", "S9"
};

enum tap_internal_state
{
    TAP_NONE         = 0,
    TAP_START        = 1,
    TAP_ACTIVE       = 2,
    TAP_LOCKING      = 3,  // Cannot be influenced anymore
    TAP_LINECLEAR    = 4,  // Tetromino is being locked to the playfield.
    TAP_ENTRY        = 5,
    TAP_GAMEOVER     = 7,  // "Game Over" is being shown on screen.
    TAP_IDLE         = 10, // No game has started, just waiting...
    TAP_FADING       = 11, // Blocks fading away when topping out (losing).
    TAP_COMPLETION   = 13, // Blocks fading when completing the game
    TAP_STARTUP      = 71,
};

enum tap_mroll_flags
{
    M_FAIL_1   = 17,
    M_FAIL_2   = 19,
    M_FAIL_END = 31,

    M_NEUTRAL  = 48,
    M_PASS_1   = 49,
    M_PASS_2   = 51,
    M_SUCCESS  = 127,
};

enum tap_game_mode
{
    TAP_MODE_NULL           = 0,
    TAP_MODE_NORMAL         = 1,
    TAP_MODE_MASTER         = 2,
    TAP_MODE_DOUBLES        = 4,
    TAP_MODE_NORMAL_VERSUS  = 9,
    TAP_MODE_MASTER_VERSUS  = 10,
    TAP_MODE_MASTER_CREDITS = 18,
    TAP_MODE_TGMPLUS        = 128,
    TAP_MODE_TGMPLUS_VERSUS = 136,
    TAP_MODE_MASTER_ITEM    = 514,
    TAP_MODE_TGMPLUS_ITEM   = 640,
    TAP_MODE_DEATH          = 4096,
    TAP_MODE_DEATH_VERSUS   = 4104
};

bool testMasterConditions(char flags)
{
    return
        flags == M_NEUTRAL ||
        flags == M_PASS_1  ||
        flags == M_PASS_2  ||
        flags == M_SUCCESS;
}

bool inPlayingState(char state)
{
    return state != TAP_NONE && state != TAP_IDLE && state != TAP_STARTUP;
}

const offs_t STATE_ADDR       = 0x06064BF5;  // p1 State
const offs_t LEVEL_ADDR       = 0x06064BBA;  // p1 Level
const offs_t TIMER_ADDR       = 0x06064BEA;  // p1 Timer
const offs_t GRADE_ADDR       = 0x06079378;  // Master-mode internal grade
const offs_t GRADEPOINTS_ADDR = 0x06079379;  // Master-mode internal grade points
const offs_t MROLLFLAGS_ADDR  = 0x06064BD0;  // M-Roll flags
const offs_t INROLL_ADDR      = 0x06066845;  // p1 in-credit-roll
const offs_t SECTION_ADDR     = 0x06064C25;  // p1 section index

const offs_t TETRO_ADDR       = 0x06064BF6;  // Current block
const offs_t NEXT_ADDR        = 0x06064BF8;  // Next block
const offs_t CURRX_ADDR       = 0x06064BFC;  // Current block X position
const offs_t CURRY_ADDR       = 0x06064C00;  // Current block Y position
const offs_t ROTATION_ADDR    = 0x06064BFA;  // Current block rotation state

const offs_t GAMEMODE_ADDR    = 0x06064BA4;  // Current game mode

// TGM2+ indexes its pieces slightly differently to fumen, so when encoding a
// diagram we must convert the indices:
// 2 3 4 5 6 7 8 (TAP)
// I Z S J L O T
// 1 4 7 6 2 3 5 (Fumen)
char TapToFumenMapping[9] = { 0, 0, 1, 4, 7, 6, 2, 3, 5 };

// Coordinates from TAP do not align perfectly with fumen's coordinates
// (depending on tetromino and rotation state).
void fixTapCoordinates(struct tap_state* tstate)
{
    if (tstate->tetromino == 1)
    {
        // Fix underflow when I tetromino is in column 1.
        if (tstate->xcoord > 10)
        {
            tstate->xcoord = -1;
        }

        if (tstate->rotation == 1 || tstate->rotation == 3)
        {
            tstate->xcoord += 1;
        }
    }
    else if (tstate->tetromino == 6)
    {
        if (tstate->rotation == 2)
        {
            tstate->ycoord -= 1;
        }
    }
    else if (tstate->tetromino == 2)
    {
        if (tstate->rotation == 2)
        {
            tstate->ycoord -= 1;
        }
    }
    else if (tstate->tetromino == 5)
    {
        if (tstate->rotation == 2)
        {
            tstate->ycoord -= 1;
        }
    }
}

void readState(const address_space* space, struct tap_state* state)
{
    state->state        = memory_read_byte(space, STATE_ADDR);
    state->grade        = memory_read_byte(space, GRADE_ADDR);
    state->gradePoints  = memory_read_byte(space, GRADEPOINTS_ADDR);

    state->level        = memory_read_word(space, LEVEL_ADDR);
    state->timer        = memory_read_word(space, TIMER_ADDR);

    state->tetromino    = memory_read_word(space, TETRO_ADDR);
    state->xcoord       = memory_read_word(space, CURRX_ADDR);
    state->ycoord       = memory_read_word(space, CURRY_ADDR);
    state->rotation     = memory_read_byte(space, ROTATION_ADDR);

    state->mrollFlags   = memory_read_byte(space, MROLLFLAGS_ADDR);
    state->inCreditRoll = memory_read_byte(space, INROLL_ADDR);

    state->gameMode = memory_read_word(space, GAMEMODE_ADDR);
}

void pushStateToList(struct tap_state* list, size_t* listSize, struct tap_state* state)
{
    state->tetromino = TapToFumenMapping[state->tetromino];

    fixTapCoordinates(state);

    list[*listSize] = *state;
    (*listSize)++;
}

// First Demo: Two simultaneous single player games.
static const size_t demo01_length = 17;
static struct tap_state demo01[] =
{
    { 0, 9, 0,  0,   27, 6, 1, 2, 2, 1, 0, 0 },
    { 0, 9, 0,  1,   76, 5, 4, 2, 2, 1, 0, 0 },
    { 0, 9, 0,  2,  138, 3, 6, 3, 0, 1, 0, 0 },
    { 0, 9, 0,  3,  227, 2, 2, 3, 2, 1, 0, 0 },
    { 0, 9, 0,  4,  292, 7, 2, 5, 0, 1, 0, 0 },
    { 0, 9, 0,  5,  385, 6, 7, 4, 0, 1, 0, 0 },
    { 0, 9, 0,  6,  439, 5, 5, 4, 3, 1, 0, 0 },
    { 0, 9, 0,  7,  492, 4, 0, 5, 1, 1, 0, 0 },
    { 0, 9, 0,  8,  544, 1, 9, 4, 3, 1, 0, 0 },
    { 0, 9, 0, 11,  636, 7, 4, 4, 1, 1, 0, 0 },
    { 0, 9, 0, 12,  693, 3, 6, 4, 0, 1, 0, 0 },
    { 0, 9, 0, 13,  760, 4, 0, 5, 1, 1, 0, 0 },
    { 0, 9, 0, 14,  828, 2, 6, 5, 0, 1, 0, 0 },
    { 0, 9, 0, 15,  884, 1, 8, 4, 1, 1, 0, 0 },
    { 0, 9, 0, 18,  970, 5, 4, 4, 1, 1, 0, 0 },
    { 0, 9, 0, 19, 1036, 6, 2, 3, 1, 1, 0, 0 },
    { 0, 9, 0, 19, 1061, 6, 2, 3, 1, 1, 0, 0 },
};

// Second Demo: Vs Mode.
static const size_t demo02_length = 15;
static struct tap_state demo02[] =
{
    { 0, 9, 0,  0, 9553, 2, 1, 2, 2, 1, 0, 0 },
    { 0, 9, 0,  1, 9492, 6, 4, 2, 2, 1, 0, 0 },
    { 0, 9, 0,  2, 9443, 1, 0, 5, 3, 1, 0, 0 },
    { 0, 9, 0,  3, 9395, 7, 4, 4, 1, 1, 0, 0 },
    { 0, 9, 0,  4, 9325, 4, 1, 4, 1, 1, 0, 0 },
    { 0, 9, 0,  5, 9264, 5, 1, 6, 1, 1, 0, 0 },
    { 0, 9, 0,  6, 9183, 3, 6, 3, 0, 1, 0, 0 },
    { 0, 9, 0,  7, 9124, 2, 6, 4, 0, 1, 0, 0 },
    { 0, 9, 0,  8, 9074, 6, 8, 3, 1, 1, 0, 0 },
    { 0, 9, 0, 10, 8975, 1, 5, 4, 0, 1, 0, 0 },
    { 0, 9, 0, 11, 8890, 4, 3, 9, 0, 1, 0, 0 },
    { 0, 9, 0, 12, 8804, 3, 5, 9, 0, 1, 0, 0 },
    { 0, 9, 0, 13, 8753, 7, 9, 7, 3, 1, 0, 0 },
    { 0, 9, 0, 16, 8666, 5, 0, 8, 1, 1, 0, 0 },
    { 0, 9, 0, 17, 8617, 2, 5, 8, 0, 1, 0, 0 },
};

// Third Demo: Doubles Mode.
static const size_t demo03_length = 15;
static struct tap_state demo03[] =
{
    { 0, 9, 0,  0,   33, 6, 1, 2, 2, 1, 0, 0 },
    { 0, 9, 0,  1,  129, 5, 4, 2, 2, 1, 0, 0 },
    { 0, 9, 0,  2,  215, 2, 2, 3, 2, 1, 0, 0 },
    { 0, 9, 0,  3,  271, 3, 0, 5, 0, 1, 0, 0 },
    { 0, 9, 0,  4,  347, 1, 6, 4, 1, 1, 0, 0 },
    { 0, 9, 0,  5,  443, 4, 2, 5, 1, 1, 0, 0 },
    { 0, 9, 0,  6,  513, 6, 1, 5, 2, 1, 0, 0 },
    { 0, 9, 0,  7,  590, 2, 2, 6, 2, 1, 0, 0 },
    { 0, 9, 0,  8,  657, 5, 5, 3, 3, 1, 0, 0 },
    { 0, 9, 0, 11,  755, 7, 2, 6, 0, 1, 0, 0 },
    { 0, 9, 0, 12,  812, 4, 0, 5, 1, 1, 0, 0 },
    { 0, 9, 0, 13,  904, 1, 5, 6, 1, 1, 0, 0 },
    { 0, 9, 0, 14,  990, 3, 2, 7, 0, 1, 0, 0 },
    { 0, 9, 0, 15, 1051, 5, 0, 7, 1, 1, 0, 0 },
    { 0, 9, 0, 15, 1061, 5, 0, 7, 1, 1, 0, 0 },
};

bool testDemoState(struct tap_state* stateList, size_t listSize, struct tap_state* demo, size_t demoSize)
{
    if (listSize > demoSize)
    {
        return false;
    }

    for (size_t i = 0; i < listSize && i < demoSize; ++i)
    {
        if (stateList[i].tetromino != demo[i].tetromino &&
            stateList[i].xcoord != demo[i].xcoord &&
            stateList[i].ycoord != demo[i].ycoord)
        {
            return false;
        }
    }
    return true;
}

bool isDemoState(struct tap_state* stateList, size_t listSize)
{
    return
        testDemoState(stateList, listSize, demo01, demo01_length) ||
        testDemoState(stateList, listSize, demo02, demo02_length) ||
        testDemoState(stateList, listSize, demo03, demo03_length);
}

static struct tap_state curState = {0}, prevState = {0};

static const size_t MAX_TAP_STATES = 1300; // What a nice number
static struct tap_state stateList[MAX_TAP_STATES];
static size_t stateListSize = 0;

static const address_space* space = NULL;

void writePlacementLog()
{
    if (stateListSize == 0)
    {
        printf("State list is empty!\n");
    }
    else if (isDemoState(stateList, stateListSize))
    {
        printf("Demo state detected!\n");
    }
    else
    {
        // Push the killing piece. We must use the previous state
        // since, upon death, TAP clears some data.
        pushStateToList(stateList, &stateListSize, &prevState);

        // Create fumen directory if it doesn't exist.
        createDir("fumen/");

        char directory[32];
        char timebuf[32];
        char filename[80];

        // Create a directory for the day if it doesn't already exist.
        time_t rawTime;
        time(&rawTime);
        const struct tm* timeInfo = localtime(&rawTime);
        strftime(directory, 32, "fumen/%Y-%m-%d", timeInfo);

        createDir(directory);

        strftime(timebuf, 32, "%H-%M-%S", timeInfo);
        snprintf(filename, 80, "%s/%s_Lvl%d.txt", directory, timebuf, stateList[stateListSize - 1].level);

        FILE* file = fopen(filename, "w");

        if (file != NULL)
        {
            printf("Writing data to %s.\n", filename);

            if (prevState.gameMode == TAP_MODE_MASTER)
                fprintf(file, "master\n");
            else if (prevState.gameMode == TAP_MODE_DEATH)
                fprintf(file, "death\n");

            for (size_t i = 0; i < stateListSize; ++i)
            {
                struct tap_state* current = &stateList[i];
                fprintf(file, "%s,%d,%d,%d,%d,%d,%d,%d,%d\n",
                        GRADE_DISPLAY[(int)current->grade],
                        current->level,
                        current->timer,
                        current->tetromino,
                        current->xcoord,
                        current->ycoord,
                        current->rotation,
                        testMasterConditions(current->mrollFlags),
                        current->inCreditRoll
                    );
            }
            fclose(file);
        }
        else
        {
            printf("Cannot write log to %s.\n", filename);
        }
    }

    stateListSize = 0;
}

void tgm2p_setAddressSpace(running_machine* machine)
{
    space = tt_setAddressSpace(machine);
}

void tgm2p_run(bool fumen, bool tracker)
{
    // We want to detect /changes/ in game state.
    prevState = curState;
    readState(space, &curState);

    /* printf("%d\n", memory_read_byte(space, STATE_ADDR)); */

    // Log placements
    if (fumen)
    {
        // Piece is locked in
        if (inPlayingState(curState.state) && prevState.state == TAP_ACTIVE && curState.state == TAP_LOCKING)
        {
            pushStateToList(stateList, &stateListSize, &curState);
        }

        // Game is over
        if (inPlayingState(prevState.state) && !inPlayingState(curState.state))
        {
            writePlacementLog();
        }
    }

    // Write current tap state to memory mapped file
    if (tracker)
    {
        *(struct tap_state*)tgm_mm_getMapPointer() = curState;
    }
}