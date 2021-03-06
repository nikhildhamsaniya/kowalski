/*
 Copyright (c) 2010-2012 Per Gantelius
 
 This software is provided 'as-is', without any express or implied
 warranty. In no event will the authors be held liable for any damages
 arising from the use of this software.
 
 Permission is granted to anyone to use this software for any purpose,
 including commercial applications, and to alter it and redistribute it
 freely, subject to the following restrictions:
 
 1. The origin of this software must not be misrepresented; you must not
 claim that you wrote the original software. If you use this software
 in a product, an acknowledgment in the product documentation would be
 appreciated but is not required.
 
 2. Altered source versions must be plainly marked as such, and must not be
 misrepresented as being the original software.
 
 3. This notice may not be removed or altered from any source
 distribution.
 */

#include <stdio.h>
#include <string.h>

#include "kwl_assert.h"
#include "kwl_audiofileutil.h"
#include "kwl_binarybuilding.h"
#include "kwl_datavalidation.h"
#include "kwl_fileutil.h"
#include "kwl_inputstream.h"
#include "kwl_memory.h"
#include "kwl_fileoutputstream.h"
#include "kwl_enginedatabinary.h"
#include "kwl_sounddefinition.h"
#include "kwl_xmlutil.h"

#define KWL_TEMP_STRING_LENGTH 1024

/*TODO: these shouldnt be global*/
static char** soundDefinitionNames = NULL;
const char* projectXmlPath = NULL;

int kwlFileIsEngineDataBinary(const char* path)
{
    kwlInputStream is;
    kwlError result = kwlInputStream_initWithFile(&is, path);
    if (result != KWL_NO_ERROR)
    {
        return 0;
    }
    
    int isEngineData = 1;
    for (int i = 0; i < KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER_LENGTH; i++)
    {
        char ci = kwlInputStream_readChar(&is);
        if (ci != KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER[i])
        {
            isEngineData = 0;
            break;
        }
    }
    
    kwlInputStream_close(&is);
    
    return isEngineData;
}

/**
 * returns the path up to the top group
 */
static const xmlChar* kwlGetNodePath(xmlNode* currentNode)
{
    xmlNode* path[KWL_TEMP_STRING_LENGTH];
    kwlMemset(path, 0, KWL_TEMP_STRING_LENGTH * sizeof(xmlNode*));
    
    xmlNode* n = currentNode;
    int idx = 0;
    while (!xmlStrEqual(n->parent->name, (xmlChar*)KWL_XML_KOWALSKI_PROJECT_NODE))
    {
        path[idx] = n;
        idx++;
        KWL_ASSERT(idx < KWL_TEMP_STRING_LENGTH);
        n = n->parent;
    }
    
    int numChars = 0;
    xmlChar tempStr[KWL_TEMP_STRING_LENGTH];
    kwlMemset(tempStr, 0, sizeof(xmlChar) * KWL_TEMP_STRING_LENGTH);
    for (int i = idx - 1; i >= 0; i--)
    {
        //id gets freed along with the xml structure
        xmlChar* id = kwlGetAttributeValue(path[i], "id");
        tempStr[numChars] = '/';
        kwlMemcpy(&tempStr[numChars + 1], id, xmlStrlen(id));
        numChars += xmlStrlen(id) + 1;
        KWL_ASSERT(numChars < KWL_TEMP_STRING_LENGTH);
    }
    tempStr[numChars] = '\0';
    
    xmlChar* pathStr = KWL_MALLOCANDZERO(numChars, "path string");
    kwlMemcpy(pathStr, &tempStr[1], numChars); //remove leading slash
    pathStr[numChars - 1] = '\0';
    return pathStr;
}

/**
 * Gather mix buses excluding sub buses
 */
static void kwlGatherMixBusesCallback(xmlNode* currentNode,
                                      void* b,
                                      int* errorOccurred,
                                      kwlLogCallback errorLogCallback)
{
    kwlEngineDataBinary* bin = (kwlEngineDataBinary*)b;
    bin->mixBusesChunk.numMixBuses += 1;
    bin->mixBusesChunk.mixBuses = KWL_REALLOC(bin->mixBusesChunk.mixBuses,
                                              sizeof(kwlMixBusChunk) * bin->mixBusesChunk.numMixBuses,
                                              "xml 2 bin mix bus realloc");
    kwlMixBusChunk* c = &bin->mixBusesChunk.mixBuses[bin->mixBusesChunk.numMixBuses - 1];
    c->id = kwlGetAttributeValueCopy(currentNode, "id");
    KWL_ASSERT(c->id != NULL);
    c->numSubBuses = kwlGetChildCount(currentNode, KWL_XML_MIX_BUS_NODE);
    if (c->numSubBuses > 0)
    {
        c->subBusIndices = KWL_MALLOCANDZERO(c->numSubBuses * sizeof(int), "xml 2 bin sub buses");
    }
}

/**
 * gather sub buses of already gathered mix buses
 */
static void kwlGatherSubBusesCallback(xmlNode* node,
                                      void* b,
                                      int* errorOccurred,
                                      kwlLogCallback errorLogCallback)
{
    kwlEngineDataBinary* bin = (kwlEngineDataBinary*)b;
    
    //find the mix bus to attach the children to
    kwlMixBusChunk* mb = NULL;
    for (int i = 0; i < bin->mixBusesChunk.numMixBuses; i++)
    {
        kwlMixBusChunk* mbi = &bin->mixBusesChunk.mixBuses[i];
        if (strcmp(mbi->id, kwlGetAttributeValue(node, KWL_XML_MIX_BUS_ID)) == 0)
        {
            mb = mbi;
            break;
        }
    }
    KWL_ASSERT(mb != NULL);
    
    int childIdx = 0;
    for (xmlNode* curr = node->children; curr != NULL; curr = curr->next)
    {
        if (!xmlStrEqual(curr->name, (xmlChar*)KWL_XML_MIX_BUS_NODE))
        {
            continue;
        }
        
        //find the index of this sub bus
        const xmlChar* id = kwlGetAttributeValue(curr, "id");
        KWL_ASSERT(id != NULL);
        int idx = -1;
        for (int i = 0; i < bin->mixBusesChunk.numMixBuses; i++)
        {
            if (strcmp(bin->mixBusesChunk.mixBuses[i].id, id) == 0)
            {
                idx = i;
                break;
            }
        }
        
        mb->subBusIndices[childIdx] = idx;
        
        
        KWL_ASSERT(idx >= 0);
        childIdx++;
    }
}

static void kwlCreateMixBusChunk(xmlNode* projectRoot, kwlEngineDataBinary* bin, int* errorOccurred, kwlLogCallback errorLogCallback)
{
    bin->mixBusesChunk.chunkId = KWL_MIX_BUSES_CHUNK_ID;
    
    kwlTraverseNodeTree(projectRoot,
                        KWL_XML_MIX_BUS_NODE,
                        KWL_XML_MIX_BUS_NODE,
                        kwlGatherMixBusesCallback,
                        bin,
                        errorOccurred,
                        errorLogCallback);
    
    /*check that all mix bus names are unique*/
    for (int i = 0; i < bin->mixBusesChunk.numMixBuses; i++)
    {
        const char* idi = bin->mixBusesChunk.mixBuses[i].id;
        for (int j = 0; j < bin->mixBusesChunk.numMixBuses; j++)
        {
            if (j != i)
            {
                const char* idj = bin->mixBusesChunk.mixBuses[j].id;
                if (strcmp(idj, idi) == 0)
                {
                    errorLogCallback("Mix bus id '%s' is not unique.\n", idi);
                    *errorOccurred = 1;
                }
            }
        }
    }
    
    if (*errorOccurred != 0)
    {
        /*if duplicate names were found, return now and skip gathering the sub buses*/
        return;
    }
    
    kwlTraverseNodeTree(projectRoot,
                        KWL_XML_MIX_BUS_NODE,
                        KWL_XML_MIX_BUS_NODE,
                        kwlGatherSubBusesCallback,
                        bin,
                        errorOccurred,
                        errorLogCallback);
    
}

static int kwlGetMixBusIndex(kwlEngineDataBinary* bin, const char* id)
{
    KWL_ASSERT(bin->mixBusesChunk.numMixBuses > 0);
    
    for (int i = 0; i < bin->mixBusesChunk.numMixBuses; i++)
    {
        kwlMixBusChunk* mbi = &bin->mixBusesChunk.mixBuses[i];
        if (strcmp(id, mbi->id) == 0)
        {
            return i;
        }
    }
    
    return -1;
}

static int kwlDoesMixBusExist(kwlEngineDataBinary* bin, const char* id)
{
    for (int i = 0; i < bin->mixBusesChunk.numMixBuses; i++)
    {
        if (strcmp(id, bin->mixBusesChunk.mixBuses[i].id) == 0)
        {
            return 1;
        }
    }
    
    return 0;
}

/**
 * Gather mix presets
 */
