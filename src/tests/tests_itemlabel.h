// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "testinterface.h"

#include <QObject>

class ItemLabelTests final : public QObject {
    Q_OBJECT
public:
    explicit ItemLabelTests(const TestInterfacePtr &test = {}, QObject *parent = nullptr);
private slots:
    void pngLabel();
    void gifLabel();
    void jpegLabel();
    void otherImageLabel();
    void pdfLabel();
    void uriListLabel();
    void objectLabel();
    void evictedLabel();
    void byteCounts();
};
