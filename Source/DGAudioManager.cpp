////////////////////////////////////////////////////////////
//
// DAGON - An Adventure Game Engine
// Copyright (c) 2011-2013 Senscape s.r.l.
// All rights reserved.
//
// This Source Code Form is subject to the terms of the
// Mozilla Public License, v. 2.0. If a copy of the MPL was
// not distributed with this file, You can obtain one at
// http://mozilla.org/MPL/2.0/.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////

#include "DGAudioManager.h"
#include "DGConfig.h"
#include "DGLog.h"

using namespace std;

////////////////////////////////////////////////////////////
// Implementation - Constructor
////////////////////////////////////////////////////////////

DGAudioManager::DGAudioManager() {
    log = &DGLog::getInstance();
    config = &DGConfig::getInstance();

    _isInitialized = false;
	_isRunning = false;
}

////////////////////////////////////////////////////////////
// Implementation - Destructor
////////////////////////////////////////////////////////////

DGAudioManager::~DGAudioManager() {
    // FIXME: Here it's important to determine if the
    // audio was created by Lua or by another class
    
    // Each audio object should unregister itself if
    // destroyed
    _isRunning = false;

    _audioThread.join();
    
    if (!_arrayOfAudios.empty()) {
        vector<DGAudio*>::iterator it;
        
        _mutexForArray.lock();
        
        it = _arrayOfAudios.begin();
        while (it != _arrayOfAudios.end()) {
            // FIXME: Should delete here and let the audio object unload itself
			(*it)->unload();
            it++;
        }
        _mutexForArray.unlock();
    }
    
    // Now we shut down OpenAL completely
    if (_isInitialized) {
        alcMakeContextCurrent(NULL);
        alcDestroyContext(_alContext);
        alcCloseDevice(_alDevice);
    }
}

////////////////////////////////////////////////////////////
// Implementation
////////////////////////////////////////////////////////////

void DGAudioManager::clear() {
    if (_isInitialized) {
        if (!_arrayOfActiveAudios.empty()) {
            vector<DGAudio*>::iterator it;
            
            _mutexForArray.lock();
            it = _arrayOfActiveAudios.begin();
            
            while (it != _arrayOfActiveAudios.end()) {
                (*it)->release();
                
                it++;
            }
            _mutexForArray.unlock();
        }
    }
}

void DGAudioManager::flush() {
	bool done = false;

    if (_isInitialized) {
        if (!_arrayOfActiveAudios.empty()) {
            vector<DGAudio*>::iterator it;
            
			while (!done) {
                _mutexForArray.lock();
				it = _arrayOfActiveAudios.begin();
				done = true;
				while ((it != _arrayOfActiveAudios.end())) {
					if ((*it)->retainCount() == 0) {
						if ((*it)->state() == DGAudioStopped) { // Had to move this outside the switch
							// TODO: Automatically flush stopped and non-retained audios after
							// n update cycles
							(*it)->unload();
							_arrayOfActiveAudios.erase(it);
							done = false;
							break;
						}
						else {
							switch ((*it)->state()) {
								case DGAudioPlaying:
									(*it)->fadeOut();
									it++;
									break;
								case DGAudioPaused:
								default:
									it++;
									break;
							}
						}
					}
					else it++;
				}
                _mutexForArray.unlock();
			}
        }
    }
}