static void kwlGatherMixPresetsCallback(xmlNode* node,
                                        void* b,
                                        int* errorOccurred,
                                        kwlLogCallback errorLogCallback)
{
    kwlEngineDataBinary* bin = (kwlEngineDataBinary*)b;
    const int numMixBuses = bin->mixBusesChunk.numMixBuses;
    KWL_ASSERT(numMixBuses > 0);
    
    bin->mixPresetsChunk.numMixPresets += 1;
    bin->mixPresetsChunk.mixPresets = KWL_REALLOC(bin->mixPresetsChunk.mixPresets,
                                                  sizeof(kwlMixPresetChunk) * bin->mixPresetsChunk.numMixPresets,
                                                  "xml 2 bin mix preset realloc");
    kwlMixPresetChunk* c = &bin->mixPresetsChunk.mixPresets[bin->mixPresetsChunk.numMixPresets - 1];
    c->id = kwlGetNodePath(node);
    c->isDefault = kwlGetBoolAttributeValue(node, KWL_XML_MIX_PRESET_DEFAULT);
    c->mixBusIndices = KWL_MALLOCANDZERO(numMixBuses * sizeof(int), "xml 2 bin mix preset bus indices");
    c->gainLeft = KWL_MALLOCANDZERO(numMixBuses * sizeof(int), "xml 2 bin mix preset bus gain l");
    c->gainRight = KWL_MALLOCANDZERO(numMixBuses * sizeof(int), "xml 2 bin mix preset bus gain r");
    c->pitch = KWL_MALLOCANDZERO(numMixBuses * sizeof(int), "xml 2 bin mix preset bus pitch");
    
    int paramSetIdx = 0;
    for (xmlNode* curr = node->children; curr != NULL; curr = curr->next)
    {
        if (xmlStrEqual(curr->name, (xmlChar*)KWL_XML_MIX_BUS_PARAM_SET_NODE))
        {
            //TODO: dont write bus index, use the already established bus order
            char* busId = kwlGetAttributeValue(curr, KWL_XML_MIX_BUS_PARAM_SET_BUS);
            const int busIdx = kwlGetMixBusIndex(bin, busId);
            
            if (busIdx < 0)
            {
                *errorOccurred = 1;
                errorLogCallback("The mix preset '%s' references non-existing mix bus '%s'.\n", c->id, busId);
            }
            
            float gainLeft = kwlGetFloatAttributeValue(curr, KWL_XML_MIX_BUS_PARAM_SET_GAIN_L);
            float gainRight = kwlGetFloatAttributeValue(curr, KWL_XML_MIX_BUS_PARAM_SET_GAIN_R);
            float pitch = kwlGetFloatAttributeValue(curr, KWL_XML_MIX_BUS_PARAM_SET_PITCH);
            
            if (paramSetIdx >= numMixBuses)
            {
                *errorOccurred = 1;
                errorLogCallback("Mix preset '%s' has more parameter sets than the number of mix buses.\n", c->id);
                break;
            }
            c->gainLeft[paramSetIdx] = gainLeft;
            c->gainLeft[paramSetIdx] = gainRight;
            c->pitch[paramSetIdx] = pitch;
            c->mixBusIndices[paramSetIdx] = busIdx;
            paramSetIdx++;
        }
    }
    
    if (paramSetIdx < numMixBuses)
    {
        errorLogCallback("The number of parameter sets for mix preset '%s' is less than the number of mix buses.\n", c->id);
        *errorOccurred = 1;
    }
    
    /*check one to one correspondence between mix bus ids and param set bus refs*/
    if (paramSetIdx == numMixBuses)
    {
        const char** busRefIds = KWL_MALLOCANDZERO(numMixBuses * sizeof(char*), "preset bus ref validation");
        
        for (int i = 0; i < numMixBuses; i++)
        {
            const int indexOfRefedBus = c->mixBusIndices[i];
            const char* idOfRefedBus = bin->mixBusesChunk.mixBuses[indexOfRefedBus].id;
            
            if (indexOfRefedBus < 0)
            {
                /*move to the next bus reference*/
                break;
            }
            
            for (int j = 0; j < numMixBuses; j++)
            {
                if (busRefIds[j] != NULL)
                {
                    if (strcmp(idOfRefedBus, busRefIds[j]) == 0 &&
                        kwlDoesMixBusExist(bin, idOfRefedBus))
                    {
                        errorLogCallback("Mix preset '%s' references the mix bus '%s' more than once.\n", c->id, idOfRefedBus);
                        *errorOccurred = 1;
                    }
                }
            }
            
            int slotFound = 0;
            for (int j = 0; j < numMixBuses; j++)
            {
                if (busRefIds[j] == NULL)
                {
                    slotFound = 1;
                    busRefIds[j] = idOfRefedBus;
                    break;
                }
            }
            
            KWL_ASSERT(slotFound != 0);
        }
        
        KWL_FREE(busRefIds);
    }
}

static void kwlCreateMixPresetChunk(xmlNode* projectRoot,
                                    kwlEngineDataBinary* bin,
                                    int* errorOccurred,
                                    kwlLogCallback errorLogCallback)
{
    bin->mixPresetsChunk.chunkId = KWL_MIX_PRESETS_CHUNK_ID;
    
    kwlTraverseNodeTree(projectRoot,
                        KWL_XML_MIX_PRESET_GROUP_NODE,
                        KWL_XML_MIX_PRESET_NODE,
                        kwlGatherMixPresetsCallback,
                        bin,
                        errorOccurred,
                        errorLogCallback);
    
    /*check that there is exactly one default preset*/
    int numDefaultsFound = 0;
    for (int i = 0; i < bin->mixPresetsChunk.numMixPresets; i++)
    {
        kwlMixPresetChunk* mpi = &bin->mixPresetsChunk.mixPresets[i];
        if (mpi->isDefault != 0)
        {
            numDefaultsFound++;
        }
    }
    
    if (numDefaultsFound == 0)
    {
        errorLogCallback("No default mix preset found.\n");
        *errorOccurred = 1;
    }
    else if (numDefaultsFound > 1)
    {
        errorLogCallback("Multiple default mix presets found.\n");
        *errorOccurred = 1;
    }
}

static void kwlGatherWaveBanksCallback(xmlNode* node,
                                       void* b,
                                       int* errorOccurred,
                                       kwlLogCallback errorLogCallback)
{
    kwlEngineDataBinary* bin = (kwlEngineDataBinary*)b;
    bin->waveBanksChunk.numWaveBanks += 1;
    bin->waveBanksChunk.waveBanks = KWL_REALLOC(bin->waveBanksChunk.waveBanks,
                                                sizeof(kwlWaveBankChunk) * bin->waveBanksChunk.numWaveBanks,
                                                "xml 2 bin wave bank realloc");
    kwlWaveBankChunk* c = &bin->waveBanksChunk.waveBanks[bin->waveBanksChunk.numWaveBanks - 1];
    const xmlChar* path = kwlGetNodePath(node);
    c->id = path;
    c->numAudioDataEntries = kwlGetChildCount(node, KWL_XML_AUDIO_DATA_NODE);
    KWL_ASSERT(c->numAudioDataEntries > 0);
    c->audioDataEntries = KWL_MALLOCANDZERO(c->numAudioDataEntries * sizeof(char*), "xml 2 bin wb entry names");
    
    int idx = 0;
    //printf("reading wavebank %s\n", c->id);
    for (xmlNode* curr = node->children; curr != NULL; curr = curr->next)
    {
        if (xmlStrEqual(curr->name, (xmlChar*)KWL_XML_AUDIO_DATA_NODE))
        {
            
            char* itemPath = kwlGetAttributeValueCopy(curr, KWL_XML_ATTR_REL_PATH);
            c->audioDataEntries[idx] = itemPath;
            idx++;
        }
    }
    
    KWL_ASSERT(path != NULL);
}

static void kwlCreateWaveBankChunk(xmlNode* projectRoot, kwlEngineDataBinary* bin, int* errorOccurred, kwlLogCallback errorLogCallback)
{
    bin->waveBanksChunk.chunkId = KWL_WAVE_BANKS_CHUNK_ID;
    
    /*collect wave banks and their ids and audio data entry counts*/
    kwlTraverseNodeTree(projectRoot,
                        KWL_XML_WAVE_BANK_GROUP_NODE,
                        KWL_XML_WAVE_BANK_NODE,
                        kwlGatherWaveBanksCallback,
                        bin,
                        errorOccurred,
                        errorLogCallback);
    /*store the total number of audio data items*/
    int totalNumItems = 0;
    for (int i = 0; i < bin->waveBanksChunk.numWaveBanks; i++)
    {
        kwlWaveBankChunk* wbi = &bin->waveBanksChunk.waveBanks[i];
        totalNumItems += wbi->numAudioDataEntries;
    }
    bin->waveBanksChunk.numAudioDataItemsTotal = totalNumItems;
}

static int kwlGetWaveBankIndex(kwlEngineDataBinary* bin, const char* id)
{
    for (int i = 0; i < bin->waveBanksChunk.numWaveBanks; i++)
    {
        kwlWaveBankChunk* wbi = &bin->waveBanksChunk.waveBanks[i];
        if (strcmp(id, wbi->id) == 0)
        {
            return i;
        }
    }
    
    return -1;
}

static int kwlGetSoundIndex(kwlEngineDataBinary* bin, const char* id)
{
    KWL_ASSERT(soundDefinitionNames != NULL);
    for (int i = 0; i < bin->soundsChunk.numSoundDefinitions; i++)
    {
        if (strcmp(id, soundDefinitionNames[i]) == 0)
        {
            return i;
        }
    }
    
    return -1;
}

static int kwlGetAudioDataIndex(kwlWaveBankChunk* wb, const char* id)
{
    KWL_ASSERT(wb != NULL);
    KWL_ASSERT(id != NULL);
    KWL_ASSERT(wb->audioDataEntries != NULL);
    
    for (int i = 0; i < wb->numAudioDataEntries; i++)
    {
        
        KWL_ASSERT(wb->audioDataEntries[i] != NULL);
        if (strcmp(id, wb->audioDataEntries[i]) == 0)
        {
            return i;
        }
    }
    
    return -1;
}

static int kwlGetSoundPlaybackModeInt(xmlChar* playbackMode)
{
    KWL_ASSERT(playbackMode != NULL);
    
    if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_PLAYBACK_MODE_RANDOM) == 0)
    {
        return 0;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_PLAYBACK_MODE_RANDOM_NO_REPEAT) == 0)
    {
        return 1;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_PLAYBACK_MODE_SEQUENTIAL) == 0)
    {
        return 2;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_PLAYBACK_MODE_SEQUENTIAL_NO_RESET) == 0)
    {
        return 3;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_PLAYBACK_MODE_IN_RANDOM_OUT) == 0)
    {
        return 4;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_PLAYBACK_MODE_IN_RANDOM_NO_REPEAT_OUT) == 0)
    {
        return 5;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_PLAYBACK_MODE_IN_SEQUENTIAL_OUT) == 0)
    {
        return 6;
    }
    
    KWL_ASSERT(0 && "invalid sound playback mode");
    
    return -1;
}



