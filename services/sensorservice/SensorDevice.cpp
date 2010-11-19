/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <math.h>
#include <sys/types.h>

#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Singleton.h>

#include <binder/BinderService.h>
#include <binder/Parcel.h>
#include <binder/IServiceManager.h>

#include <hardware/sensors.h>

#include "SensorDevice.h"

namespace android {
// ---------------------------------------------------------------------------
class BatteryService : public Singleton<BatteryService> {
    static const int TRANSACTION_noteStartSensor = IBinder::FIRST_CALL_TRANSACTION + 3;
    static const int TRANSACTION_noteStopSensor = IBinder::FIRST_CALL_TRANSACTION + 4;
    static const String16 DESCRIPTOR;

    friend class Singleton<BatteryService>;
    sp<IBinder> mBatteryStatService;

    BatteryService() {
        const sp<IServiceManager> sm(defaultServiceManager());
        if (sm != NULL) {
            const String16 name("batteryinfo");
            mBatteryStatService = sm->getService(name);
        }
    }

    status_t noteStartSensor(int uid, int handle) {
        Parcel data, reply;
        data.writeInterfaceToken(DESCRIPTOR);
        data.writeInt32(uid);
        data.writeInt32(handle);
        status_t err = mBatteryStatService->transact(
                TRANSACTION_noteStartSensor, data, &reply, 0);
        err = reply.readExceptionCode();
        return err;
    }

    status_t noteStopSensor(int uid, int handle) {
        Parcel data, reply;
        data.writeInterfaceToken(DESCRIPTOR);
        data.writeInt32(uid);
        data.writeInt32(handle);
        status_t err = mBatteryStatService->transact(
                TRANSACTION_noteStopSensor, data, &reply, 0);
        err = reply.readExceptionCode();
        return err;
    }

public:
    void enableSensor(int handle) {
        if (mBatteryStatService != 0) {
            int uid = IPCThreadState::self()->getCallingUid();
            int64_t identity = IPCThreadState::self()->clearCallingIdentity();
            noteStartSensor(uid, handle);
            IPCThreadState::self()->restoreCallingIdentity(identity);
        }
    }
    void disableSensor(int handle) {
        if (mBatteryStatService != 0) {
            int uid = IPCThreadState::self()->getCallingUid();
            int64_t identity = IPCThreadState::self()->clearCallingIdentity();
            noteStopSensor(uid, handle);
            IPCThreadState::self()->restoreCallingIdentity(identity);
        }
    }
};

const String16 BatteryService::DESCRIPTOR("com.android.internal.app.IBatteryStats");

ANDROID_SINGLETON_STATIC_INSTANCE(BatteryService)

// ---------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE(SensorDevice)

SensorDevice::SensorDevice()
    :  mSensorDevice(0),
       mSensorModule(0)
{
    status_t err = hw_get_module(SENSORS_HARDWARE_MODULE_ID,
            (hw_module_t const**)&mSensorModule);

    LOGE_IF(err, "couldn't load %s module (%s)",
            SENSORS_HARDWARE_MODULE_ID, strerror(-err));

    if (mSensorModule) {
        err = sensors_open(&mSensorModule->common, &mSensorDevice);

        LOGE_IF(err, "couldn't open device for module %s (%s)",
                SENSORS_HARDWARE_MODULE_ID, strerror(-err));

        if (mSensorDevice) {
            sensor_t const* list;
            ssize_t count = mSensorModule->get_sensors_list(mSensorModule, &list);
            mActivationCount.setCapacity(count);
            Info model;
            for (size_t i=0 ; i<size_t(count) ; i++) {
                mActivationCount.add(list[i].handle, model);
                mSensorDevice->activate(mSensorDevice, list[i].handle, 0);
            }
        }
    }
}

void SensorDevice::dump(String8& result, char* buffer, size_t SIZE)
{
    if (!mSensorModule) return;
    sensor_t const* list;
    ssize_t count = mSensorModule->get_sensors_list(mSensorModule, &list);

    snprintf(buffer, SIZE, "%d h/w sensors:\n", int(count));
    result.append(buffer);

    Mutex::Autolock _l(mLock);
    for (size_t i=0 ; i<size_t(count) ; i++) {
        snprintf(buffer, SIZE, "handle=0x%08x, active-count=%d / %d\n",
                list[i].handle,
                mActivationCount.valueFor(list[i].handle).count,
                mActivationCount.valueFor(list[i].handle).rates.size());
        result.append(buffer);
    }
}

ssize_t SensorDevice::getSensorList(sensor_t const** list) {
    if (!mSensorModule) return NO_INIT;
    ssize_t count = mSensorModule->get_sensors_list(mSensorModule, list);
    return count;
}

status_t SensorDevice::initCheck() const {
    return mSensorDevice && mSensorModule ? NO_ERROR : NO_INIT;
}

ssize_t SensorDevice::poll(sensors_event_t* buffer, size_t count) {
    if (!mSensorDevice) return NO_INIT;
    return mSensorDevice->poll(mSensorDevice, buffer, count);
}

status_t SensorDevice::activate(void* ident, int handle, int enabled)
{
    if (!mSensorDevice) return NO_INIT;
    status_t err(NO_ERROR);
    bool actuateHardware = false;

    Info& info( mActivationCount.editValueFor(handle) );
    int32_t& count(info.count);
    if (enabled) {
        if (android_atomic_inc(&count) == 0) {
            actuateHardware = true;
        }
        Mutex::Autolock _l(mLock);
        if (info.rates.indexOfKey(ident) < 0) {
            info.rates.add(ident, DEFAULT_EVENTS_PERIOD);
        }
    } else {
        if (android_atomic_dec(&count) == 1) {
            actuateHardware = true;
        }
        Mutex::Autolock _l(mLock);
        info.rates.removeItem(ident);
    }
    if (actuateHardware) {
        err = mSensorDevice->activate(mSensorDevice, handle, enabled);
        if (enabled) {
            LOGE_IF(err, "Error activating sensor %d (%s)", handle, strerror(-err));
            if (err == 0) {
                BatteryService::getInstance().enableSensor(handle);
            }
        } else {
            if (err == 0) {
                BatteryService::getInstance().disableSensor(handle);
            }
        }
    }

    if (!actuateHardware || enabled) {
        Mutex::Autolock _l(mLock);
        nsecs_t ns = info.rates.valueAt(0);
        for (size_t i=1 ; i<info.rates.size() ; i++) {
            if (info.rates.valueAt(i) < ns) {
                nsecs_t cur = info.rates.valueAt(i);
                if (cur < ns) {
                    ns = cur;
                }
            }
        }
        mSensorDevice->setDelay(mSensorDevice, handle, ns);
    }

    return err;
}

status_t SensorDevice::setDelay(void* ident, int handle, int64_t ns)
{
    if (!mSensorDevice) return NO_INIT;
    Info& info( mActivationCount.editValueFor(handle) );
    { // scope for lock
        Mutex::Autolock _l(mLock);
        ssize_t index = info.rates.indexOfKey(ident);
        if (index < 0) return BAD_INDEX;
        info.rates.editValueAt(index) = ns;
        ns = info.rates.valueAt(0);
        for (size_t i=1 ; i<info.rates.size() ; i++) {
            nsecs_t cur = info.rates.valueAt(i);
            if (cur < ns) {
                ns = cur;
            }
        }
    }
    return mSensorDevice->setDelay(mSensorDevice, handle, ns);
}

// ---------------------------------------------------------------------------
}; // namespace android

