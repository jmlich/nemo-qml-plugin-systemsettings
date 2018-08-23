/*
 * Copyright (C) 2018 Jolla Ltd. <raine.makelainen@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "udisks2monitor_p.h"
#include "udisks2block_p.h"
#include "udisks2job_p.h"
#include "udisks2defines.h"
#include "nemo-dbus/dbus.h"

#include "partitionmanager_p.h"
#include "logging_p.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QRegularExpression>

struct ErrorEntry {
    Partition::Error errorCode;
    const char *dbusErrorName;
};

// These are "copied" error from udiskserror.c so that we do not link against it.
static const ErrorEntry dbus_error_entries[] =
{
    { Partition::ErrorFailed,                 "org.freedesktop.UDisks2.Error.Failed" },
    { Partition::ErrorCancelled,              "org.freedesktop.UDisks2.Error.Cancelled" },
    { Partition::ErrorAlreadyCancelled,       "org.freedesktop.UDisks2.Error.AlreadyCancelled" },
    { Partition::ErrorNotAuthorized,          "org.freedesktop.UDisks2.Error.NotAuthorized" },
    { Partition::ErrorNotAuthorizedCanObtain, "org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain" },
    { Partition::ErrorNotAuthorizedDismissed, "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed" },
    { Partition::ErrorAlreadyMounted,         UDISKS2_ERROR_ALREADY_MOUNTED },
    { Partition::ErrorNotMounted,             "org.freedesktop.UDisks2.Error.NotMounted" },
    { Partition::ErrorOptionNotPermitted,     "org.freedesktop.UDisks2.Error.OptionNotPermitted" },
    { Partition::ErrorMountedByOtherUser,     "org.freedesktop.UDisks2.Error.MountedByOtherUser" },
    { Partition::ErrorAlreadyUnmounting,      UDISKS2_ERROR_ALREADY_UNMOUNTING },
    { Partition::ErrorNotSupported,           "org.freedesktop.UDisks2.Error.NotSupported" },
    { Partition::ErrorTimedout,               "org.freedesktop.UDisks2.Error.Timedout" },
    { Partition::ErrorWouldWakeup,            "org.freedesktop.UDisks2.Error.WouldWakeup" },
    { Partition::ErrorDeviceBusy,             "org.freedesktop.UDisks2.Error.DeviceBusy" }
};

UDisks2::Monitor *UDisks2::Monitor::sharedInstance = nullptr;

UDisks2::Monitor *UDisks2::Monitor::instance()
{
    Q_ASSERT(!sharedInstance);

    return sharedInstance;
}

UDisks2::Monitor::Monitor(PartitionManagerPrivate *manager, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
    Q_ASSERT(!sharedInstance);
    sharedInstance = this;

    qDBusRegisterMetaType<InterfaceAndPropertyMap>();
    QDBusConnection systemBus = QDBusConnection::systemBus();

    connect(systemBus.interface(), &QDBusConnectionInterface::callWithCallbackFailed, this, [this](const QDBusError &error, const QDBusMessage &call) {
        qCInfo(lcMemoryCardLog) << "====================================================";
        qCInfo(lcMemoryCardLog) << "DBus call with callback failed:" << error.message();
        qCInfo(lcMemoryCardLog) << "Name:" << error.name();
        qCInfo(lcMemoryCardLog) << "Error name" << call.errorName();
        qCInfo(lcMemoryCardLog) << "Error message:" << call.errorMessage();
        qCInfo(lcMemoryCardLog) << "Call interface:" << call.interface();
        qCInfo(lcMemoryCardLog) << "Call path:" << call.path();
        qCInfo(lcMemoryCardLog) << "====================================================";
        emit errorMessage(call.path(), error.name());
    });

    if (!systemBus.connect(
                UDISKS2_SERVICE,
                UDISKS2_PATH,
                DBUS_OBJECT_MANAGER_INTERFACE,
                QStringLiteral("InterfacesAdded"),
                this,
                SLOT(interfacesAdded(QDBusObjectPath, InterfaceAndPropertyMap)))) {
        qCWarning(lcMemoryCardLog) << "Failed to connect to interfaces added signal:" << qPrintable(systemBus.lastError().message());
    }

    if (!systemBus.connect(
                UDISKS2_SERVICE,
                UDISKS2_PATH,
                DBUS_OBJECT_MANAGER_INTERFACE,
                QStringLiteral("InterfacesRemoved"),
                this,
                SLOT(interfacesRemoved(QDBusObjectPath, QStringList)))) {
        qCWarning(lcMemoryCardLog) << "Failed to connect to interfaces added signal:" << qPrintable(systemBus.lastError().message());
    }
}

UDisks2::Monitor::~Monitor()
{
    sharedInstance = nullptr;
    qDeleteAll(m_jobsToWait);
    m_jobsToWait.clear();
}

void UDisks2::Monitor::mount(const QString &deviceName)
{
    QVariantHash arguments;
    arguments.insert(QString("fstype"), QString());
    startMountOperation(UDISKS2_FILESYSTEM_MOUNT, deviceName, arguments);
}

void UDisks2::Monitor::unmount(const QString &deviceName)
{
    QVariantHash arguments;
    arguments.insert(QString(), QString());
    startMountOperation(UDISKS2_FILESYSTEM_UNMOUNT, deviceName, arguments);
}

void UDisks2::Monitor::format(const QString &deviceName, const QString &type, const QString &label)
{
    if (deviceName.isEmpty()) {
        qCCritical(lcMemoryCardLog) << "Cannot format without device name";
        return;
    }

    QStringList fsList = m_manager->supportedFileSystems();
    if (!fsList.contains(type)) {
        qCWarning(lcMemoryCardLog) << "Can only format" << fsList.join(", ") << "filesystems.";
        return;
    }

    QVariantHash arguments;
    arguments.insert(QStringLiteral("label"), QString(label));
    arguments.insert(QStringLiteral("no-block"), true);
    arguments.insert(QStringLiteral("update-partition-type"), true);

    PartitionManagerPrivate::Partitions affectedPartions;
    lookupPartitions(affectedPartions, QStringList() << UDISKS2_BLOCK_DEVICE_PATH.arg(deviceName));

    for (auto partition : affectedPartions) {
        if (partition->status == Partition::Mounted) {
            m_operationQueue.enqueue(Operation(QString("format"), deviceName, type, arguments));
            unmount(deviceName);
            return;
        }
    }

    doFormat(deviceName, type, arguments);
}

void UDisks2::Monitor::interfacesAdded(const QDBusObjectPath &objectPath, const InterfaceAndPropertyMap &interfaces)
{
    qCInfo(lcMemoryCardLog) << "Interface added:" << objectPath.path() << interfaces;
    QString path = objectPath.path();
    if ((interfaces.contains(UDISKS2_PARTITION_INTERFACE) || interfaces.contains(UDISKS2_FILESYSTEM_INTERFACE)) && externalBlockDevice(path)) {
        m_manager->refresh();
        QVariantMap dict = interfaces.value(UDISKS2_BLOCK_INTERFACE);
        addBlockDevice(path, dict);
    } else if (path.startsWith(QStringLiteral("/org/freedesktop/UDisks2/jobs"))) {
        QVariantMap dict = interfaces.value(UDISKS2_JOB_INTERFACE);
        QString operation = dict.value(UDISKS2_JOB_KEY_OPERATION, QString()).toString();
        if (operation == UDISKS2_JOB_OP_FS_MOUNT ||
                operation == UDISKS2_JOB_OP_FS_UNMOUNT ||
                operation == UDISKS2_JOB_OP_CLEANUP ||
                operation == UDISKS2_JOB_OF_FS_FORMAT) {
            UDisks2::Job *job = new UDisks2::Job(objectPath.path(), dict);
            updatePartitionStatus(job, true);

            connect(job, &UDisks2::Job::completed, this, [this](bool success) {
                UDisks2::Job *job = qobject_cast<UDisks2::Job *>(sender());
                updatePartitionStatus(job, success);
            });
            m_jobsToWait.insert(path, job);
        }
    }

    //    object path "/org/freedesktop/UDisks2/block_devices/sda1"
    //    array [
    //       dict entry(
    //          string "org.freedesktop.UDisks2.Filesystem"

    // Register monitor for this
    //    signal time=1521817375.168670 sender=:1.83 -> destination=(null destination) serial=199
    //    path=/org/freedesktop/UDisks2/block_devices/sda1; interface=org.freedesktop.DBus.Properties; member=PropertiesChanged
    //       string "org.freedesktop.UDisks2.Block"

}

void UDisks2::Monitor::interfacesRemoved(const QDBusObjectPath &objectPath, const QStringList &interfaces)
{
    Q_UNUSED(interfaces)

    QString path = objectPath.path();
    if (m_jobsToWait.contains(path)) {
        UDisks2::Job *job = m_jobsToWait.take(path);
        job->deleteLater();
    } else if (m_blockDevices.contains(path)) {
        UDisks2::Block *block = m_blockDevices.take(path);
        block->deleteLater();
        if (externalBlockDevice(path)) {
            m_manager->refresh();
        }
    }
}

void UDisks2::Monitor::updatePartitionProperties(const UDisks2::Block *blockDevice)
{
    for (auto partition : m_manager->m_partitions) {
        if (partition->devicePath == blockDevice->device()) {
            QString label = blockDevice->idLabel();
            if (label.isEmpty()) {
                label = blockDevice->idUUID();
            }

            qCInfo(lcMemoryCardLog) << "Update block:" << blockDevice->device() << "pref:" << blockDevice->preferredDevice();
            qCInfo(lcMemoryCardLog) << "- drive:" << blockDevice->drive() << "dNumber:" << blockDevice->deviceNumber();
            qCInfo(lcMemoryCardLog) << "- id:" << blockDevice->id() << "size:" << blockDevice->size();
            qCInfo(lcMemoryCardLog) << "- isreadonly:" << blockDevice->isReadOnly() << "idtype:" << blockDevice->idType();
            qCInfo(lcMemoryCardLog) << "- idversion" << blockDevice->idVersion() << "idlabel" << blockDevice->idLabel();
            qCInfo(lcMemoryCardLog) << "- iduuid" << blockDevice->idUUID();

            partition->devicePath = blockDevice->device();
            partition->mountPath = blockDevice->mountPath();
            partition->deviceLabel = label;
            partition->filesystemType = blockDevice->idType();
            partition->readOnly = blockDevice->isReadOnly();
            partition->canMount = blockDevice->value(QStringLiteral("HintAuto")).toBool()
                    && !partition->filesystemType.isEmpty()
                    && m_manager->supportedFileSystems().contains(partition->filesystemType);
            partition->valid = true;

            m_manager->refresh(partition.data());
        }
    }
}

void UDisks2::Monitor::updatePartitionStatus(const UDisks2::Job *job, bool success)
{
    UDisks2::Job::Operation operation = job->operation();

    PartitionManagerPrivate::Partitions affectedPartions;
    lookupPartitions(affectedPartions, job->value(UDISKS2_JOB_KEY_OBJECTS).toStringList());

    if (operation == UDisks2::Job::Mount || operation == UDisks2::Job::Unmount) {
        for (auto partition : affectedPartions) {
            Partition::Status oldStatus = partition->status;

            if (success) {
                if (job->status() == UDisks2::Job::Added) {
                    partition->activeState = operation == UDisks2::Job::Mount ? QStringLiteral("activating") : QStringLiteral("deactivating");
                    partition->status = operation == UDisks2::Job::Mount ? Partition::Mounting : Partition::Unmounting;
                } else {
                    // Completed busy unmount job shall stay in mounted state.
                    if (job->deviceBusy() && operation == UDisks2::Job::Unmount)
                        operation = UDisks2::Job::Mount;

                    partition->activeState = operation == UDisks2::Job::Mount ? QStringLiteral("active") : QStringLiteral("inactive");
                    partition->status = operation == UDisks2::Job::Mount ? Partition::Mounted : Partition::Unmounted;
                }
            } else {
                partition->activeState = QStringLiteral("failed");
                partition->status = operation == UDisks2::Job::Mount ? Partition::Mounted : Partition::Unmounted;
            }

            partition->valid = true;
            partition->mountFailed = job->deviceBusy() ? false : !success;
            if (oldStatus != partition->status) {
                m_manager->refresh(partition.data());
            }
        }
    } else if (operation == UDisks2::Job::Format) {
        for (auto partition : affectedPartions) {
            Partition::Status oldStatus = partition->status;
            if (success) {
                if (job->status() == UDisks2::Job::Added) {
                    partition->activeState = QStringLiteral("inactive");
                    partition->status = Partition::Formatting;
                } else {
                    partition->activeState = QStringLiteral("inactive");
                    partition->status = Partition::Formatted;
                }
            } else {
                partition->activeState = QStringLiteral("failed");
                partition->status = Partition::Unmounted;
            }
            partition->valid = true;
            if (oldStatus != partition->status) {
                m_manager->refresh(partition.data());
            }
        }
    }
}

// Used in UDisks2 InterfacesAdded / InterfacesRemoved signals.
bool UDisks2::Monitor::externalBlockDevice(const QString &deviceName) const
{
    static const QRegularExpression externalBlockDevice(QStringLiteral("^/org/freedesktop/UDisks2/block_devices/%1$").arg(externalDevice));
    return externalBlockDevice.match(deviceName).hasMatch();
}

void UDisks2::Monitor::startMountOperation(const QString &dbusMethod, const QString &deviceName, QVariantHash arguments)
{

    Q_ASSERT(dbusMethod == UDISKS2_FILESYSTEM_MOUNT || dbusMethod == UDISKS2_FILESYSTEM_UNMOUNT);

    if (deviceName.isEmpty()) {
        qCCritical(lcMemoryCardLog) << "Cannot" << dbusMethod.toLower() << "without device name";
        return;
    }

    QDBusInterface udisks2Interface(UDISKS2_SERVICE,
                                    UDISKS2_BLOCK_DEVICE_PATH.arg(deviceName),
                                    UDISKS2_FILESYSTEM_INTERFACE,
                                    QDBusConnection::systemBus());

    QDBusPendingCall pendingCall = udisks2Interface.asyncCall(dbusMethod, arguments);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, [this, deviceName, dbusMethod](QDBusPendingCallWatcher *watcher) {
        if (watcher->isValid() && watcher->isFinished()) {
            if (dbusMethod == UDISKS2_FILESYSTEM_MOUNT) {
                emit status(deviceName, Partition::Mounted);
            } else {
                emit status(deviceName, Partition::Unmounted);
            }
        } else if (watcher->isError()) {
            QDBusError error = watcher->error();
            QByteArray errorData = error.name().toLocal8Bit();
            const char *errorCStr = errorData.constData();

            qCWarning(lcMemoryCardLog) << dbusMethod << "error:" << errorCStr;

            for (uint i = 0; i < sizeof(dbus_error_entries) / sizeof(ErrorEntry); i++) {
                if (strcmp(dbus_error_entries[i].dbusErrorName, errorCStr) == 0) {
                    if (dbusMethod == UDISKS2_FILESYSTEM_MOUNT) {
                        emit mountError(dbus_error_entries[i].errorCode);
                        break;
                    } else {
                        emit unmountError(dbus_error_entries[i].errorCode);
                        break;
                    }
                }
            }

            if (strcmp(UDISKS2_ERROR_ALREADY_UNMOUNTING, errorCStr) == 0) {
                // Do nothing
            } else if (strcmp(UDISKS2_ERROR_ALREADY_MOUNTED, errorCStr) == 0) {
                emit status(deviceName, Partition::Mounted);
            } else if (dbusMethod == UDISKS2_FILESYSTEM_UNMOUNT) {
                // All other errors will revert back the previous state.
                emit status(deviceName, Partition::Mounted);
            } else if (dbusMethod == UDISKS2_FILESYSTEM_MOUNT) {
                // All other errors will revert back the previous state.
                emit status(deviceName, Partition::Unmounted);
            }
        }

        watcher->deleteLater();
    });

    if (dbusMethod == UDISKS2_FILESYSTEM_MOUNT) {
        emit status(deviceName, Partition::Mounting);
    } else {
        emit status(deviceName, Partition::Unmounting);
    }
}

void UDisks2::Monitor::lookupPartitions(PartitionManagerPrivate::Partitions &affectedPartions, const QStringList &objects)
{
    for (const QString &object : objects) {
        QString deviceName = object.section(QChar('/'), 5);
        for (auto partition : m_manager->m_partitions) {
            if (partition->deviceName == deviceName) {
                affectedPartions << partition;
            }
        }
    }
}

void UDisks2::Monitor::addBlockDevice(const QString &path, const QVariantMap &dict)
{
    if (m_blockDevices.contains(path)) {
        return;
    }

    UDisks2::Block *block = new UDisks2::Block(path, dict);
    m_blockDevices.insert(path, block);
    if (block->hasData()) {
        updatePartitionProperties(block);
    }
    // When e.g. partition formatted, update partition info.
    connect(block, &UDisks2::Block::blockUpdated, this, [this]() {
        UDisks2::Block *block = qobject_cast<UDisks2::Block *>(sender());
        updatePartitionProperties(block);
    });

    connect(block, &UDisks2::Block::mountPathChanged, this, [this]() {
        UDisks2::Block *block = qobject_cast<UDisks2::Block *>(sender());

        // Both updatePartitionStatus and updatePartitionProperties
        // emits partition refresh => latter one is enough.

        m_manager->blockSignals(true);
        QVariantMap data;
        data.insert(UDISKS2_JOB_KEY_OPERATION, block->mountPath().isEmpty() ? UDISKS2_JOB_OP_FS_UNMOUNT : UDISKS2_JOB_OP_FS_MOUNT);
        data.insert(UDISKS2_JOB_KEY_OBJECTS, QStringList() << block->path());
        qCInfo(lcMemoryCardLog) << "New partition status:" << data;
        UDisks2::Job tmpJob(QString(), data);
        tmpJob.complete(true);
        updatePartitionStatus(&tmpJob, true);
        m_manager->blockSignals(false);

        updatePartitionProperties(block);

        if (!m_operationQueue.isEmpty()) {
            Operation op = m_operationQueue.head();
            if (op.command == QStringLiteral("format") && block->mountPath().isEmpty()) {
                m_operationQueue.dequeue();
                doFormat(op.deviceName, op.type, op.arguments);
            }
        }
    });
}

void UDisks2::Monitor::doFormat(const QString &deviceName, const QString &type, const QVariantHash &arguments)
{
    QDBusInterface blockDeviceInterface(UDISKS2_SERVICE,
                                    UDISKS2_BLOCK_DEVICE_PATH.arg(deviceName),
                                    UDISKS2_BLOCK_INTERFACE,
                                    QDBusConnection::systemBus());

    QDBusPendingCall pendingCall = blockDeviceInterface.asyncCall(UDISKS2_BLOCK_FORMAT, type, arguments);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, [this, deviceName](QDBusPendingCallWatcher *watcher) {
        if (watcher->isValid() && watcher->isFinished()) {
            emit status(deviceName, Partition::Formatted);
        } else if (watcher->isError()) {
            QDBusError error = watcher->error();
            QByteArray errorData = error.name().toLocal8Bit();
            const char *errorCStr = errorData.constData();
            qCWarning(lcMemoryCardLog) << "Format error:" << errorCStr;

            for (uint i = 0; i < sizeof(dbus_error_entries) / sizeof(ErrorEntry); i++) {
                if (strcmp(dbus_error_entries[i].dbusErrorName, errorCStr) == 0) {
                    emit formatError(dbus_error_entries[i].errorCode);
                    break;
                }
            }
        }
        watcher->deleteLater();
    });
}

void UDisks2::Monitor::getBlockDevices()
{
    QVector<Partition> partitions = m_manager->partitions(Partition::External | Partition::ExcludeParents);
    for (const Partition &partition : partitions) {
        QString path = UDISKS2_BLOCK_DEVICE_PATH.arg(partition.deviceName());
        QVariantMap data;
        addBlockDevice(path, data);
    }
}