static void kwlGatherSoundsCallback(xmlNode* node,
                                    void* b,
                                    int* errorOccurred,
                                    kwlLogCallback errorLogCallback)
{
    *errorOccurred = 0;
    
    kwlEngineDataBinary* bin = (kwlEngineDataBinary*)b;
    bin->soundsChunk.numSoundDefinitions += 1;
    bin->soundsChunk.soundDefinitions = KWL_REALLOC(bin->soundsChunk.soundDefinitions,
                                                    sizeof(kwlSoundChunk) * bin->soundsChunk.numSoundDefinitions,
                                                    "xml 2 bin sound realloc");
    kwlSoundChunk* c = &bin->soundsChunk.soundDefinitions[bin->soundsChunk.numSoundDefinitions - 1];
    
    soundDefinitionNames = KWL_REALLOC(soundDefinitionNames,
                                       bin->soundsChunk.numSoundDefinitions * sizeof(char*),
                                       "sound def id list realloc");
    soundDefinitionNames[bin->soundsChunk.numSoundDefinitions - 1] = kwlGetNodePath(node);
    
    c->gain = kwlGetFloatAttributeValue(node, KWL_XML_SOUND_GAIN);
    c->gainVariation = kwlGetFloatAttributeValue(node, KWL_XML_SOUND_GAIN_VAR);
    c->pitch = kwlGetFloatAttributeValue(node, KWL_XML_SOUND_PITCH);
    c->pitchVariation = kwlGetFloatAttributeValue(node, KWL_XML_SOUND_PITCH_VAR);
    c->deferStop = kwlGetIntAttributeValue(node, KWL_XML_SOUND_DEFER_STOP);
    c->playbackCount = kwlGetIntAttributeValue(node, KWL_XML_SOUND_PLAYBACK_COUNT);
    c->playbackMode = kwlGetSoundPlaybackModeInt(kwlGetAttributeValue(node, KWL_XML_SOUND_PLAYBACK_MODE));
    c->numWaveReferences = kwlGetChildCount(node, KWL_XML_AUDIO_DATA_REFERENCE_NODE);
    
    if (c->numWaveReferences > 0)
    {
        c->waveBankIndices = KWL_MALLOCANDZERO(c->numWaveReferences * sizeof(int), "xml 2 bin sound wb indices");
        c->audioDataIndices = KWL_MALLOCANDZERO(c->numWaveReferences * sizeof(int), "xml 2 bin sound ad indices");
    }
    
    int refIdx = 0;
    for (xmlNode* curr = node->children; curr != NULL; curr = curr->next)
    {
        if (xmlStrEqual(curr->name, (xmlChar*)KWL_XML_AUDIO_DATA_REFERENCE_NODE))
        {
            xmlChar* filePath = kwlGetAttributeValue(curr, KWL_XML_AUDIO_DATA_REFERENCE_PATH);
            xmlChar* waveBankId = kwlGetAttributeValue(curr, KWL_XML_AUDIO_DATA_REFERENCE_WAVEBANK);
            
            const int wbIdx = kwlGetWaveBankIndex(bin, waveBankId);
            c->waveBankIndices[refIdx] = wbIdx;
            
            
            if (wbIdx < 0)
            {
                const char* soundPath = kwlGetNodePath(node);
                *errorOccurred = 1;
                errorLogCallback("Invalid wave bank path '%s' found in sound definition '%s'.\n",
                                 waveBankId, soundPath);
                KWL_FREE(soundPath);
            }
            else
            {
                kwlWaveBankChunk* wb = &bin->waveBanksChunk.waveBanks[wbIdx];
                const int itemIdx = kwlGetAudioDataIndex(wb, filePath);
                c->audioDataIndices[refIdx] = itemIdx;
                const char* soundPath = kwlGetNodePath(node);
                if (itemIdx < 0)
                {
                    *errorOccurred = 1;
                    errorLogCallback("Invalid reference to '%s' in wave bank '%s' found in sound definition '%s'.\n",
                                     filePath, waveBankId, soundPath);
                    
                }
                else
                {
                    xmlNode* audioDataNode = kwlResolveAudioDataReference(node, wb->id, wb->audioDataEntries[itemIdx]);
                    KWL_ASSERT(audioDataNode != NULL);
                    int streamFromDisk = kwlGetBoolAttributeValue(audioDataNode, KWL_XML_AUDIO_DATA_STREAM);
                    if (streamFromDisk)
                    {
                        *errorOccurred = 1;
                        errorLogCallback("Sound definition '%s' references audio data item '%s' with stream flag set.\n",
                                         soundPath, wb->audioDataEntries[itemIdx]);
                    }
                }
                
                KWL_FREE(soundPath);
            }
            
            KWL_ASSERT(refIdx < bin->waveBanksChunk.numAudioDataItemsTotal);
            refIdx++;
        }
    }
    
    KWL_ASSERT(refIdx == c->numWaveReferences);
}

static void kwlCreateSoundChunk(xmlNode* projectRoot, kwlEngineDataBinary* bin, int* errorOccurred, kwlLogCallback errorLogCallback)
{
    bin->soundsChunk.chunkId = KWL_SOUNDS_CHUNK_ID;
    
    kwlTraverseNodeTree(projectRoot,
                        KWL_XML_SOUND_GROUP_NODE,
                        KWL_XML_SOUND_NODE,
                        kwlGatherSoundsCallback,
                        bin,
                        errorOccurred,
                        errorLogCallback);
}

static int kwlGetEventRetriggerModeInt(xmlChar* playbackMode)
{
    if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_RETRIGGER_MODE_RETRIGGER) == 0)
    {
        return 0;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_RETRIGGER_MODE_NO_RETRIGGER) == 0)
    {
        return 1;
    }
    
    KWL_ASSERT(0 && "invalid event retrigger mode");
    
    return -1;
}

static int kwlGetEventInstanceStealingModeInt(xmlChar* playbackMode)
{
    if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_INSTACE_STEALING_MODE_QUIETEST) == 0)
    {
        return 0;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_INSTACE_STEALING_MODE_RANDOM) == 0)
    {
        return 1;
    }
    else if (xmlStrcmp(playbackMode, (xmlChar*)KWL_XML_INSTACE_STEALING_MODE_NO_STEAL) == 0)
    {
        return 2;
    }
    
    KWL_ASSERT(0 && "invalid event instance stealing mode");
    
    return -1;
}

static void kwlGatherEventsCallback(xmlNode* node,
                                    void* b,
                                    int* errorOccurred,
                                    kwlLogCallback errorLogCallback)
{
    kwlEngineDataBinary* bin = (kwlEngineDataBinary*)b;
    bin->eventsChunk.numEventDefinitions += 1;
    bin->eventsChunk.eventDefinitions = KWL_REALLOC(bin->eventsChunk.eventDefinitions,
                                                    sizeof(kwlEventChunk) * bin->eventsChunk.numEventDefinitions,
                                                    "xml 2 bin event realloc");
    kwlEventChunk* c = &bin->eventsChunk.eventDefinitions[bin->eventsChunk.numEventDefinitions - 1];
    kwlMemset(c, 0, sizeof(kwlEventChunk));
    
    c->id = kwlGetNodePath(node);
    c->outerConeAngleDeg = kwlGetFloatAttributeValue(node, KWL_XML_EVENT_OUTER_ANGLE);
    c->outerConeGain = kwlGetFloatAttributeValue(node, KWL_XML_EVENT_OUTER_GAIN);
    c->innerConeAngleDeg = kwlGetFloatAttributeValue(node, KWL_XML_EVENT_INNER_ANGLE);
    c->gain = kwlGetFloatAttributeValue(node, KWL_XML_EVENT_GAIN);
    c->pitch = kwlGetFloatAttributeValue(node, KWL_XML_EVENT_PITCH);
    c->instanceCount = kwlGetFloatAttributeValue(node, KWL_XML_EVENT_INSTANCE_COUNT);
    c->isPositional = kwlGetBoolAttributeValue(node, KWL_XML_EVENT_IS_POSITIONAL);
    c->mixBusIndex = kwlGetMixBusIndex(bin, kwlGetAttributeValue(node, KWL_XML_EVENT_BUS));
    c->retriggerMode = kwlGetEventRetriggerModeInt(kwlGetAttributeValue(node, KWL_XML_EVENT_RETRIGGER_MODE));
    c->instanceStealingMode = kwlGetEventInstanceStealingModeInt(kwlGetAttributeValue(node, KWL_XML_EVENT_INSTANCE_STEALING_MODE));
    
    if (c->mixBusIndex < 0)
    {
        *errorOccurred = 1;
        errorLogCallback("The mix bus '%s' referenced by event definition '%s' does not exist\n",
                         (const char*)kwlGetAttributeValue(node, KWL_XML_EVENT_BUS),
                         c->id);
    }
    
    const int eventHasAudioRef = kwlGetChildCount(node, KWL_XML_SOUND_REFERENCE_NODE) == 0;
    
    c->soundIndex = -1;
    c->waveBankIndex = -1;
    c->audioDataIndex = -1;
    
    if (eventHasAudioRef)
    {
        xmlNode* audioDataRefNode = kwlGetChild(node, KWL_XML_AUDIO_DATA_REFERENCE_NODE);
        KWL_ASSERT(audioDataRefNode != NULL);
        xmlChar* wbPath = kwlGetAttributeValue(audioDataRefNode, KWL_XML_AUDIO_DATA_REFERENCE_WAVEBANK);
        char* audioDataPath = kwlGetAttributeValue(audioDataRefNode, KWL_XML_AUDIO_DATA_REFERENCE_PATH);
        const int loop = kwlGetBoolAttributeValue(audioDataRefNode, KWL_XML_AUDIO_DATA_REFERENCE_LOOP);
        
        
        c->waveBankIndex = kwlGetWaveBankIndex(bin, wbPath);
        c->audioDataIndex = -1;
        c->loopIfStreaming = loop;
        if (c->waveBankIndex < 0)
        {
            *errorOccurred = 1;
            errorLogCallback("Streaming event '%s' references non-existing wave bank '%s'.\n", c->id, wbPath);
        }
        else
        {
            KWL_ASSERT(c->waveBankIndex >= 0 && c->waveBankIndex < bin->waveBanksChunk.numWaveBanks);
            kwlWaveBankChunk* wb = &bin->waveBanksChunk.waveBanks[c->waveBankIndex];
            const int itemIdx = kwlGetAudioDataIndex(wb, audioDataPath);
            if (itemIdx < 0)
            {
                *errorOccurred = 1;
                errorLogCallback("Streaming event '%s' references non-existing audio data item '%s'.\n", c->id, audioDataPath);
            }
            else
            {
                c->audioDataIndex = kwlGetAudioDataIndex(wb, audioDataPath);
            }
        }
        
        c->numReferencedWaveBanks = 1;
        c->waveBankIndices = KWL_MALLOCANDZERO(c->numReferencedWaveBanks * sizeof(int), "bin streaming evt wb indices");
        c->waveBankIndices[0] = c->waveBankIndex;
    }
    else
    {
        xmlNode* soundRefNode = kwlGetChild(node, KWL_XML_SOUND_REFERENCE_NODE);
        KWL_ASSERT(soundRefNode != NULL);
        xmlChar* soundPath = kwlGetAttributeValue(soundRefNode, KWL_XML_SOUND_REFERENCE_SOUND);
        c->soundIndex = kwlGetSoundIndex(bin, soundPath);
        
        if (c->soundIndex < 0)
        {
            *errorOccurred = 1;
            errorLogCallback("Event '%s' references non-existing sound '%s'.\n", c->id, soundPath);
        }
        else
        {
            /*gather wavebanks indirectly referenced by this event through its sound definition*/
            c->numReferencedWaveBanks = 0;
            c->waveBankIndices = NULL;
            
            kwlSoundChunk* s = &bin->soundsChunk.soundDefinitions[c->soundIndex];
            
            for (int i = 0; i < s->numWaveReferences; i++)
            {
                const int wbIdx = s->waveBankIndices[i];
                
                int wbAlreadyInList = 0;
                for (int j = 0; j < c->numReferencedWaveBanks; j++)
                {
                    if (c->waveBankIndices[j] == wbIdx)
                    {
                        wbAlreadyInList = 1;
                        break;
                    }
                }
                
                if (wbAlreadyInList == 0)
                {
                    c->numReferencedWaveBanks++;
                    c->waveBankIndices = KWL_REALLOC(c->waveBankIndices,
                                                     c->numReferencedWaveBanks * sizeof(int),
                                                     "bin event ref wb list");
                    c->waveBankIndices[c->numReferencedWaveBanks - 1] = wbIdx;
                }
            }
        }
    }
}

