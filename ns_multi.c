#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <time.h>
#include <conio.h>
#include "ns_dpmi.h"
#include "ns_usrho.h"
#include "ns_inter.h"
#include "ns_dma.h"
#include "ns_ll.h"
#include "ns_cards.h"
#include "ns_sb.h"
#include "ns_scape.h"
#include "ns_dsney.h"
#include "ns_pas16.h"
#include "ns_gusau.h"
#include "ns_pitch.h"
#include "ns_multi.h"
#include "ns_muldf.h"

#define RoundFixed(fixedval, bits)             \
    (                                          \
        (                                      \
            (fixedval) + (1 << ((bits)-1))) >> \
        (bits))

#define IS_QUIET(ptr) ((void *)(ptr) == (void *)&MV_VolumeTable[0])

static int MV_ReverbLevel;
static int MV_ReverbDelay;
static VOLUME16 *MV_ReverbTable = NULL;

//static signed short MV_VolumeTable[ MV_MaxVolume + 1 ][ 256 ];
static signed short MV_VolumeTable[63 + 1][256];

//static Pan MV_PanTable[ MV_NumPanPositions ][ MV_MaxVolume + 1 ];
static Pan MV_PanTable[MV_NumPanPositions][63 + 1];

static int MV_Installed = FALSE;
static int MV_SoundCard = SoundBlaster;
static int MV_TotalVolume = MV_MaxTotalVolume;
static int MV_MaxVoices = 1;

static int MV_BufferSize = MixBufferSize;
static int MV_BufferLength;

static int MV_NumberOfBuffers = NumberOfBuffers;

static int MV_MixMode = MONO_8BIT;
static int MV_Channels = 1;
static int MV_Bits = 8;

static int MV_Silence = SILENCE_8BIT;
static int MV_SwapLeftRight = FALSE;

static int MV_RequestedMixRate;
static int MV_MixRate;

static int MV_DMAChannel = -1;
static int MV_BuffShift;

static int MV_TotalMemory;

static int MV_BufferDescriptor;
static int MV_BufferEmpty[NumberOfBuffers];
char *MV_MixBuffer[NumberOfBuffers + 1];

static VoiceNode *MV_Voices = NULL;

static volatile VoiceNode VoiceList;
static volatile VoiceNode VoicePool;

static int MV_MixPage = 0;
static int MV_VoiceHandle = MV_MinVoiceHandle;

static void (*MV_CallBackFunc)(unsigned long) = NULL;
static void (*MV_MixFunction)(VoiceNode *voice, int buffer);

static int MV_MaxVolume = 63;

char *MV_HarshClipTable;
char *MV_MixDestination;
short *MV_LeftVolume;
short *MV_RightVolume;
int MV_SampleSize = 1;
int MV_RightChannelOffset;

unsigned long MV_MixPosition;

int MV_ErrorCode = MV_Ok;

#define MV_SetErrorCode(status) \
    MV_ErrorCode = (status);

/**********************************************************************

   Memory locked functions:

**********************************************************************/

#define MV_LockStart MV_Mix

/*---------------------------------------------------------------------
   Function: MV_Mix

   Mixes the sound into the buffer.
---------------------------------------------------------------------*/

static void MV_Mix(
    VoiceNode *voice,
    int buffer)

{
    char *start;
    int length;
    long voclength;
    unsigned long position;
    unsigned long rate;
    unsigned long FixedPointBufferSize;

    if ((voice->length == 0) && (voice->GetSound(voice) != KeepPlaying))
    {
        return;
    }

    length = MixBufferSize;
    FixedPointBufferSize = voice->FixedPointBufferSize;

    MV_MixDestination = MV_MixBuffer[buffer];
    MV_LeftVolume = voice->LeftVolume;
    MV_RightVolume = voice->RightVolume;

    if ((MV_Channels == 2) && (IS_QUIET(MV_LeftVolume)))
    {
        MV_LeftVolume = MV_RightVolume;
        MV_MixDestination += MV_RightChannelOffset;
    }

    // Add this voice to the mix
    while (length > 0)
    {
        start = voice->sound;
        rate = voice->RateScale;
        position = voice->position;

        // Check if the last sample in this buffer would be
        // beyond the length of the sample block
        if ((position + FixedPointBufferSize) >= voice->length)
        {
            if (position < voice->length)
            {
                voclength = (voice->length - position + rate - 1) / rate;
            }
            else
            {
                voice->GetSound(voice);
                return;
            }
        }
        else
        {
            voclength = length;
        }

        voice->mix(position, rate, start, voclength);

        if (voclength & 1)
        {
            MV_MixPosition += rate;
            voclength -= 1;
        }
        voice->position = MV_MixPosition;

        length -= voclength;

        if (voice->position >= voice->length)
        {
            // Get the next block of sound
            if (voice->GetSound(voice) != KeepPlaying)
            {
                return;
            }

            if (length > 0)
            {
                // Get the position of the last sample in the buffer
                FixedPointBufferSize = voice->RateScale * (length - 1);
            }
        }
    }
}

/*---------------------------------------------------------------------
   Function: MV_PlayVoice

   Adds a voice to the play list.
---------------------------------------------------------------------*/

void MV_PlayVoice(
    VoiceNode *voice)

{
    unsigned flags;

    flags = DisableInterrupts();
    LL_SortedInsertion(&VoiceList, voice, prev, next, VoiceNode, priority);

    RestoreInterrupts(flags);
}

/*---------------------------------------------------------------------
   Function: MV_StopVoice

   Removes the voice from the play list and adds it to the free list.
---------------------------------------------------------------------*/

void MV_StopVoice(
    VoiceNode *voice)

{
    unsigned flags;

    flags = DisableInterrupts();

    // move the voice from the play list to the free list
    LL_Remove(voice, next, prev);
    LL_Add(&VoicePool, voice, next, prev);

    RestoreInterrupts(flags);
}

/*---------------------------------------------------------------------
   Function: MV_ServiceVoc

   Starts playback of the waiting buffer and mixes the next one.
---------------------------------------------------------------------*/

// static int backcolor = 1;

void MV_ServiceVoc(
    void)

