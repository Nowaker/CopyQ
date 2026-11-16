// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "testinterface.h"

#include <QObject>

class ItemEvictionTests final : public QObject {
    Q_OBJECT
public:
    explicit ItemEvictionTests(const TestInterfacePtr &test = {}, QObject *parent = nullptr);
private slots:
    void underLimitEvictsNothing();
    void evictsHighestScoreFirstUntilUnderTarget();
    void keepsNewestRecentAndPinned();
    void noLimit();
    void binaryFormats();
    void withoutBinaryDataRecordsEvictedFormats();
    void usageOfUnsavedData();
};