static void kwlCreateEventChunk(xmlNode* eventsRootGroup,
                                kwlEngineDataBinary* bin,
                                int* errorOccurred,
                                kwlLogCallback errorLogCallback)
{
    bin->eventsChunk.chunkId = KWL_EVENTS_CHUNK_ID;
    
    kwlTraverseNodeTree(eventsRootGroup,
                        KWL_XML_EVENT_GROUP_NODE,
                        KWL_XML_EVENT_NODE,
                        kwlGatherEventsCallback,
                        bin,
                        errorOccurred,
                        errorLogCallback);
    
    /*we don't need the sound definition names anymore*/
    for (int i = 0; i < bin->soundsChunk.numSoundDefinitions; i++)
    {
        KWL_FREE(soundDefinitionNames[i]);
    }
    
    KWL_FREE(soundDefinitionNames);
    
    if (*errorOccurred)
    {
        return;
    }
    
    /*Finally, create sound definitions for events referencing
     non-streaming pcm data directly*/
    
    xmlNode* projNode = eventsRootGroup->parent;
    char* audioFileRoot = kwlGetAttributeValueCopy(projNode, KWL_XML_KOWALSKI_PROJECT_AUDIO_FILE_ROOT);
    const int rootIsRelative = kwlGetBoolAttributeValue(projNode, KWL_XML_KOWALSKI_PROJECT_AUDIO_FILE_ROOT_IS_RELATIVE);
    
    for (int i = 0; i < bin->eventsChunk.numEventDefinitions; i++)
    {
        kwlEventChunk* ei = &bin->eventsChunk.eventDefinitions[i];
        const int wbIdx = ei->waveBankIndex;
        const int adIdx = ei->audioDataIndex;
        if (ei->soundIndex < 0)
        {
            xmlNode* audioDataNode = kwlResolveAudioDataReference(eventsRootGroup,
                                                                  bin->waveBanksChunk.waveBanks[wbIdx].id,
                                                                  bin->waveBanksChunk.waveBanks[wbIdx].audioDataEntries[adIdx]);
            KWL_ASSERT(audioDataNode);
            char* relFilePath = kwlGetAttributeValueCopy(audioDataNode, KWL_XML_ATTR_REL_PATH);
            const int streaming = kwlGetBoolAttributeValue(audioDataNode, KWL_XML_AUDIO_DATA_STREAM);
            
            char* absFilePath = kwlGetAudioFilePath(projectXmlPath,
                                                    audioFileRoot,
                                                    rootIsRelative,
                                                    relFilePath);
            
            kwlAudioData ad;
            //printf("loadint audio file %s\n", absFilePath);
            kwlError e = kwlLoadAudioFile(absFilePath,
                                          &ad,
                                          KWL_SKIP_AUDIO_DATA);
            
            if (e == KWL_NO_ERROR &&
                kwlAudioData_isLinearPCM(&ad) &&
                !streaming)
            {
                /*Create a sound definition for this event*/
                //printf("Event '%s' references PCM audio file '%s'. Creating extra sound definition.\n", ei->id, absFilePath);
                bin->soundsChunk.numSoundDefinitions++;
                const int newSndIdx = bin->soundsChunk.numSoundDefinitions - 1;
                bin->soundsChunk.soundDefinitions = KWL_REALLOC(bin->soundsChunk.soundDefinitions,
                                                                bin->soundsChunk.numSoundDefinitions * sizeof(kwlSoundChunk),
                                                                "extra bin sounds realloc");
                kwlSoundChunk* s = &bin->soundsChunk.soundDefinitions[newSndIdx];
                kwlMemset(s, 0, sizeof(kwlSoundChunk));
                s->gain = 1.0f;
                s->gainVariation = 0.0f;
                s->pitch = 1.0;
                s->pitchVariation = 0.0f;
                s->numWaveReferences = 1;
                s->playbackMode = KWL_RANDOM;
                s->playbackCount = 1;
                s->audioDataIndices = KWL_MALLOCANDZERO(sizeof(int), "extra sound data idx");
                s->waveBankIndices = KWL_MALLOCANDZERO(sizeof(int), "extra sound wb idx");
                s->waveBankIndices[0] = ei->waveBankIndex;
                ei->waveBankIndex = -1;
                s->audioDataIndices[0] = ei->audioDataIndex;
                ei->audioDataIndex = -1;
                
                ei->soundIndex = newSndIdx;
            }
            
            kwlAudioData_free(&ad);
            
            KWL_FREE(relFilePath);
            KWL_FREE(absFilePath);
        }
    }
    
    KWL_FREE(audioFileRoot);
}

static void kwlCheckPathUniqueness(xmlNode* node,
                                   const char* branchNodeName,
                                   const char* leafNodeName,
                                   int* uniquenessErrorOccurred,
                                   kwlLogCallback errorLogCallback)
{
    const int childCount = kwlGetChildCount(node, branchNodeName) + kwlGetChildCount(node, leafNodeName);
    
    /*gather child id list*/
    const xmlChar** childNames = KWL_MALLOC(childCount * sizeof(char*), "path uniqueness check list");
    int childIdx = 0;
    for (xmlNode* curr = node->children; curr != NULL; curr = curr->next)
    {
        if (xmlStrEqual(curr->name, (xmlChar*)branchNodeName) ||
            xmlStrEqual(curr->name, (xmlChar*)leafNodeName))
        {
            childNames[childIdx] = kwlGetAttributeValue(curr, "id");
            KWL_ASSERT(childNames[childIdx] != NULL);
            childIdx++;
        }
    }
    
    KWL_ASSERT(childCount == childIdx);
    
    /*check id uniqueness*/
    for (int i = 0; i < childCount; i++)
    {
        int numMatches = 0;
        for (int j = 0; j < childCount; j++)
        {
            if (xmlStrEqual(childNames[i], childNames[j]))
            {
                numMatches++;
            }
        }
        
        KWL_ASSERT(numMatches > 0);
        
        if (numMatches > 1)
        {
            errorLogCallback("The id '%s' is not unique among the children of the %s node with id '%s'.\n",
                             childNames[i], branchNodeName, kwlGetAttributeValue(node, "id"));
            *uniquenessErrorOccurred = 1;
        }
    }
    
    KWL_FREE(childNames);
    
    /*traverse branch nodes*/
    for (xmlNode* curr = node->children; curr != NULL; curr = curr->next)
    {
        if (xmlStrEqual(curr->name, (xmlChar*)branchNodeName))
        {
            kwlCheckPathUniqueness(curr,
                                   branchNodeName,
                                   leafNodeName,
                                   uniquenessErrorOccurred,
                                   errorLogCallback);
        }
    }
}