{
    VoiceNode *voice;
    VoiceNode *next;
    char *buffer;

    if (MV_DMAChannel >= 0)
    {
        // Get the currently playing buffer
        buffer = (char *)DMA_GetCurrentPos(MV_DMAChannel);
        MV_MixPage = (unsigned)(buffer - MV_MixBuffer[0]);
        MV_MixPage >>= MV_BuffShift;
    }

    // Toggle which buffer we'll mix next
    MV_MixPage++;
    if (MV_MixPage >= MV_NumberOfBuffers)
    {
        MV_MixPage -= MV_NumberOfBuffers;
    }

    if (MV_ReverbLevel == 0)
    {
        // Initialize buffer
        //Commented out so that the buffer is always cleared.
        //This is so the guys at Echo Speech can mix into the
        //buffer even when no sounds are playing.
        //if ( !MV_BufferEmpty[ MV_MixPage ] )
        {
            ClearBuffer_DW(MV_MixBuffer[MV_MixPage], MV_Silence, MV_BufferSize >> 2);
            if ((MV_SoundCard == UltraSound) && (MV_Channels == 2))
            {
                ClearBuffer_DW(MV_MixBuffer[MV_MixPage] + MV_RightChannelOffset,
                               MV_Silence, MV_BufferSize >> 2);
            }
            MV_BufferEmpty[MV_MixPage] = TRUE;
        }
    }
    else
    {
        char *end;
        char *source;
        char *dest;
        int count;
        int length;

        end = MV_MixBuffer[0] + MV_BufferLength;
        ;
        dest = MV_MixBuffer[MV_MixPage];
        source = MV_MixBuffer[MV_MixPage] - MV_ReverbDelay;
        if (source < MV_MixBuffer[0])
        {
            source += MV_BufferLength;
        }

        length = MV_BufferSize;
        while (length > 0)
        {
            count = length;
            if (source + count > end)
            {
                count = end - source;
            }

            if (MV_Bits == 16)
            {
                if (MV_ReverbTable != NULL)
                {
                    MV_16BitReverb(source, dest, MV_ReverbTable, count / 2);
                    if ((MV_SoundCard == UltraSound) && (MV_Channels == 2))
                    {
                        MV_16BitReverb(source + MV_RightChannelOffset,
                                       dest + MV_RightChannelOffset, MV_ReverbTable, count / 2);
                    }
                }
                else
                {
                    MV_16BitReverbFast(source, dest, count / 2, MV_ReverbLevel);
                    if ((MV_SoundCard == UltraSound) && (MV_Channels == 2))
                    {
                        MV_16BitReverbFast(source + MV_RightChannelOffset,
                                           dest + MV_RightChannelOffset, count / 2, MV_ReverbLevel);
                    }
                }
            }
            else
            {
                if (MV_ReverbTable != NULL)
                {
                    MV_8BitReverb(source, dest, MV_ReverbTable, count);
                    if ((MV_SoundCard == UltraSound) && (MV_Channels == 2))
                    {
                        MV_8BitReverb(source + MV_RightChannelOffset,
                                      dest + MV_RightChannelOffset, MV_ReverbTable, count);
                    }
                }
                else
                {
                    MV_8BitReverbFast(source, dest, count, MV_ReverbLevel);
                    if ((MV_SoundCard == UltraSound) && (MV_Channels == 2))
                    {
                        MV_8BitReverbFast(source + MV_RightChannelOffset,
                                          dest + MV_RightChannelOffset, count, MV_ReverbLevel);
                    }
                }
            }

            // if we go through the loop again, it means that we've wrapped around the buffer
            source = MV_MixBuffer[0];
            dest += count;
            length -= count;
        }
    }

    // Play any waiting voices
    for (voice = VoiceList.next; voice != &VoiceList; voice = next)
    {
        //      if ( ( voice < &MV_Voices[ 0 ] ) || ( voice > &MV_Voices[ 8 ] ) )
        //         {
        //         SetBorderColor(backcolor++);
        //         break;
        //         }

        MV_BufferEmpty[MV_MixPage] = FALSE;

        MV_MixFunction(voice, MV_MixPage);

        next = voice->next;

        // Is this voice done?
        if (!voice->Playing)
        {
            MV_StopVoice(voice);

            if (MV_CallBackFunc)
            {
                MV_CallBackFunc(voice->callbackval);
            }
        }
    }
}

int leftpage = -1;
int rightpage = -1;

void MV_ServiceGus(char **ptr, unsigned long *length)
{
    if (leftpage == MV_MixPage)
    {
        MV_ServiceVoc();
    }

    leftpage = MV_MixPage;

    *ptr = MV_MixBuffer[MV_MixPage];
    *length = MV_BufferSize;
}

void MV_ServiceRightGus(char **ptr, unsigned long *length)
{
    if (rightpage == MV_MixPage)
    {
        MV_ServiceVoc();
    }

    rightpage = MV_MixPage;

    *ptr = MV_MixBuffer[MV_MixPage] + MV_RightChannelOffset;
    *length = MV_BufferSize;
}

/*---------------------------------------------------------------------
   Function: MV_GetNextVOCBlock

   Interperate the information of a VOC format sound file.
---------------------------------------------------------------------*/

playbackstatus MV_GetNextVOCBlock(
    VoiceNode *voice)

{
    unsigned char *ptr;
    int blocktype;
    int lastblocktype;
    unsigned long blocklength;
    unsigned long samplespeed;
    unsigned int tc;
    int packtype;
    int voicemode;
    int done;
    unsigned BitsPerSample;
    unsigned Channels;
    unsigned Format;

    if (voice->BlockLength > 0)
    {
        voice->position -= voice->length;
        voice->sound += voice->length >> 16;
        if (voice->bits == 16)
        {
            voice->sound += voice->length >> 16;
        }
        voice->length = min(voice->BlockLength, 0x8000);
        voice->BlockLength -= voice->length;
        voice->length <<= 16;
        return (KeepPlaying);
    }

    if ((voice->length > 0) && (voice->LoopEnd != NULL) &&
        (voice->LoopStart != NULL))
    {
        voice->BlockLength = voice->LoopSize;
        voice->sound = voice->LoopStart;
        voice->position = 0;
        voice->length = min(voice->BlockLength, 0x8000);
        voice->BlockLength -= voice->length;
        voice->length <<= 16;
        return (KeepPlaying);
    }

    ptr = (unsigned char *)voice->NextBlock;

    voice->Playing = TRUE;

    voicemode = 0;
    lastblocktype = 0;
    packtype = 0;

    done = FALSE;
    while (!done)
    {
        // Stop playing if we get a NULL pointer
        if (ptr == NULL)
        {
            voice->Playing = FALSE;
            done = TRUE;
            break;
        }

        blocktype = (int)*ptr;
        blocklength = (*(unsigned long *)(ptr + 1)) & 0x00ffffff;
        ptr += 4;

        switch (blocktype)
        {
        case 0:
            // End of data
            if ((voice->LoopStart == NULL) ||
                (voice->LoopStart >= (ptr - 4)))
            {
                voice->Playing = FALSE;
                done = TRUE;
            }
            else
            {
                voice->BlockLength = (ptr - 4) - voice->LoopStart;
                voice->sound = voice->LoopStart;
                voice->position = 0;
                voice->length = min(voice->BlockLength, 0x8000);
                voice->BlockLength -= voice->length;
                voice->length <<= 16;
                return (KeepPlaying);
            }
            break;

        case 1:
            // Sound data block
            voice->bits = 8;
            if (lastblocktype != 8)
            {
                tc = (unsigned int)*ptr << 8;
                packtype = *(ptr + 1);
            }

            ptr += 2;
            blocklength -= 2;

            samplespeed = 256000000L / (65536 - tc);

            // Skip packed or stereo data
            if ((packtype != 0) || (voicemode != 0))
            {
                ptr += blocklength;
            }
            else
            {
                done = TRUE;
            }
            voicemode = 0;
            break;

        case 2:
            // Sound continuation block
            samplespeed = voice->SamplingRate;
            done = TRUE;
            break;

        case 3:
            // Silence
            // Not implimented.
            ptr += blocklength;
            break;

        case 4:
            // Marker
            // Not implimented.
            ptr += blocklength;
            break;

        case 5:
            // ASCII string
            // Not implimented.
            ptr += blocklength;
            break;

        case 6:
            // Repeat begin
            if (voice->LoopEnd == NULL)
            {
                voice->LoopCount = *(unsigned short *)ptr;
                voice->LoopStart = ptr + blocklength;
            }
            ptr += blocklength;
            break;

        case 7:
            // Repeat end
            ptr += blocklength;
            if (lastblocktype == 6)
            {
                voice->LoopCount = 0;
            }
            else
            {
                if ((voice->LoopCount > 0) && (voice->LoopStart != NULL))
                {
                    ptr = voice->LoopStart;
                    if (voice->LoopCount < 0xffff)
                    {
                        voice->LoopCount--;
                        if (voice->LoopCount == 0)
                        {
                            voice->LoopStart = NULL;
                        }
                    }
                }
            }
            break;

        case 8:
            // Extended block
            voice->bits = 8;
            tc = *(unsigned short *)ptr;
            packtype = *(ptr + 2);
            voicemode = *(ptr + 3);
            ptr += blocklength;
            break;

        case 9:
            // New sound data block
            samplespeed = *(unsigned long *)ptr;
            BitsPerSample = (unsigned)*(ptr + 4);
            Channels = (unsigned)*(ptr + 5);
            Format = (unsigned)*(unsigned short *)(ptr + 6);

            if ((BitsPerSample == 8) && (Channels == 1) &&
                (Format == VOC_8BIT))
            {
                ptr += 12;
                blocklength -= 12;
                voice->bits = 8;
                done = TRUE;
            }
            else if ((BitsPerSample == 16) && (Channels == 1) &&
                     (Format == VOC_16BIT))
            {
                ptr += 12;
                blocklength -= 12;
                voice->bits = 16;
                done = TRUE;
            }
            else
            {
                ptr += blocklength;
            }
            break;

        default:
            // Unknown data.  Probably not a VOC file.
            voice->Playing = FALSE;
            done = TRUE;
            break;
        }

        lastblocktype = blocktype;
    }

    if (voice->Playing)
    {
        voice->NextBlock = ptr + blocklength;
        voice->sound = ptr;

        voice->SamplingRate = samplespeed;
        voice->RateScale = (voice->SamplingRate * voice->PitchScale) / MV_MixRate;

        // Multiply by MixBufferSize - 1
        voice->FixedPointBufferSize = (voice->RateScale * MixBufferSize) -
                                      voice->RateScale;

        if (voice->LoopEnd != NULL)
        {
            if (blocklength > (unsigned long)voice->LoopEnd)
            {
                blocklength = (unsigned long)voice->LoopEnd;
            }
            else
            {
                voice->LoopEnd = (char *)blocklength;
            }

            voice->LoopStart = voice->sound + (unsigned long)voice->LoopStart;
            voice->LoopEnd = voice->sound + (unsigned long)voice->LoopEnd;
            voice->LoopSize = voice->LoopEnd - voice->LoopStart;
        }

        if (voice->bits == 16)
        {
            blocklength /= 2;
        }

        voice->position = 0;
        voice->length = min(blocklength, 0x8000);
        voice->BlockLength = blocklength - voice->length;
        voice->length <<= 16;

        MV_SetVoiceMixMode(voice);

        return (KeepPlaying);
    }

    return (NoMoreData);
}