void DGAudioManager::init() {
    log->trace(DGModAudio, "%s", DGMsg070000);
    
    char deviceName[256];
	char *defaultDevice;
	char *deviceList;
	char *devices[12];
	int	numDevices, numDefaultDevice;
    
	strcpy(deviceName, "");
	if (config->debugMode) {
        if (alcIsExtensionPresent(NULL, (ALCchar*)"ALC_ENUMERATION_EXT") == AL_TRUE) { // Check if enumeration extension is present
            defaultDevice = (char *)alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);
            deviceList = (char *)alcGetString(NULL, ALC_DEVICE_SPECIFIER);
            for (numDevices = 0; numDevices < 12; numDevices++) {devices[numDevices] = NULL;}
            for (numDevices = 0; numDevices < 12; numDevices++) {
                devices[numDevices] = deviceList;
                if (strcmp(devices[numDevices], defaultDevice) == 0) {
                    numDefaultDevice = numDevices;
                }
                deviceList += strlen(deviceList);
                if (deviceList[0] == 0) {
                    if (deviceList[1] == 0) {
                        break;
                    } else {
                        deviceList += 1;
                    }
                }
            }
            
            if (devices[numDevices] != NULL) {
                int i;
                
                numDevices++;
                /*log->trace(DGModAudio, "%s", DGMsg080002);
                log->trace(DGModAudio, "0. NULL");
                for (i = 0; i < numDevices; i++) {
                    log->trace(DGModAudio, "%d. %s", i + 1, devices[i]);
                }*/
                
                i = config->audioDevice;
                if ((i != 0) && (strlen(devices[i - 1]) < 256)) {
                    strcpy(deviceName, devices[i - 1]);
                }
            }
        }
	}
    
    if (strlen(deviceName) == 0) {
		log->trace(DGModAudio, "%s", DGMsg080003);
		_alDevice = alcOpenDevice(NULL); // Select the preferred device
	} else {
		log->trace(DGModAudio, "%s: %s", DGMsg080004, deviceName);
		_alDevice = alcOpenDevice((ALCchar*)deviceName); // Use the name from the enumeration process
	}
    
    if (!_alDevice) {
        log->error(DGModAudio, "%s", DGMsg270001);
        return;
    }
    
	_alContext = alcCreateContext(_alDevice, NULL);
    
    if (!_alContext) {
        log->error(DGModAudio, "%s", DGMsg270002);
        
        return;
    }
    
	alcMakeContextCurrent(_alContext);
    
    ALfloat listenerPos[] = {0.0, 0.0, 0.0};
	ALfloat listenerVel[] = {0.0, 0.0, 0.0};
	ALfloat listenerOri[] = {0.0, 0.0, -1.0, 
                             0.0, 1.0, 0.0}; // Listener facing into the screen
    
	alListenerfv(AL_POSITION, listenerPos);
	alListenerfv(AL_VELOCITY, listenerVel);
	alListenerfv(AL_ORIENTATION, listenerOri);
    
    ALint error = alGetError();
    
	if (error != AL_NO_ERROR) {
		log->error(DGModAudio, "%s: init (%d)", DGMsg270003, error);
		return;
	}
    
    log->info(DGModAudio, "%s: %s", DGMsg070001, alGetString(AL_VERSION));
    log->info(DGModAudio, "%s: %s", DGMsg070002, vorbis_version_string());
    
    _isInitialized = true;
	_isRunning = true;
    
    _audioThread = thread([](){
        chrono::milliseconds dura(1);
        while (DGAudioManager::instance().update()) {
            this_thread::sleep_for(dura);
        }
    });
}

void DGAudioManager::registerAudio(DGAudio* target) {
    _arrayOfAudios.push_back(target);
}

void DGAudioManager::requestAudio(DGAudio* target) {
    if (!target->isLoaded()) {
        target->load();
    }

    target->retain();
    
    // If the audio is not active, then it's added to
    // that vector
    
    bool isActive = false;
    std::vector<DGAudio*>::iterator it;
    it = _arrayOfActiveAudios.begin();
    
    while (it != _arrayOfActiveAudios.end()) {
        if ((*it) == target) {
            isActive = true;
            break;
        }
        
        it++;
    }
    
    if (!isActive) {
        _mutexForArray.lock();
        _arrayOfActiveAudios.push_back(target);
        _mutexForArray.unlock();
    }
    
    // FIXME: Not very elegant. Must implement a state condition for
    // each audio object. Perhaps use AL_STATE even.
    if (target->state() == DGAudioPaused) {
        target->play();
    }
}

void DGAudioManager::setOrientation(float* orientation) {
    if (_isInitialized) {
        alListenerfv(AL_ORIENTATION, orientation);
    }
}

void DGAudioManager::terminate() {
	_isRunning = false;
}

// Asynchronous method
bool DGAudioManager::update() {
    if (_isRunning) {
        if (!_arrayOfActiveAudios.empty()) {
            vector<DGAudio*>::iterator it;
            
            _mutexForArray.lock();
            it = _arrayOfActiveAudios.begin();
            
            while (it != _arrayOfActiveAudios.end()) {
                (*it)->update();
                it++;
            }
            _mutexForArray.unlock();
        }

		return true;
    }

	return false;
}