kwlResultCode kwlEngineDataBinary_loadFromXMLDocument(kwlEngineDataBinary* bin,
                                                      const char* xmlPath,
                                                      xmlDocPtr doc,
                                                      kwlLogCallback errorLogCallback)
{
    projectXmlPath = xmlPath; //TODO: pass this along somehow
    
    /*Get the root element node */
    xmlNode* projectRootNode = xmlDocGetRootElement(doc);
    
    /*Process mix bus, mix presets, wave banks, sounds and events*/
    xmlNode* mixBusRootNode = NULL;
    xmlNode* mixPresetRootNode = NULL;
    xmlNode* waveBankRootNode = NULL;
    xmlNode* soundRootNode = NULL;
    xmlNode* eventRootNode = NULL;
    
    for (xmlNode* currNode = projectRootNode->children; currNode; currNode = currNode->next)
    {
        if (xmlStrcmp(currNode->name, (xmlChar*)KWL_XML_MIX_BUS_NODE) == 0)
        {
            mixBusRootNode = currNode;
        }
        else if (xmlStrcmp(currNode->name, (xmlChar*)KWL_XML_MIX_PRESET_GROUP_NODE) == 0)
        {
            mixPresetRootNode = currNode;
        }
        else if (xmlStrcmp(currNode->name, (xmlChar*)KWL_XML_WAVE_BANK_GROUP_NODE) == 0)
        {
            waveBankRootNode = currNode;
        }
        else if (xmlStrcmp(currNode->name, (xmlChar*)KWL_XML_SOUND_GROUP_NODE) == 0)
        {
            soundRootNode = currNode;
        }
        else if (xmlStrcmp(currNode->name, (xmlChar*)KWL_XML_EVENT_GROUP_NODE) == 0)
        {
            eventRootNode = currNode;
        }
    }
    
    KWL_ASSERT(projectRootNode != NULL);
    KWL_ASSERT(mixBusRootNode != NULL);
    KWL_ASSERT(mixPresetRootNode != NULL);
    KWL_ASSERT(waveBankRootNode != NULL);
    KWL_ASSERT(soundRootNode != NULL);
    KWL_ASSERT(eventRootNode != NULL);
    
    /*before starting to build the binary, check path uniqueness*/
    {
        int uniquenessErrorOccurred = 0;
        
        kwlCheckPathUniqueness(mixPresetRootNode,
                               KWL_XML_MIX_PRESET_GROUP_NODE,
                               KWL_XML_MIX_PRESET_NODE,
                               &uniquenessErrorOccurred,
                               errorLogCallback);
        
        kwlCheckPathUniqueness(waveBankRootNode,
                               KWL_XML_WAVE_BANK_GROUP_NODE,
                               KWL_XML_WAVE_BANK_NODE,
                               &uniquenessErrorOccurred,
                               errorLogCallback);
        
        kwlCheckPathUniqueness(soundRootNode,
                               KWL_XML_SOUND_GROUP_NODE,
                               KWL_XML_SOUND_NODE,
                               &uniquenessErrorOccurred,
                               errorLogCallback);
        
        kwlCheckPathUniqueness(eventRootNode,
                               KWL_XML_EVENT_GROUP_NODE,
                               KWL_XML_EVENT_NODE,
                               &uniquenessErrorOccurred,
                               errorLogCallback);
        
        if (uniquenessErrorOccurred != 0)
        {
            return KWL_PROJECT_XML_STRUCTURE_ERROR;
        }
    }
    
    kwlMemset(bin, 0, sizeof(kwlEngineDataBinary));
    /*first, write file identifier*/
    for (int i = 0; i < KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER_LENGTH; i++)
    {
        bin->fileIdentifier[i] = KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER[i];
    }
    
    /*then gather the different chunks*/
    soundDefinitionNames = NULL;
    
    int mixBusErrorOccurred = 0;
    kwlCreateMixBusChunk(projectRootNode, bin, &mixBusErrorOccurred, errorLogCallback);
    int mixPresetErrorOccurred = 0;
    kwlCreateMixPresetChunk(mixPresetRootNode, bin, &mixPresetErrorOccurred, errorLogCallback);
    int waveBankErrorOccurred = 0;
    kwlCreateWaveBankChunk(waveBankRootNode, bin, &waveBankErrorOccurred, errorLogCallback);
    int soundErrorOccurred = 0;
    kwlCreateSoundChunk(soundRootNode, bin, &soundErrorOccurred, errorLogCallback);
    int eventErrorOccurred = 0;
    kwlCreateEventChunk(eventRootNode, bin, &eventErrorOccurred, errorLogCallback);
    
    int audioDataReferenceErrorOccurred = 0;
    
    
    /*check if everything went well*/
    if (mixBusErrorOccurred ||
        mixPresetErrorOccurred ||
        waveBankErrorOccurred ||
        soundErrorOccurred ||
        eventErrorOccurred ||
        audioDataReferenceErrorOccurred)
    {
        kwlEngineDataBinary_free(bin);
        return KWL_PROJECT_XML_STRUCTURE_ERROR;
    }
    
    return KWL_SUCCESS;
}


kwlResultCode kwlEngineDataBinary_loadFromXMLFile(kwlEngineDataBinary* bin,
                                                  const char* xmlPath,
                                                  const char* xsdPath,
                                                  int validateAudioFileReferences,
                                                  kwlLogCallback errorLogCallback)

{
    kwlMemset(bin, 0, sizeof(kwlEngineDataBinary));
    xmlDocPtr doc;
    kwlResultCode validationResult = kwlLoadAndValidateProjectDataDoc(xmlPath, xsdPath, &doc, errorLogCallback);
    
    if (validationResult != KWL_SUCCESS)
    {
        return validationResult;
    }
    
    kwlResultCode loadingResult = kwlEngineDataBinary_loadFromXMLDocument(bin,
                                                                          xmlPath,
                                                                          doc,
                                                                          errorLogCallback);
    
    /*If requested, check that all audio data file references can be resolved.*/
    if (validateAudioFileReferences &&
        loadingResult == KWL_SUCCESS)
    {
        xmlNode* projNode = xmlDocGetRootElement(doc);
        char* audioFileRoot = kwlGetAttributeValueCopy(projNode, KWL_XML_KOWALSKI_PROJECT_AUDIO_FILE_ROOT);
        int rootIsRelative = kwlGetBoolAttributeValue(projNode, KWL_XML_KOWALSKI_PROJECT_AUDIO_FILE_ROOT_IS_RELATIVE);
        
        kwlResultCode r = kwlEngineDataBinary_validateFileReferences(bin,
                                                                     xmlPath,
                                                                     audioFileRoot,
                                                                     rootIsRelative,
                                                                     errorLogCallback);
        KWL_FREE(audioFileRoot);
        xmlFreeDoc(doc);
        xmlCleanupParser();
        return r;
    }
    else
    {
        xmlFreeDoc(doc);
        xmlCleanupParser();
        return loadingResult;
    }
    
}

kwlResultCode kwlEngineDataBinary_validateFileReferences(kwlEngineDataBinary* bin,
                                                         const char* xmlPath,
                                                         const char* audioFileRoot,
                                                         int rootIsRelative,
                                                         kwlLogCallback errorLogCallback)
{
    kwlResultCode result = KWL_SUCCESS;
    for (int i = 0; i < bin->waveBanksChunk.numWaveBanks; i++)
    {
        kwlWaveBankChunk* wbi = &bin->waveBanksChunk.waveBanks[i];
        for (int j = 0; j < wbi->numAudioDataEntries; j++)
        {
            char* relPathj = wbi->audioDataEntries[j];
            
            char* audioFilePath = kwlGetAudioFilePath(xmlPath, audioFileRoot, rootIsRelative, relPathj);
            if (!kwlDoesFileExist(audioFilePath))
            {
                errorLogCallback("The audio file '%s' referenced by wave bank '%s' does not exist.\n", audioFilePath, wbi->id);
                result = KWL_AUDIO_FILE_REFERENCE_ERROR;
            }
            KWL_FREE(audioFilePath);
        }
    }
    
    return result;
}

