// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "testinterface.h"

#include <QObject>

#include <memory>

class SyncServer;

/**
 * Tests for syncing with Flycut through flycut-sync-server.
 *
 * The tests marked as server tests run against a real flycut-sync-server binary
 * (build it from the Flycut repository's sync-server directory with `go build`),
 * given by the FLYCUT_SYNC_SERVER environment variable; without it they are skipped.
 */
class FlycutSyncTests final : public QObject {
    Q_OBJECT
public:
    explicit FlycutSyncTests(const TestInterfacePtr &test, QObject *parent = nullptr);

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void cryptoMatchesBridgeAndServer();
    void cryptoRejectsAlteredData();
    void eventOrdering();
    void rankPlacement();
    void migrationTakesOverBridgeState();

    // server tests
    void sharesLocalChanges();
    void appliesOtherMachinesChanges();
    void sharesUserRemovalsOnly();
    void continuesBridgeHistory();
    void searchesArchiveAndOpensHistory();
    void refetchesEvictedFormats();

private:
    TestInterfacePtr m_test;
    std::shared_ptr<SyncServer> m_server;
};
