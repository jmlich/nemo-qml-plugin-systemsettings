/*
 * Copyright (C) 2013 Jolla Ltd. <pekka.vuorela@jollamobile.com>
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

#ifndef ABOUTSETTINGS_H
#define ABOUTSETTINGS_H

#include <QObject>
#include <QVariant>

class QStorageInfo;
class QNetworkInfo;
class QDeviceInfo;
class AboutSettings: public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString bluetoothAddress READ bluetoothAddress CONSTANT)
    Q_PROPERTY(QString wlanMacAddress READ wlanMacAddress CONSTANT)
    Q_PROPERTY(QString imei READ imei CONSTANT)
    Q_PROPERTY(QString serial READ serial CONSTANT)
    Q_PROPERTY(QString softwareVersion READ softwareVersion CONSTANT)
    Q_PROPERTY(QString softwareVersionId READ softwareVersionId CONSTANT)
    Q_PROPERTY(QString adaptationVersion READ adaptationVersion CONSTANT)

    Q_PROPERTY(QVariant internalStorageUsageModel READ diskUsageModel NOTIFY storageChanged)
    Q_PROPERTY(QVariant externalStorageUsageModel READ externalStorageUsageModel NOTIFY storageChanged)

public:
    explicit AboutSettings(QObject *parent = 0);
    virtual ~AboutSettings();

    // Deprecated -- use diskUsageModel() instead
    Q_INVOKABLE qlonglong totalDiskSpace() const;
    // Deprecated -- use diskUsageModel() instead
    Q_INVOKABLE qlonglong availableDiskSpace() const;

    /**
     * Returns a list of JS objects with the following keys:
     *  - storageType: one of "mass" (mass storage), "system" (system storage) or "user" (user storage)
     *  - path: filesystem path (e.g. "/" or "/home/")
     *  - available: available bytes on the storage
     *  - total: total bytes on the storage
     **/
    Q_INVOKABLE QVariant diskUsageModel() const;
    QVariant externalStorageUsageModel() const;
    Q_INVOKABLE void refreshStorageModels();

    QString bluetoothAddress() const;
    QString wlanMacAddress() const;
    QString imei() const;
    QString serial() const;
    QString softwareVersion() const;
    QString softwareVersionId() const;
    QString adaptationVersion() const;

signals:
    void storageChanged();

private:
    QStorageInfo *m_sysinfo;
    QNetworkInfo *m_netinfo;
    QDeviceInfo *m_devinfo;

    QVariantList m_internalStorage;
    QVariantList m_externalStorage;
    mutable QMap<QString, QString> m_osRelease;
    mutable QMap<QString, QString> m_hardwareRelease;
};

#endif