kwlResultCode kwlEngineDataBinary_writeToFile(kwlEngineDataBinary* bin,
                                              const char* path)
{
    kwlFileOutputStream fos;
    int success = kwlFileOutputStream_initWithPath(&fos, path);
    if (!success)
    {
        return KWL_COULD_NOT_OPEN_FILE_FOR_WRITING;
    }
    
    /*write file identifier*/
    kwlFileOutputStream_write(&fos, bin->fileIdentifier, KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER_LENGTH);
    
    long chunkStartPositions[5] = {0, 0, 0, 0, 0};
    long chunkEndPositions[5] = {0, 0, 0, 0, 0};
    
    /*write wave banks chunk*/
    {
        kwlWaveBanksChunk* wbc = &bin->waveBanksChunk;
        kwlFileOutputStream_writeInt32BE(&fos, KWL_WAVE_BANKS_CHUNK_ID);
        kwlFileOutputStream_writeInt32BE(&fos, 0); /*size*/
        chunkStartPositions[0] = ftell(fos.file);
        kwlFileOutputStream_writeInt32BE(&fos, wbc->numAudioDataItemsTotal);
        kwlFileOutputStream_writeInt32BE(&fos, wbc->numWaveBanks);
        
        for (int i = 0; i < wbc->numWaveBanks; i++)
        {
            kwlWaveBankChunk* wbi = &wbc->waveBanks[i];
            kwlFileOutputStream_writeASCIIString(&fos, wbi->id);
            kwlFileOutputStream_writeInt32BE(&fos, wbi->numAudioDataEntries);
            for (int j = 0; j < wbi->numAudioDataEntries; j++)
            {
                kwlFileOutputStream_writeASCIIString(&fos, wbi->audioDataEntries[j]);
            }
        }
        chunkEndPositions[0] = ftell(fos.file);
    }
    
    /*mix bus chunk*/
    {
        kwlMixBusesChunk* mbc = &bin->mixBusesChunk;
        kwlFileOutputStream_writeInt32BE(&fos, KWL_MIX_BUSES_CHUNK_ID);
        kwlFileOutputStream_writeInt32BE(&fos, 0); /*size*/
        chunkStartPositions[1] = ftell(fos.file);
        kwlFileOutputStream_writeInt32BE(&fos, mbc->numMixBuses);
        
        for (int i = 0; i < mbc->numMixBuses; i++)
        {
            kwlMixBusChunk* mbi = &mbc->mixBuses[i];
            kwlFileOutputStream_writeASCIIString(&fos, mbi->id);
            kwlFileOutputStream_writeInt32BE(&fos, mbi->numSubBuses);
            for (int j = 0; j < mbi->numSubBuses; j++)
            {
                kwlFileOutputStream_writeInt32BE(&fos, mbi->subBusIndices[j]);
            }
        }
        chunkEndPositions[1] = ftell(fos.file);
    }
    
    /*mix preset chunk*/
    {
        const int numMixBuses = bin->mixBusesChunk.numMixBuses;
        KWL_ASSERT(numMixBuses > 0);
        kwlMixPresetsChunk * mpc = &bin->mixPresetsChunk;
        kwlFileOutputStream_writeInt32BE(&fos, KWL_MIX_PRESETS_CHUNK_ID);
        kwlFileOutputStream_writeInt32BE(&fos, 0); /*size*/
        chunkStartPositions[2] = ftell(fos.file);
        kwlFileOutputStream_writeInt32BE(&fos, mpc->numMixPresets);
        
        for (int i = 0; i < mpc->numMixPresets; i++)
        {
            kwlMixPresetChunk* mpi = &mpc->mixPresets[i];
            kwlFileOutputStream_writeASCIIString(&fos, mpi->id);
            kwlFileOutputStream_writeInt32BE(&fos, mpi->isDefault);
            
            for (int j = 0; j < numMixBuses; j++)
            {
                kwlFileOutputStream_writeInt32BE(&fos, mpi->mixBusIndices[j]);
                kwlFileOutputStream_writeFloat32BE(&fos, mpi->gainLeft[j]);
                kwlFileOutputStream_writeFloat32BE(&fos, mpi->gainRight[j]);
                kwlFileOutputStream_writeFloat32BE(&fos, mpi->pitch[j]);
            }
        }
        chunkEndPositions[2] = ftell(fos.file);
    }
    
    /*sound chunk*/
    {
        kwlSoundsChunk * sdc = &bin->soundsChunk;
        kwlFileOutputStream_writeInt32BE(&fos, KWL_SOUNDS_CHUNK_ID);
        kwlFileOutputStream_writeInt32BE(&fos, 0); /*size*/
        chunkStartPositions[3] = ftell(fos.file);
        kwlFileOutputStream_writeInt32BE(&fos, sdc->numSoundDefinitions);
        
        for (int i = 0; i < sdc->numSoundDefinitions; i++)
        {
            kwlSoundChunk* si = &sdc->soundDefinitions[i];
            kwlFileOutputStream_writeInt32BE(&fos, si->playbackCount);
            kwlFileOutputStream_writeInt32BE(&fos, si->deferStop);
            kwlFileOutputStream_writeFloat32BE(&fos, si->gain);
            kwlFileOutputStream_writeFloat32BE(&fos, si->gainVariation);
            kwlFileOutputStream_writeFloat32BE(&fos, si->pitch);
            kwlFileOutputStream_writeFloat32BE(&fos, si->pitchVariation);
            kwlFileOutputStream_writeInt32BE(&fos, si->playbackMode);
            kwlFileOutputStream_writeInt32BE(&fos, si->numWaveReferences);
            
            for (int j = 0; j < si->numWaveReferences; j++)
            {
                kwlFileOutputStream_writeInt32BE(&fos, si->waveBankIndices[j]);
                kwlFileOutputStream_writeInt32BE(&fos, si->audioDataIndices[j]);
            }
        }
        chunkEndPositions[3] = ftell(fos.file);
    }
    
    /*event chunk*/
    {
        kwlEventsChunk * edc = &bin->eventsChunk;
        kwlFileOutputStream_writeInt32BE(&fos, KWL_EVENTS_CHUNK_ID);
        kwlFileOutputStream_writeInt32BE(&fos, 0); /*size*/
        chunkStartPositions[4] = ftell(fos.file);
        kwlFileOutputStream_writeInt32BE(&fos, edc->numEventDefinitions);
        
        for (int i = 0; i < edc->numEventDefinitions; i++)
        {
            kwlEventChunk* ei = &edc->eventDefinitions[i];
            kwlFileOutputStream_writeASCIIString(&fos, ei->id);
            kwlFileOutputStream_writeInt32BE(&fos, ei->instanceCount);
            kwlFileOutputStream_writeFloat32BE(&fos, ei->gain);
            kwlFileOutputStream_writeFloat32BE(&fos, ei->pitch);
            kwlFileOutputStream_writeFloat32BE(&fos, ei->innerConeAngleDeg);
            kwlFileOutputStream_writeFloat32BE(&fos, ei->outerConeAngleDeg);
            kwlFileOutputStream_writeFloat32BE(&fos, ei->outerConeGain);
            kwlFileOutputStream_writeInt32BE(&fos, ei->mixBusIndex);
            kwlFileOutputStream_writeInt32BE(&fos, ei->isPositional);
            kwlFileOutputStream_writeInt32BE(&fos, ei->soundIndex);
            kwlFileOutputStream_writeInt32BE(&fos, ei->retriggerMode);
            kwlFileOutputStream_writeInt32BE(&fos, ei->waveBankIndex);
            kwlFileOutputStream_writeInt32BE(&fos, ei->audioDataIndex);
            kwlFileOutputStream_writeInt32BE(&fos, ei->loopIfStreaming);
            kwlFileOutputStream_writeInt32BE(&fos, ei->numReferencedWaveBanks);
            for (int j = 0; j < ei->numReferencedWaveBanks; j++)
            {
                kwlFileOutputStream_writeInt32BE(&fos, ei->waveBankIndices[j]);
            }
        }
        
        chunkEndPositions[4] = ftell(fos.file);
    }
    
    /*finally write the chunk sizes*/
    for (int i = 0; i < 5; i++)
    {
        fseek(fos.file,
              chunkStartPositions[i] - 4,
              SEEK_SET);
        const long size = chunkEndPositions[i] - chunkStartPositions[i];
        kwlFileOutputStream_writeInt32BE(&fos, (int)size);
    }
    
    /*done*/
    kwlFileOutputStream_close(&fos);
    
    return KWL_SUCCESS;
}