/*---------------------------------------------------------------------
   Function: MV_GetNextDemandFeedBlock

   Controls playback of demand fed data.
---------------------------------------------------------------------*/

playbackstatus MV_GetNextDemandFeedBlock(
    VoiceNode *voice)

{
    if (voice->BlockLength > 0)
    {
        voice->position -= voice->length;
        voice->sound += voice->length >> 16;
        voice->length = min(voice->BlockLength, 0x8000);
        voice->BlockLength -= voice->length;
        voice->length <<= 16;

        return (KeepPlaying);
    }

    if (voice->DemandFeed == NULL)
    {
        return (NoMoreData);
    }

    voice->position = 0;
    (voice->DemandFeed)(&voice->sound, &voice->BlockLength);
    voice->length = min(voice->BlockLength, 0x8000);
    voice->BlockLength -= voice->length;
    voice->length <<= 16;

    if ((voice->length > 0) && (voice->sound != NULL))
    {
        return (KeepPlaying);
    }
    return (NoMoreData);
}

/*---------------------------------------------------------------------
   Function: MV_GetNextRawBlock

   Controls playback of demand fed data.
---------------------------------------------------------------------*/

playbackstatus MV_GetNextRawBlock(
    VoiceNode *voice)

{
    if (voice->BlockLength <= 0)
    {
        if (voice->LoopStart == NULL)
        {
            voice->Playing = FALSE;
            return (NoMoreData);
        }

        voice->BlockLength = voice->LoopSize;
        voice->NextBlock = voice->LoopStart;
        voice->length = 0;
        voice->position = 0;
    }

    voice->sound = voice->NextBlock;
    voice->position -= voice->length;
    voice->length = min(voice->BlockLength, 0x8000);
    voice->NextBlock += voice->length;
    if (voice->bits == 16)
    {
        voice->NextBlock += voice->length;
    }
    voice->BlockLength -= voice->length;
    voice->length <<= 16;

    return (KeepPlaying);
}

/*---------------------------------------------------------------------
   Function: MV_GetNextWAVBlock

   Controls playback of demand fed data.
---------------------------------------------------------------------*/

playbackstatus MV_GetNextWAVBlock(
    VoiceNode *voice)

{
    if (voice->BlockLength <= 0)
    {
        if (voice->LoopStart == NULL)
        {
            voice->Playing = FALSE;
            return (NoMoreData);
        }

        voice->BlockLength = voice->LoopSize;
        voice->NextBlock = voice->LoopStart;
        voice->length = 0;
        voice->position = 0;
    }

    voice->sound = voice->NextBlock;
    voice->position -= voice->length;
    voice->length = min(voice->BlockLength, 0x8000);
    voice->NextBlock += voice->length;
    if (voice->bits == 16)
    {
        voice->NextBlock += voice->length;
    }
    voice->BlockLength -= voice->length;
    voice->length <<= 16;

    return (KeepPlaying);
}

/*---------------------------------------------------------------------
   Function: MV_GetVoice

   Locates the voice with the specified handle.
---------------------------------------------------------------------*/

VoiceNode *MV_GetVoice(
    int handle)

{
    VoiceNode *voice;
    unsigned flags;

    flags = DisableInterrupts();

    for (voice = VoiceList.next; voice != &VoiceList; voice = voice->next)
    {
        if (handle == voice->handle)
        {
            break;
        }
    }

    RestoreInterrupts(flags);

    if (voice == &VoiceList)
    {
        MV_SetErrorCode(MV_VoiceNotFound);
    }

    return (voice);
}

/*---------------------------------------------------------------------
   Function: MV_VoicePlaying

   Checks if the voice associated with the specified handle is
   playing.
---------------------------------------------------------------------*/

int MV_VoicePlaying(
    int handle)

{
    VoiceNode *voice;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (FALSE);
    }

    voice = MV_GetVoice(handle);

    if (voice == NULL)
    {
        return (FALSE);
    }

    return (FALSE); // WRONG! HACK TO MAKE SOUND WORK AGAIN! FIX THIS!
}

/*---------------------------------------------------------------------
   Function: MV_KillAllVoices

   Stops output of all currently active voices.
---------------------------------------------------------------------*/

int MV_KillAllVoices(
    void)

