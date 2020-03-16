/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Copyright (c) 2014 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#ifndef OPFLEXAGENT_TEST_MOCKSWITCHCONNECTION_H_
#define OPFLEXAGENT_TEST_MOCKSWITCHCONNECTION_H_

#include "SwitchConnection.h"
#include "ovs-ofputil.h"

namespace opflexagent {

/**
 * Mock switch connection object useful for tests
 */
class MockSwitchConnection : public SwitchConnection {
public:
    MockSwitchConnection()
        : SwitchConnection("mockBridge"), connected(false) {
    }
    virtual ~MockSwitchConnection() {
        clear();
    }

    virtual void clear() {
        std::lock_guard<std::mutex> guard(sentMsgMutex);
        sentMsgs.clear();
    }

    virtual int Connect(int protoVer) {
        connected = true;
        notifyConnectListeners();
        return 0;
    }

    virtual int GetProtocolVersion() { return OFP13_VERSION; }

    virtual int SendMessage(OfpBuf& msg) {
        std::lock_guard<std::mutex> guard(sentMsgMutex);
        sentMsgs.push_back(std::move(msg));
        return 0;
    }

    virtual bool IsConnected() { return connected; }

    bool connected;

    int getSentMsgCount() {
        std::lock_guard<std::mutex> guard(sentMsgMutex);
        return sentMsgs.size();
    }

    const OfpBuf& getSentMsg(int index) {
        std::lock_guard<std::mutex> guard(sentMsgMutex);
        return sentMsgs[index];
    }

    const std::vector<OfpBuf>& getSentMsgs() {
        std::lock_guard<std::mutex> guard(sentMsgMutex);
        return sentMsgs;
    }

private:
    std::vector<OfpBuf> sentMsgs;
    std::mutex sentMsgMutex;
};

} // namespace opflexagent

#endif /* OPFLEXAGENT_TEST_MOCKSWITCHCONNECTION_H_ */