kwlResultCode kwlEngineDataBinary_loadFromBinaryFile(kwlEngineDataBinary* binaryRep,
                                                     const char* binaryPath,
                                                     kwlLogCallback errorLogCallbackIn)
{
    
    kwlMemset(binaryRep, 0, sizeof(kwlEngineDataBinary));
    
    kwlLogCallback errorLogCallback = errorLogCallbackIn == NULL ? kwlSilentLogCallback : errorLogCallbackIn;
    
    kwlInputStream is;
    kwlError fileOpenResult = kwlInputStream_initWithFile(&is, binaryPath);
    if (fileOpenResult != KWL_NO_ERROR)
    {
        //error loading file
        errorLogCallback("Could not open engine data binary %s\n", binaryPath);
        return KWL_COULD_NOT_OPEN_ENGINE_DATA_BINARY_FILE;
    }
    
    /*check file identifier*/
    for (int i = 0; i < KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER_LENGTH; i++)
    {
        const char identifierChari = kwlInputStream_readChar(&is);
        binaryRep->fileIdentifier[i] = identifierChari;
        if (identifierChari != KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER[i])
        {
            //invalid file format
            kwlInputStream_close(&is);
            errorLogCallback("Invalid engine data binary file header in %s\n", binaryPath);
            return KWL_INVALID_FILE_IDENTIFIER;
        }
    }
    
    kwlResultCode result = KWL_SUCCESS;
    
    //mix buses
    {
        binaryRep->mixBusesChunk.chunkId = KWL_MIX_BUSES_CHUNK_ID;
        binaryRep->mixBusesChunk.chunkSize = kwlInputStream_seekToEngineDataChunk(&is, KWL_MIX_BUSES_CHUNK_ID);
        
        //allocate memory for the mix bus data
        binaryRep->mixBusesChunk.numMixBuses = kwlInputStream_readIntBE(&is);
        binaryRep->mixBusesChunk.mixBuses = KWL_MALLOCANDZERO(binaryRep->mixBusesChunk.numMixBuses * sizeof(kwlMixBusChunk),
                                                              "bin mix buses");
        
        //read mix bus data
        for (int i = 0; i < binaryRep->mixBusesChunk.numMixBuses; i++)
        {
            kwlMixBusChunk* mi = &binaryRep->mixBusesChunk.mixBuses[i];
            mi->id = kwlInputStream_readASCIIString(&is);
            mi->numSubBuses = kwlInputStream_readIntBE(&is);
            mi->subBusIndices = NULL;
            
            if (mi->numSubBuses > 0)
            {
                mi->subBusIndices = KWL_MALLOCANDZERO(mi->numSubBuses * sizeof(int), "bin sub bus list");
                for (int j = 0; j < mi->numSubBuses; j++)
                {
                    const int subBusIndexj = kwlInputStream_readIntBE(&is);
                    if(subBusIndexj < 0 || subBusIndexj >= binaryRep->mixBusesChunk.numMixBuses)
                    {
                        errorLogCallback("Mix bus %s sub bus index at %d is %d. Expected value in [0, %d]\n",
                                         mi->id, j, subBusIndexj, binaryRep->mixBusesChunk.numMixBuses - 1);
                        result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                        goto onDataError;
                    }
                    mi->subBusIndices[j] = subBusIndexj;
                }
            }
        }
    }
    
    
    //mix presets
    {
        binaryRep->mixPresetsChunk.chunkId = KWL_MIX_PRESETS_CHUNK_ID;
        binaryRep->mixPresetsChunk.chunkSize = kwlInputStream_seekToEngineDataChunk(&is, KWL_MIX_PRESETS_CHUNK_ID);
        
        //allocate memory for the mix preset data
        binaryRep->mixPresetsChunk.numMixPresets = kwlInputStream_readIntBE(&is);
        binaryRep->mixPresetsChunk.mixPresets = KWL_MALLOCANDZERO(binaryRep->mixPresetsChunk.numMixPresets * sizeof(kwlMixPresetChunk), "bin mix presets");
        
        const int numParameterSets = binaryRep->mixBusesChunk.numMixBuses;
        int defaultPresetIndex = -1;
        
        //read data
        for (int i = 0; i < binaryRep->mixPresetsChunk.numMixPresets; i++)
        {
            kwlMixPresetChunk* mpi = &binaryRep->mixPresetsChunk.mixPresets[i];
            
            mpi->id = kwlInputStream_readASCIIString(&is);
            mpi->isDefault = kwlInputStream_readIntBE(&is);
            
            if (mpi->isDefault != 0)
            {
                if(defaultPresetIndex != -1)
                {
                    errorLogCallback("Multiple default mix presets found\n");
                    result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                    goto onDataError;
                }
                defaultPresetIndex = i;
            }
            
            mpi->gainLeft = (float*)KWL_MALLOCANDZERO(sizeof(float) * numParameterSets, "bin mp gains l");
            mpi->gainRight = (float*)KWL_MALLOCANDZERO(sizeof(float) * numParameterSets, "bin mp gains r");
            mpi->pitch = (float*)KWL_MALLOCANDZERO(sizeof(float) * numParameterSets, "bin mp pitches");
            mpi->mixBusIndices = (int*)KWL_MALLOCANDZERO(sizeof(int) * numParameterSets, "bin mp indices");
            
            for (int j = 0; j < numParameterSets; j++)
            {
                mpi->mixBusIndices[j] = kwlInputStream_readIntBE(&is);
                if (mpi->mixBusIndices[j] < 0 ||  mpi->mixBusIndices[j] >= numParameterSets)
                {
                    errorLogCallback("Mix preset %s references out of bounds index %d (mix bus count is %d)\n",
                                     mpi->id, mpi->mixBusIndices[j],
                                     binaryRep->mixBusesChunk.numMixBuses);
                    result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                    goto onDataError;
                }
                mpi->gainLeft[j] = kwlInputStream_readFloatBE(&is);
                mpi->gainRight[j] = kwlInputStream_readFloatBE(&is);
                mpi->pitch[j] = kwlInputStream_readFloatBE(&is);
            }
        }
        
        if (defaultPresetIndex < 0)
        {
            errorLogCallback("No default mix preset found\n");
            result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
            goto onDataError;
        }
    }
    
    //wave bank data
    {
        binaryRep->waveBanksChunk.chunkId = KWL_WAVE_BANKS_CHUNK_ID;
        binaryRep->waveBanksChunk.chunkSize = kwlInputStream_seekToEngineDataChunk(&is, KWL_WAVE_BANKS_CHUNK_ID);
        
        /*deserialize wave bank structures*/
        binaryRep->waveBanksChunk.numAudioDataItemsTotal = kwlInputStream_readIntBE(&is);
        if (binaryRep->waveBanksChunk.numAudioDataItemsTotal < 0)
        {
            errorLogCallback("Expected at least one audio data entry, found %d\n",
                             binaryRep->waveBanksChunk.numAudioDataItemsTotal);
            result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
            goto onDataError;
        }
        
        binaryRep->waveBanksChunk.numWaveBanks = kwlInputStream_readIntBE(&is);
        if (binaryRep->waveBanksChunk.numWaveBanks < 0)
        {
            errorLogCallback("Expected at least one wave bank, found %d\n",
                             binaryRep->waveBanksChunk.numWaveBanks);
            result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
            goto onDataError;
        }
        
        binaryRep->waveBanksChunk.waveBanks = KWL_MALLOCANDZERO(binaryRep->waveBanksChunk.numWaveBanks * sizeof(kwlWaveBankChunk),
                                                                "bin wbs");
        
        int audioDataItemIdx = 0;
        for (int i = 0; i < binaryRep->waveBanksChunk.numWaveBanks; i++)
        {
            kwlWaveBankChunk* wbi = &binaryRep->waveBanksChunk.waveBanks[i];
            wbi->id = kwlInputStream_readASCIIString(&is);
            wbi->numAudioDataEntries = kwlInputStream_readIntBE(&is);
            if (wbi->numAudioDataEntries < 0)
            {
                errorLogCallback("Wave bank %s has %d audio data items. Expected at least 1.\n",
                                 wbi->id,
                                 wbi->numAudioDataEntries);
                result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                goto onDataError;
            }
            
            wbi->audioDataEntries = KWL_MALLOCANDZERO(wbi->numAudioDataEntries * sizeof(char*),
                                                      "bin wb audio data list");
            
            for (int j = 0; j < wbi->numAudioDataEntries; j++)
            {
                wbi->audioDataEntries[j] = kwlInputStream_readASCIIString(&is);
                audioDataItemIdx++;
            }
        }
    }
    
    //sound data
    {
        binaryRep->soundsChunk.chunkId = KWL_SOUNDS_CHUNK_ID;
        binaryRep->soundsChunk.chunkSize = kwlInputStream_seekToEngineDataChunk(&is, KWL_SOUNDS_CHUNK_ID);
        
        /*allocate memory for sound definitions*/
        binaryRep->soundsChunk.numSoundDefinitions = kwlInputStream_readIntBE(&is);
        binaryRep->soundsChunk.soundDefinitions = KWL_MALLOCANDZERO(binaryRep->soundsChunk.numSoundDefinitions * sizeof(kwlSoundChunk),
                                                                    "bin sound defs");
        
        /*read sound definitions*/
        for (int i = 0; i < binaryRep->soundsChunk.numSoundDefinitions; i++)
        {
            kwlSoundChunk* si = & binaryRep->soundsChunk.soundDefinitions[i];
            si->playbackCount = kwlInputStream_readIntBE(&is);
            si->deferStop = kwlInputStream_readIntBE(&is);
            si->gain = kwlInputStream_readFloatBE(&is);
            si->gainVariation = kwlInputStream_readFloatBE(&is);
            si->pitch = kwlInputStream_readFloatBE(&is);
            si->pitchVariation = kwlInputStream_readFloatBE(&is);
            si->playbackMode = (kwlSoundPlaybackMode)kwlInputStream_readIntBE(&is);
            
            si->numWaveReferences = kwlInputStream_readIntBE(&is);
            if (si->numWaveReferences < 0)
            {
                errorLogCallback("Sound with index %d has %d audio data items. Expected at least 1.\n", i, si->numWaveReferences);
                result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                goto onDataError;
            }
            
            si->waveBankIndices = KWL_MALLOCANDZERO(si->numWaveReferences * sizeof(int), "bin wb idcs");
            si->audioDataIndices = KWL_MALLOCANDZERO(si->numWaveReferences * sizeof(int), "bin ad idcs");
            
            for (int j = 0; j < si->numWaveReferences; j++)
            {
                si->waveBankIndices[j] = kwlInputStream_readIntBE(&is);
                si->audioDataIndices[j] = kwlInputStream_readIntBE(&is);
            }
        }
    }
    
    //event data
    {
        binaryRep->eventsChunk.chunkId = KWL_EVENTS_CHUNK_ID;
        binaryRep->eventsChunk.chunkSize = kwlInputStream_seekToEngineDataChunk(&is, KWL_EVENTS_CHUNK_ID);
        
        /*read the total number of event definitions*/
        binaryRep->eventsChunk.numEventDefinitions = kwlInputStream_readIntBE(&is);
        if (binaryRep->eventsChunk.numEventDefinitions <= 0)
        {
            errorLogCallback("Found %d event definitions. Expected at least 1.\n",
                             binaryRep->eventsChunk.numEventDefinitions);
            result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
            goto onDataError;
        }
        
        binaryRep->eventsChunk.eventDefinitions =
        KWL_MALLOCANDZERO(binaryRep->eventsChunk.numEventDefinitions * sizeof(kwlEventChunk),
                          "bin ev defs");
        
        for (int i = 0; i < binaryRep->eventsChunk.numEventDefinitions; i++)
        {
            kwlEventChunk* ei = &binaryRep->eventsChunk.eventDefinitions[i];
            
            /*read the id of this event definition*/
            ei->id = kwlInputStream_readASCIIString(&is);
            
            ei->instanceCount = kwlInputStream_readIntBE(&is);
            KWL_ASSERT(ei->instanceCount >= -1);
            
            ei->gain = kwlInputStream_readFloatBE(&is);
            ei->pitch = kwlInputStream_readFloatBE(&is);
            ei->innerConeAngleDeg = kwlInputStream_readFloatBE(&is);
            ei->outerConeAngleDeg = kwlInputStream_readFloatBE(&is);
            ei->outerConeGain = kwlInputStream_readFloatBE(&is);
            
            /*read the index of the mix bus that this event belongs to*/
            ei->mixBusIndex = kwlInputStream_readIntBE(&is);
            KWL_ASSERT(ei->mixBusIndex >= 0 && ei->mixBusIndex < binaryRep->mixBusesChunk.numMixBuses);
            ei->isPositional = kwlInputStream_readIntBE(&is);
            
            ei->soundIndex = kwlInputStream_readIntBE(&is);
            if (ei->soundIndex < -1) //-1 is a valid value and indicates a streaming event
            {
                errorLogCallback("Invalid sound index %d in event definition %s.\n", ei->soundIndex, ei->id);
                result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                goto onDataError;
            }
            
            ei->retriggerMode = kwlInputStream_readIntBE(&is);
            ei->waveBankIndex = kwlInputStream_readIntBE(&is);
            if (ei->waveBankIndex < -1) //-1 is a valid value and indicates a non-streaming event
            {
                errorLogCallback("Invalid wave bank index %d in event definition %s.\n", ei->waveBankIndex, ei->id);
                result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                goto onDataError;
            }
            ei->audioDataIndex = kwlInputStream_readIntBE(&is);
            if (ei->audioDataIndex < -1) //-1 is a valid value and indicates a non-streaming event
            {
                errorLogCallback("Invalid audio data index %d in event definition %s.\n", ei->audioDataIndex, ei->id);
                result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                goto onDataError;
            }
            
            ei->loopIfStreaming = kwlInputStream_readIntBE(&is);
            KWL_ASSERT(ei->loopIfStreaming == 0 || ei->loopIfStreaming == 1);
            
            /*read referenced wave banks*/
            ei->numReferencedWaveBanks = kwlInputStream_readIntBE(&is);
            KWL_ASSERT(ei->numReferencedWaveBanks >= 0 && ei->numReferencedWaveBanks <= binaryRep->waveBanksChunk.numWaveBanks);
            ei->waveBankIndices = (int*)KWL_MALLOCANDZERO(ei->numReferencedWaveBanks * sizeof(int),
                                                          "bin evt wave bank refs");
            
            for (int j = 0; j < ei->numReferencedWaveBanks; j++)
            {
                ei->waveBankIndices[j] = kwlInputStream_readIntBE(&is);
                if (ei->waveBankIndices[j] < 0 || ei->waveBankIndices[j] >= binaryRep->waveBanksChunk.numWaveBanks)
                {
                    errorLogCallback("Invalid wave bank index %d in event definition %s (wave bank count is %d).\n",
                                     ei->waveBankIndices[j],
                                     ei->id,
                                     binaryRep->waveBanksChunk.numWaveBanks);
                    result = KWL_ENGINE_DATA_STRUCTURE_ERROR;
                    goto onDataError;
                }
            }
        }
    }
    
    kwlInputStream_close(&is);
    return KWL_SUCCESS;
    
onDataError:
    kwlInputStream_close(&is);
    kwlEngineDataBinary_free(binaryRep);
    return result;
}