{
    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    // Remove all the voices from the list
    while (VoiceList.next != &VoiceList)
    {
        MV_Kill(VoiceList.next->handle);
    }

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_Kill

   Stops output of the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_Kill(
    int handle)

{
    VoiceNode *voice;
    unsigned flags;
    unsigned long callbackval;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    flags = DisableInterrupts();

    voice = MV_GetVoice(handle);
    if (voice == NULL)
    {
        RestoreInterrupts(flags);
        MV_SetErrorCode(MV_VoiceNotFound);
        return (MV_Error);
    }

    callbackval = voice->callbackval;

    MV_StopVoice(voice);

    RestoreInterrupts(flags);

    if (MV_CallBackFunc)
    {
        MV_CallBackFunc(callbackval);
    }

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_VoicesPlaying

   Determines the number of currently active voices.
---------------------------------------------------------------------*/

int MV_VoicesPlaying(
    void)

{
    VoiceNode *voice;
    int NumVoices = 0;
    unsigned flags;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (0);
    }

    flags = DisableInterrupts();

    for (voice = VoiceList.next; voice != &VoiceList; voice = voice->next)
    {
        NumVoices++;
    }

    RestoreInterrupts(flags);

    return (NumVoices);
}

/*---------------------------------------------------------------------
   Function: MV_AllocVoice

   Retrieve an inactive or lower priority voice for output.
---------------------------------------------------------------------*/

VoiceNode *MV_AllocVoice(
    int priority)

{
    VoiceNode *voice;
    VoiceNode *node;
    unsigned flags;

    flags = DisableInterrupts();

    // Check if we have any free voices
    if (LL_Empty(&VoicePool, next, prev))
    {
        // check if we have a higher priority than a voice that is playing.
        voice = VoiceList.next;
        for (node = voice->next; node != &VoiceList; node = node->next)
        {
            if (node->priority < voice->priority)
            {
                voice = node;
            }
        }

        if (priority >= voice->priority)
        {
            MV_Kill(voice->handle);
        }
    }

    // Check if any voices are in the voice pool
    if (LL_Empty(&VoicePool, next, prev))
    {
        // No free voices
        RestoreInterrupts(flags);
        return (NULL);
    }

    voice = VoicePool.next;
    LL_Remove(voice, next, prev);
    RestoreInterrupts(flags);

    // Find a free voice handle
    do
    {
        MV_VoiceHandle++;
        if (MV_VoiceHandle < MV_MinVoiceHandle)
        {
            MV_VoiceHandle = MV_MinVoiceHandle;
        }
    } while (MV_VoicePlaying(MV_VoiceHandle));

    voice->handle = MV_VoiceHandle;

    return (voice);
}

/*---------------------------------------------------------------------
   Function: MV_VoiceAvailable

   Checks if a voice can be play at the specified priority.
---------------------------------------------------------------------*/

int MV_VoiceAvailable(
    int priority)

{
    VoiceNode *voice;
    VoiceNode *node;
    unsigned flags;

    // Check if we have any free voices
    if (!LL_Empty(&VoicePool, next, prev))
    {
        return (TRUE);
    }

    flags = DisableInterrupts();

    // check if we have a higher priority than a voice that is playing.
    voice = VoiceList.next;
    for (node = VoiceList.next; node != &VoiceList; node = node->next)
    {
        if (node->priority < voice->priority)
        {
            voice = node;
        }
    }

    RestoreInterrupts(flags);

    if ((voice != &VoiceList) && (priority >= voice->priority))
    {
        return (TRUE);
    }

    return (FALSE);
}

/*---------------------------------------------------------------------
   Function: MV_SetVoicePitch

   Sets the pitch for the specified voice.
---------------------------------------------------------------------*/

void MV_SetVoicePitch(
    VoiceNode *voice,
    unsigned long rate,
    int pitchoffset)

{
    voice->SamplingRate = rate;
    voice->PitchScale = PITCH_GetScale(pitchoffset);
    voice->RateScale = (rate * voice->PitchScale) / MV_MixRate;

    // Multiply by MixBufferSize - 1
    voice->FixedPointBufferSize = (voice->RateScale * MixBufferSize) -
                                  voice->RateScale;
}

/*---------------------------------------------------------------------
   Function: MV_SetPitch

   Sets the pitch for the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_SetPitch(
    int handle,
    int pitchoffset)

{
    VoiceNode *voice;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    voice = MV_GetVoice(handle);
    if (voice == NULL)
    {
        MV_SetErrorCode(MV_VoiceNotFound);
        return (MV_Error);
    }

    MV_SetVoicePitch(voice, voice->SamplingRate, pitchoffset);

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_SetFrequency

   Sets the frequency for the voice associated with the specified handle.
---------------------------------------------------------------------*/

int MV_SetFrequency(
    int handle,
    int frequency)

{
    VoiceNode *voice;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    voice = MV_GetVoice(handle);
    if (voice == NULL)
    {
        MV_SetErrorCode(MV_VoiceNotFound);
        return (MV_Error);
    }

    MV_SetVoicePitch(voice, frequency, 0);

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_GetVolumeTable

   Returns a pointer to the volume table associated with the specified
   volume.
---------------------------------------------------------------------*/

static short *MV_GetVolumeTable(
    int vol)

{
    int volume;
    short *table;

    volume = MIX_VOLUME(vol);

    table = &MV_VolumeTable[volume];

    return (table);
}

/*---------------------------------------------------------------------
   Function: MV_SetVoiceMixMode

   Selects which method should be used to mix the voice.
---------------------------------------------------------------------*/

static void MV_SetVoiceMixMode(
    VoiceNode *voice)

{
    unsigned flags;
    int test;

    flags = DisableInterrupts();

    test = T_DEFAULT;
    if (MV_Bits == 8)
    {
        test |= T_8BITS;
    }

    if (voice->bits == 16)
    {
        test |= T_16BITSOURCE;
    }

    if (MV_Channels == 1)
    {
        test |= T_MONO;
    }
    else
    {
        if (IS_QUIET(voice->RightVolume))
        {
            test |= T_RIGHTQUIET;
        }
        else if (IS_QUIET(voice->LeftVolume))
        {
            test |= T_LEFTQUIET;
        }
    }

    // Default case
    voice->mix = MV_Mix8BitMono;

    switch (test)
    {
    case T_8BITS | T_MONO | T_16BITSOURCE:
        voice->mix = MV_Mix8BitMono16;
        break;

    case T_8BITS | T_MONO:
        voice->mix = MV_Mix8BitMono;
        break;

    case T_8BITS | T_16BITSOURCE | T_LEFTQUIET:
        MV_LeftVolume = MV_RightVolume;
        voice->mix = MV_Mix8BitMono16;
        break;

    case T_8BITS | T_LEFTQUIET:
        MV_LeftVolume = MV_RightVolume;
        voice->mix = MV_Mix8BitMono;
        break;

    case T_8BITS | T_16BITSOURCE | T_RIGHTQUIET:
        voice->mix = MV_Mix8BitMono16;
        break;

    case T_8BITS | T_RIGHTQUIET:
        voice->mix = MV_Mix8BitMono;
        break;

    case T_8BITS | T_16BITSOURCE:
        voice->mix = MV_Mix8BitStereo16;
        break;

    case T_8BITS:
        voice->mix = MV_Mix8BitStereo;
        break;

    case T_MONO | T_16BITSOURCE:
        voice->mix = MV_Mix16BitMono16;
        break;

    case T_MONO:
        voice->mix = MV_Mix16BitMono;
        break;

    case T_16BITSOURCE | T_LEFTQUIET:
        MV_LeftVolume = MV_RightVolume;
        voice->mix = MV_Mix16BitMono16;
        break;

    case T_LEFTQUIET:
        MV_LeftVolume = MV_RightVolume;
        voice->mix = MV_Mix16BitMono;
        break;

    case T_16BITSOURCE | T_RIGHTQUIET:
        voice->mix = MV_Mix16BitMono16;
        break;

    case T_RIGHTQUIET:
        voice->mix = MV_Mix16BitMono;
        break;

    case T_16BITSOURCE:
        voice->mix = MV_Mix16BitStereo16;
        break;

    case T_SIXTEENBIT_STEREO:
        voice->mix = MV_Mix16BitStereo;
        break;

    default:
        voice->mix = MV_Mix8BitMono;
    }

    RestoreInterrupts(flags);
}

/*---------------------------------------------------------------------
   Function: MV_SetVoiceVolume

   Sets the stereo and mono volume level of the voice associated
   with the specified handle.
---------------------------------------------------------------------*/

void MV_SetVoiceVolume(
    VoiceNode *voice,
    int vol,
    int left,
    int right)

{
    if (MV_Channels == 1)
    {
        left = vol;
        right = vol;
    }

    if (MV_SwapLeftRight)
    {
        // SBPro uses reversed panning
        voice->LeftVolume = MV_GetVolumeTable(right);
        voice->RightVolume = MV_GetVolumeTable(left);
    }
    else
    {
        voice->LeftVolume = MV_GetVolumeTable(left);
        voice->RightVolume = MV_GetVolumeTable(right);
    }

    MV_SetVoiceMixMode(voice);
}

/*---------------------------------------------------------------------
   Function: MV_EndLooping

   Stops the voice associated with the specified handle from looping
   without stoping the sound.
---------------------------------------------------------------------*/

int MV_EndLooping(
    int handle)

{
    VoiceNode *voice;
    unsigned flags;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    flags = DisableInterrupts();

    voice = MV_GetVoice(handle);
    if (voice == NULL)
    {
        RestoreInterrupts(flags);
        MV_SetErrorCode(MV_VoiceNotFound);
        return (MV_Warning);
    }

    voice->LoopCount = 0;
    voice->LoopStart = NULL;
    voice->LoopEnd = NULL;

    RestoreInterrupts(flags);

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_SetPan

   Sets the stereo and mono volume level of the voice associated
   with the specified handle.
---------------------------------------------------------------------*/

int MV_SetPan(
    int handle,
    int vol,
    int left,
    int right)

{
    VoiceNode *voice;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    voice = MV_GetVoice(handle);
    if (voice == NULL)
    {
        MV_SetErrorCode(MV_VoiceNotFound);
        return (MV_Warning);
    }

    MV_SetVoiceVolume(voice, vol, left, right);

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_SetMixMode

   Prepares Multivoc to play stereo of mono digitized sounds.
---------------------------------------------------------------------*/

int MV_SetMixMode(
    int numchannels,
    int samplebits)

{
    int mode;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    mode = 0;
    if (numchannels == 2)
    {
        mode |= STEREO;
    }
    if (samplebits == 16)
    {
        mode |= SIXTEEN_BIT;
    }

    switch (MV_SoundCard)
    {
    case UltraSound:
        MV_MixMode = mode;
        break;

    case SoundBlaster:
    case Awe32:
        MV_MixMode = BLASTER_SetMixMode(mode);
        break;

    case ProAudioSpectrum:
    case SoundMan16:
        MV_MixMode = PAS_SetMixMode(mode);
        break;

    case SoundScape:
        MV_MixMode = SOUNDSCAPE_SetMixMode(mode);
        break;

#ifndef SOUNDSOURCE_OFF
    case SoundSource:
    case TandySoundSource:
        MV_MixMode = SS_SetMixMode(mode);
        break;
#endif
    }

    MV_Channels = 1;
    if (MV_MixMode & STEREO)
    {
        MV_Channels = 2;
    }

    MV_Bits = 8;
    if (MV_MixMode & SIXTEEN_BIT)
    {
        MV_Bits = 16;
    }

    MV_BuffShift = 7 + MV_Channels;
    MV_SampleSize = sizeof(MONO8) * MV_Channels;

    if (MV_Bits == 8)
    {
        MV_Silence = SILENCE_8BIT;
    }
    else
    {
        MV_Silence = SILENCE_16BIT;
        MV_BuffShift += 1;
        MV_SampleSize *= 2;
    }

    MV_BufferSize = MixBufferSize * MV_SampleSize;
    MV_NumberOfBuffers = TotalBufferSize / MV_BufferSize;
    MV_BufferLength = TotalBufferSize;

    MV_RightChannelOffset = MV_SampleSize / 2;
    if ((MV_SoundCard == UltraSound) && (MV_Channels == 2))
    {
        MV_SampleSize /= 2;
        MV_BufferSize /= 2;
        MV_RightChannelOffset = MV_BufferSize * MV_NumberOfBuffers;
        MV_BufferLength /= 2;
    }

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_StartPlayback

   Starts the sound playback engine.
---------------------------------------------------------------------*/

int MV_StartPlayback(
    void)

{
    int status;
    int buffer;

    // Initialize the buffers
    ClearBuffer_DW(MV_MixBuffer[0], MV_Silence, TotalBufferSize >> 2);
    for (buffer = 0; buffer < MV_NumberOfBuffers; buffer++)
    {
        MV_BufferEmpty[buffer] = TRUE;
    }

    // Set the mix buffer variables
    MV_MixPage = 1;

    MV_MixFunction = MV_Mix;

    //JIM
    //   MV_MixRate = MV_RequestedMixRate;
    //   return( MV_Ok );

    // Start playback
    switch (MV_SoundCard)
    {
    case SoundBlaster:
    case Awe32:
        status = BLASTER_BeginBufferedPlayback(MV_MixBuffer[0],
                                               TotalBufferSize, MV_NumberOfBuffers,
                                               MV_RequestedMixRate, MV_MixMode, MV_ServiceVoc);

        if (status != BLASTER_Ok)
        {
            MV_SetErrorCode(MV_BlasterError);
            return (MV_Error);
        }

        MV_MixRate = BLASTER_GetPlaybackRate();
        MV_DMAChannel = BLASTER_DMAChannel;
        break;

    case UltraSound:

        status = GUSWAVE_StartDemandFeedPlayback(MV_ServiceGus, 1,
                                                 MV_Bits, MV_RequestedMixRate, 0, (MV_Channels == 1) ? 0 : 24, 255, 0xffff, 0);
        if (status < GUSWAVE_Ok)
        {
            MV_SetErrorCode(MV_BlasterError);
            return (MV_Error);
        }

        if (MV_Channels == 2)
        {
            status = GUSWAVE_StartDemandFeedPlayback(MV_ServiceRightGus, 1,
                                                     MV_Bits, MV_RequestedMixRate, 0, 8, 255, 0xffff, 0);
            if (status < GUSWAVE_Ok)
            {
                GUSWAVE_KillAllVoices();
                MV_SetErrorCode(MV_BlasterError);
                return (MV_Error);
            }
        }

        MV_MixRate = MV_RequestedMixRate;
        MV_DMAChannel = -1;
        break;

    case ProAudioSpectrum:
    case SoundMan16:
        status = PAS_BeginBufferedPlayback(MV_MixBuffer[0],
                                           TotalBufferSize, MV_NumberOfBuffers,
                                           MV_RequestedMixRate, MV_MixMode, MV_ServiceVoc);

        if (status != PAS_Ok)
        {
            MV_SetErrorCode(MV_PasError);
            return (MV_Error);
        }

        MV_MixRate = PAS_GetPlaybackRate();
        MV_DMAChannel = PAS_DMAChannel;
        break;

    case SoundScape:
        status = SOUNDSCAPE_BeginBufferedPlayback(MV_MixBuffer[0],
                                                  TotalBufferSize, MV_NumberOfBuffers, MV_RequestedMixRate,
                                                  MV_MixMode, MV_ServiceVoc);

        if (status != SOUNDSCAPE_Ok)
        {
            MV_SetErrorCode(MV_SoundScapeError);
            return (MV_Error);
        }

        MV_MixRate = SOUNDSCAPE_GetPlaybackRate();
        MV_DMAChannel = SOUNDSCAPE_DMAChannel;
        break;

#ifndef SOUNDSOURCE_OFF
    case SoundSource:
    case TandySoundSource:
        SS_BeginBufferedPlayback(MV_MixBuffer[0],
                                 TotalBufferSize, MV_NumberOfBuffers,
                                 MV_ServiceVoc);
        MV_MixRate = SS_SampleRate;
        MV_DMAChannel = -1;
        break;
#endif
    }

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_StopPlayback

   Stops the sound playback engine.
---------------------------------------------------------------------*/

void MV_StopPlayback(
    void)

{
    VoiceNode *voice;
    VoiceNode *next;
    unsigned flags;

    // Stop sound playback
    switch (MV_SoundCard)
    {
    case SoundBlaster:
    case Awe32:
        BLASTER_StopPlayback();
        break;

    case UltraSound:
        GUSWAVE_KillAllVoices();
        break;

    case ProAudioSpectrum:
    case SoundMan16:
        PAS_StopPlayback();
        break;

    case SoundScape:
        SOUNDSCAPE_StopPlayback();
        break;

#ifndef SOUNDSOURCE_OFF
    case SoundSource:
    case TandySoundSource:
        SS_StopPlayback();
        break;
#endif
    }

    // Make sure all callbacks are done.
    flags = DisableInterrupts();

    for (voice = VoiceList.next; voice != &VoiceList; voice = next)
    {
        next = voice->next;

        MV_StopVoice(voice);

        if (MV_CallBackFunc)
        {
            MV_CallBackFunc(voice->callbackval);
        }
    }

    RestoreInterrupts(flags);
}

/*---------------------------------------------------------------------
   Function: MV_StartDemandFeedPlayback

   Plays a digitized sound from a user controlled buffering system.
---------------------------------------------------------------------*/

int MV_StartDemandFeedPlayback(
    void (*function)(char **ptr, unsigned long *length),
    int rate,
    int pitchoffset,
    int vol,
    int left,
    int right,
    int priority,
    unsigned long callbackval)

{
    VoiceNode *voice;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    // Request a voice from the voice pool
    voice = MV_AllocVoice(priority);
    if (voice == NULL)
    {
        MV_SetErrorCode(MV_NoVoices);
        return (MV_Error);
    }

    voice->wavetype = DemandFeed;
    voice->bits = 8;
    voice->GetSound = MV_GetNextDemandFeedBlock;
    voice->NextBlock = NULL;
    voice->DemandFeed = function;
    voice->LoopStart = NULL;
    voice->LoopCount = 0;
    voice->BlockLength = 0;
    voice->position = 0;
    voice->sound = NULL;
    voice->length = 0;
    voice->BlockLength = 0;
    voice->Playing = TRUE;
    voice->next = NULL;
    voice->prev = NULL;
    voice->priority = priority;
    voice->callbackval = callbackval;

    MV_SetVoicePitch(voice, rate, pitchoffset);
    MV_SetVoiceVolume(voice, vol, left, right);
    MV_PlayVoice(voice);

    return (voice->handle);
}

/*---------------------------------------------------------------------
   Function: MV_PlayLoopedRaw

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayLoopedRaw(
    char *ptr,
    unsigned long length,
    char *loopstart,
    char *loopend,
    unsigned rate,
    int pitchoffset,
    int vol,
    int left,
    int right,
    int priority,
    unsigned long callbackval)

{
    VoiceNode *voice;

    if (!MV_Installed)
    {
        MV_SetErrorCode(MV_NotInstalled);
        return (MV_Error);
    }

    // Request a voice from the voice pool
    voice = MV_AllocVoice(priority);
    if (voice == NULL)
    {
        MV_SetErrorCode(MV_NoVoices);
        return (MV_Error);
    }

    voice->wavetype = Raw;
    voice->bits = 8;
    voice->GetSound = MV_GetNextRawBlock;
    voice->Playing = TRUE;
    voice->NextBlock = ptr;
    voice->position = 0;
    voice->BlockLength = length;
    voice->length = 0;
    voice->next = NULL;
    voice->prev = NULL;
    voice->priority = priority;
    voice->callbackval = callbackval;
    voice->LoopStart = loopstart;
    voice->LoopEnd = loopend;
    voice->LoopSize = (voice->LoopEnd - voice->LoopStart) + 1;

    MV_SetVoicePitch(voice, rate, pitchoffset);
    MV_SetVoiceVolume(voice, vol, left, right);
    MV_PlayVoice(voice);

    return (voice->handle);
}

/*---------------------------------------------------------------------
   Function: MV_PlayRaw

   Begin playback of sound data with the given sound levels and
   priority.
---------------------------------------------------------------------*/

int MV_PlayRaw(
    char *ptr,
    unsigned long length,
    unsigned rate,
    int pitchoffset,
    int vol,
    int left,
    int right,
    int priority,
    unsigned long callbackval)

{
    int status;

    status = MV_PlayLoopedRaw(ptr, length, NULL, NULL, rate, pitchoffset,
                              vol, left, right, priority, callbackval);

    return (status);
}

/*---------------------------------------------------------------------
   Function: MV_LockEnd

   Used for determining the length of the functions to lock in memory.
---------------------------------------------------------------------*/

static void MV_LockEnd(
    void)

{
}

/*---------------------------------------------------------------------
   Function: MV_CreateVolumeTable

   Create the table used to convert sound data to a specific volume
   level.
---------------------------------------------------------------------*/

void MV_CreateVolumeTable(
    int index,
    int volume,
    int MaxVolume)

{
    int val;
    int level;
    int i;

    level = (volume * MaxVolume) / MV_MaxTotalVolume;
    if (MV_Bits == 16)
    {
        for (i = 0; i < 65536; i += 256)
        {
            val = i - 0x8000;
            val *= level;
            val /= MV_MaxVolume;
            MV_VolumeTable[index][i / 256] = val;
        }
    }
    else
    {
        for (i = 0; i < 256; i++)
        {
            val = i - 0x80;
            val *= level;
            val /= MV_MaxVolume;
            MV_VolumeTable[volume][i] = val;
        }
    }
}

/*---------------------------------------------------------------------
   Function: MV_CalcVolume

   Create the table used to convert sound data to a specific volume
   level.
---------------------------------------------------------------------*/

void MV_CalcVolume(
    int MaxVolume)

{
    int volume;

    for (volume = 0; volume < 128; volume++)
    {
        MV_HarshClipTable[volume] = 0;
        MV_HarshClipTable[volume + 384] = 255;
    }
    for (volume = 0; volume < 256; volume++)
    {
        MV_HarshClipTable[volume + 128] = volume;
    }

    // For each volume level, create a translation table with the
    // appropriate volume calculated.
    for (volume = 0; volume <= MV_MaxVolume; volume++)
    {
        MV_CreateVolumeTable(volume, volume, MaxVolume);
    }
}

/*---------------------------------------------------------------------
   Function: MV_CalcPanTable

   Create the table used to determine the stereo volume level of
   a sound located at a specific angle and distance from the listener.
---------------------------------------------------------------------*/

void MV_CalcPanTable(
    void)

{
    int level;
    int angle;
    int distance;
    int HalfAngle;
    int ramp;

    HalfAngle = (MV_NumPanPositions / 2);

    for (distance = 0; distance <= MV_MaxVolume; distance++)
    {
        level = (255 * (MV_MaxVolume - distance)) / MV_MaxVolume;
        for (angle = 0; angle <= HalfAngle / 2; angle++)
        {
            ramp = level - ((level * angle) /
                            (MV_NumPanPositions / 4));

            MV_PanTable[angle][distance].left = ramp;
            MV_PanTable[HalfAngle - angle][distance].left = ramp;
            MV_PanTable[HalfAngle + angle][distance].left = level;
            MV_PanTable[MV_MaxPanPosition - angle][distance].left = level;

            MV_PanTable[angle][distance].right = level;
            MV_PanTable[HalfAngle - angle][distance].right = level;
            MV_PanTable[HalfAngle + angle][distance].right = ramp;
            MV_PanTable[MV_MaxPanPosition - angle][distance].right = ramp;
        }
    }
}

/*---------------------------------------------------------------------
   Function: MV_SetVolume

   Sets the volume of digitized sound playback.
---------------------------------------------------------------------*/

void MV_SetVolume(
    int volume)

{
    volume = max(0, volume);
    volume = min(volume, MV_MaxTotalVolume);

    MV_TotalVolume = volume;

    // Calculate volume table
    MV_CalcVolume(volume);
}

/*---------------------------------------------------------------------
   Function: MV_SetCallBack

   Set the function to call when a voice stops.
---------------------------------------------------------------------*/

void MV_SetCallBack(
    void (*function)(unsigned long))

{
    MV_CallBackFunc = function;
}

/*---------------------------------------------------------------------
   Function: MV_SetReverseStereo

   Set the orientation of the left and right channels.
---------------------------------------------------------------------*/

void MV_SetReverseStereo(
    int setting)

{
    MV_SwapLeftRight = setting;
}

/*---------------------------------------------------------------------
   Function: MV_GetReverseStereo

   Returns the orientation of the left and right channels.
---------------------------------------------------------------------*/

int MV_GetReverseStereo(
    void)

{
    return (MV_SwapLeftRight);
}

/*---------------------------------------------------------------------
   Function: MV_TestPlayback

   Checks if playback has started.
---------------------------------------------------------------------*/

int MV_TestPlayback(
    void)

{
    unsigned flags;
    long time;
    int start;
    int status;
    int pos;

    if (MV_SoundCard == UltraSound)
    {
        return (MV_Ok);
    }

    flags = DisableInterrupts();
    _enable();

    status = MV_Error;
    start = MV_MixPage;
    time = clock() + CLOCKS_PER_SEC * 2;

    while (clock() < time)
    {
        if (MV_MixPage != start)
        {
            status = MV_Ok;
        }
    }

    RestoreInterrupts(flags);

    if (status != MV_Ok)
    {
        // Just in case an error doesn't get reported
        MV_SetErrorCode(MV_DMAFailure);

        switch (MV_SoundCard)
        {
        case SoundBlaster:
        case Awe32:
            pos = BLASTER_GetCurrentPos();
            break;

        case ProAudioSpectrum:
        case SoundMan16:
            pos = PAS_GetCurrentPos();
            break;

        case SoundScape:
            pos = SOUNDSCAPE_GetCurrentPos();
            break;

#ifndef SOUNDSOURCE_OFF
        case SoundSource:
        case TandySoundSource:
            MV_SetErrorCode(MV_SoundSourceFailure);
            pos = -1;
            break;
#endif

        default:
            MV_SetErrorCode(MV_UnsupportedCard);
            pos = -2;
            break;
        }

        if (pos > 0)
        {
            MV_SetErrorCode(MV_IrqFailure);
        }
        else if (pos == 0)
        {
            if (MV_Bits == 16)
            {
                MV_SetErrorCode(MV_DMA16Failure);
            }
            else
            {
                MV_SetErrorCode(MV_DMAFailure);
            }
        }
    }

    return (status);
}

/*---------------------------------------------------------------------
   Function: MV_Init

   Perform the initialization of variables and memory used by
   Multivoc.
---------------------------------------------------------------------*/

int MV_Init(
    int soundcard,
    int MixRate,
    int Voices,
    int numchannels,
    int samplebits)

{
    char *ptr;
    int status;
    int buffer;
    int index;

    if (MV_Installed)
    {
        MV_Shutdown();
    }

    MV_SetErrorCode(MV_Ok);

    status = MV_LockMemory();
    if (status != MV_Ok)
    {
        return (status);
    }

    MV_TotalMemory = Voices * sizeof(VoiceNode) + sizeof(HARSH_CLIP_TABLE_8);
    status = USRHOOKS_GetMem((void **)&ptr, MV_TotalMemory);
    if (status != USRHOOKS_Ok)
    {
        MV_UnlockMemory();
        MV_SetErrorCode(MV_NoMem);
        return (MV_Error);
    }

    status = DPMI_LockMemory(ptr, MV_TotalMemory);
    if (status != DPMI_Ok)
    {
        USRHOOKS_FreeMem(ptr);
        MV_UnlockMemory();
        MV_SetErrorCode(MV_DPMI_Error);
        return (MV_Error);
    }

    MV_Voices = (VoiceNode *)ptr;
    MV_HarshClipTable = ptr + (MV_TotalMemory - sizeof(HARSH_CLIP_TABLE_8));

    // Set number of voices before calculating volume table
    MV_MaxVoices = Voices;

    LL_Reset(&VoiceList, next, prev);
    LL_Reset(&VoicePool, next, prev);

    for (index = 0; index < Voices; index++)
    {
        LL_Add(&VoicePool, &MV_Voices[index], next, prev);
    }

    // Allocate mix buffer within 1st megabyte
    status = DPMI_GetDOSMemory((void **)&ptr, &MV_BufferDescriptor,
                               2 * TotalBufferSize);

    if (status)
    {
        DPMI_UnlockMemory(MV_Voices, MV_TotalMemory);
        USRHOOKS_FreeMem(MV_Voices);
        MV_Voices = NULL;
        MV_TotalMemory = 0;
        MV_UnlockMemory();

        MV_SetErrorCode(MV_NoMem);
        return (MV_Error);
    }

    MV_SetReverseStereo(FALSE);

    // Initialize the sound card
    switch (soundcard)
    {
    case UltraSound:
        status = GUSWAVE_Init(2);
        if (status != GUSWAVE_Ok)
        {
            //JIM
            MV_SetErrorCode(MV_BlasterError);
        }
        break;

    case SoundBlaster:
    case Awe32:
        status = BLASTER_Init();
        if (status != BLASTER_Ok)
        {
            MV_SetErrorCode(MV_BlasterError);
        }

        if ((BLASTER_Config.Type == SBPro) ||
            (BLASTER_Config.Type == SBPro2))
        {
            MV_SetReverseStereo(TRUE);
        }
        break;

    case ProAudioSpectrum:
    case SoundMan16:
        status = PAS_Init();
        if (status != PAS_Ok)
        {
            MV_SetErrorCode(MV_PasError);
        }
        break;

    case SoundScape:
        status = SOUNDSCAPE_Init();
        if (status != SOUNDSCAPE_Ok)
        {
            MV_SetErrorCode(MV_SoundScapeError);
        }
        break;

#ifndef SOUNDSOURCE_OFF
    case SoundSource:
    case TandySoundSource:
        status = SS_Init(soundcard);
        if (status != SS_Ok)
        {
            MV_SetErrorCode(MV_SoundSourceError);
        }
        break;
#endif

    default:
        MV_SetErrorCode(MV_UnsupportedCard);
        break;
    }

    if (MV_ErrorCode != MV_Ok)
    {
        status = MV_ErrorCode;

        DPMI_UnlockMemory(MV_Voices, MV_TotalMemory);
        USRHOOKS_FreeMem(MV_Voices);
        MV_Voices = NULL;
        MV_TotalMemory = 0;

        DPMI_FreeDOSMemory(MV_BufferDescriptor);
        MV_UnlockMemory();

        MV_SetErrorCode(status);
        return (MV_Error);
    }

    MV_SoundCard = soundcard;
    MV_Installed = TRUE;
    MV_CallBackFunc = NULL;
    MV_ReverbLevel = 0;
    MV_ReverbTable = NULL;

    // Set the sampling rate
    MV_RequestedMixRate = MixRate;

    // Set Mixer to play stereo digitized sound
    MV_SetMixMode(numchannels, samplebits);
    MV_ReverbDelay = MV_BufferSize * 3;

    // Make sure we don't cross a physical page
    if (((unsigned long)ptr & 0xffff) + TotalBufferSize > 0x10000)
    {
        ptr = (char *)(((unsigned long)ptr & 0xff0000) + 0x10000);
    }

    MV_MixBuffer[MV_NumberOfBuffers] = ptr;
    for (buffer = 0; buffer < MV_NumberOfBuffers; buffer++)
    {
        MV_MixBuffer[buffer] = ptr;
        ptr += MV_BufferSize;
    }

    // Calculate pan table
    MV_CalcPanTable();

    MV_SetVolume(MV_MaxTotalVolume);

    // Start the playback engine
    status = MV_StartPlayback();
    if (status != MV_Ok)
    {
        // Preserve error code while we shutdown.
        status = MV_ErrorCode;
        MV_Shutdown();
        MV_SetErrorCode(status);
        return (MV_Error);
    }

    if (MV_TestPlayback() != MV_Ok)
    {
        status = MV_ErrorCode;
        MV_Shutdown();
        MV_SetErrorCode(status);
        return (MV_Error);
    }

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_Shutdown

   Restore any resources allocated by Multivoc back to the system.
---------------------------------------------------------------------*/

int MV_Shutdown(
    void)

{
    int buffer;
    unsigned flags;

    if (!MV_Installed)
    {
        return (MV_Ok);
    }

    flags = DisableInterrupts();

    MV_KillAllVoices();

    MV_Installed = FALSE;

    // Stop the sound playback engine
    MV_StopPlayback();

    // Shutdown the sound card
    switch (MV_SoundCard)
    {
    case UltraSound:
        GUSWAVE_Shutdown();
        break;

    case SoundBlaster:
    case Awe32:
        BLASTER_Shutdown();
        break;

    case ProAudioSpectrum:
    case SoundMan16:
        PAS_Shutdown();
        break;

    case SoundScape:
        SOUNDSCAPE_Shutdown();
        break;

#ifndef SOUNDSOURCE_OFF
    case SoundSource:
    case TandySoundSource:
        SS_Shutdown();
        break;
#endif
    }

    RestoreInterrupts(flags);

    // Free any voices we allocated
    DPMI_UnlockMemory(MV_Voices, MV_TotalMemory);
    USRHOOKS_FreeMem(MV_Voices);
    MV_Voices = NULL;
    MV_TotalMemory = 0;

    LL_Reset(&VoiceList, next, prev);
    LL_Reset(&VoicePool, next, prev);

    MV_MaxVoices = 1;

    // Release the descriptor from our mix buffer
    DPMI_FreeDOSMemory(MV_BufferDescriptor);
    for (buffer = 0; buffer < NumberOfBuffers; buffer++)
    {
        MV_MixBuffer[buffer] = NULL;
    }

    return (MV_Ok);
}

/*---------------------------------------------------------------------
   Function: MV_UnlockMemory

   Unlocks all neccessary data.
---------------------------------------------------------------------*/

void MV_UnlockMemory(
    void)

{
    PITCH_UnlockMemory();

    DPMI_UnlockMemoryRegion(MV_LockStart, MV_LockEnd);
    DPMI_Unlock(MV_VolumeTable);
    DPMI_Unlock(MV_PanTable);
    DPMI_Unlock(MV_Installed);
    DPMI_Unlock(MV_SoundCard);
    DPMI_Unlock(MV_TotalVolume);
    DPMI_Unlock(MV_MaxVoices);
    DPMI_Unlock(MV_BufferSize);
    DPMI_Unlock(MV_BufferLength);
    DPMI_Unlock(MV_SampleSize);
    DPMI_Unlock(MV_NumberOfBuffers);
    DPMI_Unlock(MV_MixMode);
    DPMI_Unlock(MV_Channels);
    DPMI_Unlock(MV_Bits);
    DPMI_Unlock(MV_Silence);
    DPMI_Unlock(MV_SwapLeftRight);
    DPMI_Unlock(MV_RequestedMixRate);
    DPMI_Unlock(MV_MixRate);
    DPMI_Unlock(MV_BufferDescriptor);
    DPMI_Unlock(MV_MixBuffer);
    DPMI_Unlock(MV_BufferEmpty);
    DPMI_Unlock(MV_Voices);
    DPMI_Unlock(VoiceList);
    DPMI_Unlock(VoicePool);
    DPMI_Unlock(MV_MixPage);
    DPMI_Unlock(MV_VoiceHandle);
    DPMI_Unlock(MV_CallBackFunc);
    DPMI_Unlock(MV_MixFunction);
    DPMI_Unlock(MV_HarshClipTable);
    DPMI_Unlock(MV_MixDestination);
    DPMI_Unlock(MV_LeftVolume);
    DPMI_Unlock(MV_RightVolume);
    DPMI_Unlock(MV_MixPosition);
    DPMI_Unlock(MV_ErrorCode);
    DPMI_Unlock(MV_DMAChannel);
    DPMI_Unlock(MV_BuffShift);
    DPMI_Unlock(MV_ReverbLevel);
    DPMI_Unlock(MV_ReverbDelay);
    DPMI_Unlock(MV_ReverbTable);
}

/*---------------------------------------------------------------------
   Function: MV_LockMemory

   Locks all neccessary data.
---------------------------------------------------------------------*/

int MV_LockMemory(
    void)

{
    int status;
    int pitchstatus;

    status = DPMI_LockMemoryRegion(MV_LockStart, MV_LockEnd);
    status |= DPMI_Lock(MV_VolumeTable);
    status |= DPMI_Lock(MV_PanTable);
    status |= DPMI_Lock(MV_Installed);
    status |= DPMI_Lock(MV_SoundCard);
    status |= DPMI_Lock(MV_TotalVolume);
    status |= DPMI_Lock(MV_MaxVoices);
    status |= DPMI_Lock(MV_BufferSize);
    status |= DPMI_Lock(MV_BufferLength);
    status |= DPMI_Lock(MV_SampleSize);
    status |= DPMI_Lock(MV_NumberOfBuffers);
    status |= DPMI_Lock(MV_MixMode);
    status |= DPMI_Lock(MV_Channels);
    status |= DPMI_Lock(MV_Bits);
    status |= DPMI_Lock(MV_Silence);
    status |= DPMI_Lock(MV_SwapLeftRight);
    status |= DPMI_Lock(MV_RequestedMixRate);
    status |= DPMI_Lock(MV_MixRate);
    status |= DPMI_Lock(MV_BufferDescriptor);
    status |= DPMI_Lock(MV_MixBuffer);
    status |= DPMI_Lock(MV_BufferEmpty);
    status |= DPMI_Lock(MV_Voices);
    status |= DPMI_Lock(VoiceList);
    status |= DPMI_Lock(VoicePool);
    status |= DPMI_Lock(MV_MixPage);
    status |= DPMI_Lock(MV_VoiceHandle);
    status |= DPMI_Lock(MV_CallBackFunc);
    status |= DPMI_Lock(MV_MixFunction);
    status |= DPMI_Lock(MV_HarshClipTable);
    status |= DPMI_Lock(MV_MixDestination);
    status |= DPMI_Lock(MV_LeftVolume);
    status |= DPMI_Lock(MV_RightVolume);
    status |= DPMI_Lock(MV_MixPosition);
    status |= DPMI_Lock(MV_ErrorCode);
    status |= DPMI_Lock(MV_DMAChannel);
    status |= DPMI_Lock(MV_BuffShift);
    status |= DPMI_Lock(MV_ReverbLevel);
    status |= DPMI_Lock(MV_ReverbDelay);
    status |= DPMI_Lock(MV_ReverbTable);

    pitchstatus = PITCH_LockMemory();
    if ((pitchstatus != PITCH_Ok) || (status != DPMI_Ok))
    {
        MV_UnlockMemory();
        MV_SetErrorCode(MV_DPMI_Error);
        return (MV_Error);
    }

    return (MV_Ok);
}