void kwlEngineDataBinary_free(kwlEngineDataBinary* bin)
{
    for (int i = 0; i < bin->mixBusesChunk.numMixBuses; i++)
    {
        kwlMixBusChunk* mbi = &bin->mixBusesChunk.mixBuses[i];
        KWL_FREE(mbi->id);
        if (mbi->numSubBuses > 0)
        {
            KWL_FREE(mbi->subBusIndices);
        }
    }
    
    KWL_FREE(bin->mixBusesChunk.mixBuses);
    
    for (int i = 0; i < bin->mixPresetsChunk.numMixPresets; i++)
    {
        kwlMixPresetChunk* mpi = &bin->mixPresetsChunk.mixPresets[i];
        KWL_FREE(mpi->id);
        KWL_FREE(mpi->gainLeft);
        KWL_FREE(mpi->gainRight);
        KWL_FREE(mpi->pitch);
        KWL_FREE(mpi->mixBusIndices);
    }
    
    KWL_FREE(bin->mixPresetsChunk.mixPresets);
    
    for (int i = 0; i < bin->waveBanksChunk.numWaveBanks; i++)
    {
        kwlWaveBankChunk* wbi = &bin->waveBanksChunk.waveBanks[i];
        KWL_FREE(wbi->id);
        for (int j = 0; j < wbi->numAudioDataEntries; j++)
        {
            KWL_FREE(wbi->audioDataEntries[j]);
        }
        KWL_FREE(wbi->audioDataEntries);
    }
    
    KWL_FREE(bin->waveBanksChunk.waveBanks);
    
    for (int i = 0; i < bin->soundsChunk.numSoundDefinitions; i++)
    {
        kwlSoundChunk* si = &bin->soundsChunk.soundDefinitions[i];
        if (si->numWaveReferences > 0)
        {
            KWL_FREE(si->waveBankIndices);
            KWL_FREE(si->audioDataIndices);
        }
    }
    
    KWL_FREE(bin->soundsChunk.soundDefinitions);
    
    for (int i = 0; i < bin->eventsChunk.numEventDefinitions; i++)
    {
        kwlEventChunk* ei = &bin->eventsChunk.eventDefinitions[i];
        KWL_FREE(ei->id);
        if (ei->numReferencedWaveBanks > 0)
        {
            KWL_FREE(ei->waveBankIndices);
        }
    }
    
    KWL_FREE(bin->eventsChunk.eventDefinitions);
    
    kwlMemset(bin, 0, sizeof(kwlEngineDataBinary));
}

void kwlEngineDataBinary_dump(kwlEngineDataBinary* bin, kwlLogCallback logCallback)
{
    logCallback("Kowalski project data binary (");
    logCallback("file ID: ");
    for (int i = 0; i < KWL_ENGINE_DATA_BINARY_FILE_IDENTIFIER_LENGTH; i++)
    {
        logCallback("%d ", (int)bin->fileIdentifier[i]);
    }
    logCallback(")\n");
    
    logCallback("    Mix bus chunk (ID %d, %d bytes, %d mix buses):\n",
                bin->mixBusesChunk.chunkId,
                bin->mixBusesChunk.chunkSize,
                bin->mixBusesChunk.numMixBuses);
    for (int i = 0; i < bin->mixBusesChunk.numMixBuses; i++)
    {
        kwlMixBusChunk* mbi = &bin->mixBusesChunk.mixBuses[i];
        logCallback("        '%s' (idx %d, %d sub buses", mbi->id, i, mbi->numSubBuses);
        for (int j = 0; j < mbi->numSubBuses; j++)
        {
            logCallback("%s%d%s", j == 0 ? ": " : "", mbi->subBusIndices[j], j < mbi->numSubBuses - 1 ? ", " : "");
        }
        logCallback(")\n");
    }
    
    logCallback("\n");
    logCallback("    Mix preset chunk (ID %d, %d bytes, %d mix presets):\n",
                bin->mixPresetsChunk.chunkId,
                bin->mixPresetsChunk.chunkSize,
                bin->mixPresetsChunk.numMixPresets);
    for (int i = 0; i < bin->mixPresetsChunk.numMixPresets; i++)
    {
        kwlMixPresetChunk* mpi = &bin->mixPresetsChunk.mixPresets[i];
        logCallback("        '%s' (default %d)\n", mpi->id, mpi->isDefault);
        for (int j = 0; j < bin->mixBusesChunk.numMixBuses; j++)
        {
            logCallback("            bus idx %d: gain left %f, gain right %f, pitch %f\n",
                        mpi->mixBusIndices[j],
                        mpi->gainLeft[j],
                        mpi->gainRight[j],
                        mpi->pitch[j]);
        }
    }
    
    logCallback("\n");
    logCallback("    Wave bank chunk (ID %d, %d bytes, %d items total, %d wave banks):\n",
                bin->waveBanksChunk.chunkId,
                bin->waveBanksChunk.chunkSize,
                bin->waveBanksChunk.numAudioDataItemsTotal,
                bin->waveBanksChunk.numWaveBanks);
    for (int i = 0; i < bin->waveBanksChunk.numWaveBanks; i++)
    {
        kwlWaveBankChunk* wbi = &bin->waveBanksChunk.waveBanks[i];
        logCallback("        '%s' (%d entries)\n", wbi->id, wbi->numAudioDataEntries);
        for (int j = 0; j < wbi->numAudioDataEntries; j++)
        {
            logCallback("            %s\n", wbi->audioDataEntries[j]);
        }
    }
    
    logCallback("\n");
    logCallback("    Sound chunk (ID %d, %d bytes, %d sound definitions):\n",
                bin->soundsChunk.chunkId,
                bin->soundsChunk.chunkSize,
                bin->soundsChunk.numSoundDefinitions);
    for (int i = 0; i < bin->soundsChunk.numSoundDefinitions; i++)
    {
        kwlSoundChunk* si = &bin->soundsChunk.soundDefinitions[i];
        logCallback("        %d%s: defer stop %d, playback count %d, playback mode %d, num wave refs %d\n",
                    i, i < 10 ? " " : "", si->deferStop, si->playbackCount, si->playbackMode, si->numWaveReferences);
        logCallback("            gain (var) %f (%f), pitch (var) %f (%f)\n",
                    si->gain, si->gainVariation, si->pitch, si->pitchVariation);
        for (int j = 0; j < si->numWaveReferences; j++)
        {
            logCallback("                wave bank idx %d, item idx %d\n",
                        si->waveBankIndices[j],
                        si->audioDataIndices[j]);
        }
    }
    
    logCallback("\n");
    logCallback("    Event chunk (ID %d, %d bytes, %d event definitions):\n",
                bin->eventsChunk.chunkId,
                bin->eventsChunk.chunkSize,
                bin->eventsChunk.numEventDefinitions);
    for (int i = 0; i < bin->eventsChunk.numEventDefinitions; i++)
    {
        kwlEventChunk* ei = &bin->eventsChunk.eventDefinitions[i];
        logCallback("        %d: '%s'\n", i, ei->id);
        logCallback("                gain %f, pitch %f, \n", ei->gain, ei->pitch);
        logCallback("                inner cone angle %f, outer cone angle %f, outer cone gain %f\n",
                    ei->innerConeAngleDeg, ei->outerConeAngleDeg, ei->outerConeGain);
        logCallback("                instance count %d, is positional %d, stealing mode %d, retrigger mode %d\n",
                    ei->instanceCount, ei->isPositional, ei->instanceStealingMode, ei->retriggerMode);
        logCallback("                audio data index %d, wave bank index %d, loop %d (streaming events only)\n", ei->audioDataIndex, ei->waveBankIndex, ei->loopIfStreaming);
        logCallback("                sound index %d (non-streaming events only)\n", ei->soundIndex);
        logCallback("                %d referenced wave bank(s):\n", ei->numReferencedWaveBanks);
        for (int j = 0; j < ei->numReferencedWaveBanks; j++)
        {
            logCallback("                    idx %d\n", ei->waveBankIndices[j]);
        }
    }
}
